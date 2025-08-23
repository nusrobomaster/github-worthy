// ===== WPT Charger (Peripheral/Server) =====
// 1-button: short press -> pairing window 30s
// Accepts new bonds only while window open; otherwise bonded-only.
// One connection max; 10s same-peer cooldown.
// Advertises 1-byte manufacturer data: 0=normal, 1=pairing window.

#include <Arduino.h>
#include <NimBLEDevice.h>

static const char* DEVICE_NAME = "WPT_Charger";

// ---- Pins ----
const int PIN_PAIR_BTN = 0;            // change to your board

// ---- Timers ----
const uint32_t PAIR_WINDOW_MS        = 30000;   // 30 s pairing window
const uint32_t RECONNECT_COOLDOWN_MS = 10000;   // 10 s same-peer cooldown

// ---- GATT UUIDs ----
#define SVC_UUID   "180F"
#define CTRL_UUID  "2A19"   // Write No Response (Device->Charger), Notify optional

// ---- Globals ----
NimBLEServer*          gServer        = nullptr;
NimBLEAdvertising*     gAdv           = nullptr;
NimBLECharacteristic*  gCtrl          = nullptr;

volatile bool   g_pairMode      = false;
uint32_t        g_pairModeUntil = 0;

bool            g_connected     = false;
bool            g_hasLastPeer   = false;
NimBLEAddress   g_lastPeer;
uint32_t        g_lastDiscMs    = 0;

uint32_t        g_passkey       = 123456; // not used for Numeric Comparison; harmless

// --- Advertise 1 byte: 1=pairing open, 0=closed ---
static void updatePairFlag() {
  if (!gAdv) return;
  std::string md(1, (g_pairMode && (millis() < g_pairModeUntil)) ? char(1) : char(0));
  gAdv->setManufacturerData(md);
}

static void startAdvertising(bool fast = true) {
  if (!gAdv) return;
  gAdv->stop();

  if (fast) { gAdv->setMinInterval(48);  gAdv->setMaxInterval(80);  }  // 30–50 ms
  else       { gAdv->setMinInterval(160); gAdv->setMaxInterval(160); } // 100 ms

  updatePairFlag();
  gAdv->start();
  Serial.println("[Charger] Advertising...");
}

class CtrlCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    const std::string& d = c->getValue();
    Serial.printf("[Charger] RX %u bytes from %s\n",
                  (unsigned)d.size(), connInfo.getAddress().toString().c_str());
    // TODO: act on control frame
  }
} ctrlCB;

class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& connInfo) override {
    const NimBLEAddress peer = connInfo.getAddress();
    const bool bonded = NimBLEDevice::isBonded(peer);
    const bool windowOpen = g_pairMode && (millis() < g_pairModeUntil);

    if (g_connected) { s->disconnect(connInfo.getConnHandle()); return; }

    if (!bonded && !windowOpen) { // bonded-only outside pairing window
      Serial.printf("[Charger] Reject unbonded %s (pair window closed)\n", peer.toString().c_str());
      s->disconnect(connInfo.getConnHandle()); return;
    }

    if (g_hasLastPeer && peer.equals(g_lastPeer) &&
        (millis() - g_lastDiscMs) < RECONNECT_COOLDOWN_MS) {
      Serial.printf("[Charger] %s within cooldown -> disconnect.\n", peer.toString().c_str());
      s->disconnect(connInfo.getConnHandle()); return;
    }

    g_connected = true;
    Serial.printf("[Charger] Connected: %s bonded=%d\n", peer.toString().c_str(), bonded);

    s->updateConnParams(connInfo.getConnHandle(), 6, 12, 0, 50); // 7.5–15 ms, 0, 500 ms
    if (gAdv) gAdv->stop(); // no second client
  }

  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& connInfo, int reason) override {
    (void)s; (void)reason;
    g_connected   = false;
    g_lastPeer    = connInfo.getAddress();
    g_hasLastPeer = true;
    g_lastDiscMs  = millis();
    Serial.printf("[Charger] Disconnected from %s\n", g_lastPeer.toString().c_str());
    startAdvertising(true);
  }

  void onMTUChange(uint16_t MTU, NimBLEConnInfo& connInfo) override {
    Serial.printf("[Charger] MTU -> %u from %s\n", MTU, connInfo.getAddress().toString().c_str());
  }

  // Numeric Comparison auto-confirm
  void onConfirmPassKey(NimBLEConnInfo& connInfo, uint32_t pass_key) override {
    Serial.printf("[Charger] Compare: %06u (auto-accept)\n", pass_key);
    NimBLEDevice::injectConfirmPasskey(connInfo, true);
  }

  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    if (!connInfo.isEncrypted()) {
      Serial.println("[Charger] Encryption failed -> disconnect");
      NimBLEDevice::getServer()->disconnect(connInfo.getConnHandle()); return;
    }
    Serial.printf("[Charger] Secured, bonded=%d\n", connInfo.isBonded());
  }
} serverCB;

void setup() {
  pinMode(PIN_PAIR_BTN, INPUT_PULLUP);
  Serial.begin(115200);
  delay(50);

  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setSecurityAuth(true, true, true);           // bond+MITM+SC
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_YESNO);   // Numeric Comparison
  NimBLEDevice::setSecurityPasskey(g_passkey);               // unused for NC

  gServer = NimBLEDevice::createServer();
  gServer->setCallbacks(&serverCB);

  NimBLEService* svc = gServer->createService(SVC_UUID);
  gCtrl = svc->createCharacteristic(CTRL_UUID,
            NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  gCtrl->setCallbacks(&ctrlCB);
  svc->start();

  gAdv = NimBLEDevice::getAdvertising();
  gAdv->addServiceUUID(SVC_UUID);
  startAdvertising(true);
}

void loop() {
  static bool prev = true;
  bool now = (digitalRead(PIN_PAIR_BTN) == LOW);

  // short press: open 30s pairing window
  if (now && !prev) {
    g_pairMode = true;
    g_pairModeUntil = millis() + PAIR_WINDOW_MS;
    updatePairFlag();
    Serial.println("[Charger] Pairing window OPEN (30 s)");
  }
  if (g_pairMode && millis() > g_pairModeUntil) {
    g_pairMode = false;
    updatePairFlag();
    Serial.println("[Charger] Pairing window CLOSED");
  }
  prev = now;

  // Optional: slow ADV after idle
  static uint32_t t0 = millis();
  if (!g_connected && (millis() - t0) > 8000) { startAdvertising(false); t0 = millis(); }

  delay(10);
}

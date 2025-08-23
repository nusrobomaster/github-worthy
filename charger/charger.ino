/**
 * WPT Charger - NimBLE Peripheral (Server)
 * - Bonding via Numeric Comparison during a physical pairing window
 * - Rejects unbonded peers otherwise
 * - One connection only, 10 s same-peer cooldown after disconnect
 */

#include <Arduino.h>
#include <NimBLEDevice.h>

static const char* DEVICE_NAME = "WPT_Charger";

// ---- Pins ----
const int PIN_PAIR_BTN = 0;            // press to open pairing window

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

volatile bool   g_pairMode       = false;
uint32_t        g_pairModeUntil  = 0;

bool            g_connected      = false;
bool            g_hasLastPeer    = false;
NimBLEAddress   g_lastPeer;
uint32_t        g_lastDiscMs     = 0;

uint32_t        g_passkey        = 123456;      // used only if you swap to Passkey method

// --- Advertising helper ---
static void startAdvertising(bool fast = true) {
  if (!gAdv) return;
  gAdv->stop();

  if (fast) {
    gAdv->setMinInterval(48);  // 30ms (0.625ms units)
    gAdv->setMaxInterval(80);  // 50ms
  } else {
    gAdv->setMinInterval(160); // 100ms
    gAdv->setMaxInterval(160);
  }

  gAdv->start();
  Serial.println("[Charger] Advertising...");
}

// --- Control characteristic callback ---
class CtrlCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    const std::string& d = c->getValue();
    Serial.printf("[Charger] RX %u bytes from %s\n",
                  (unsigned)d.size(), connInfo.getAddress().toString().c_str());
    // TODO: parse & act on control data here
  }
} ctrlCB;

// --- Server callbacks (includes security hooks in this NimBLE build) ---
class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& connInfo) override {
    const NimBLEAddress peer = connInfo.getAddress();
    const bool bonded = NimBLEDevice::isBonded(peer);
    const bool windowOpen = g_pairMode && (millis() < g_pairModeUntil);

    // Enforce single connection
    if (g_connected) {
      Serial.println("[Charger] Already connected; dropping new attempt.");
      s->disconnect(connInfo.getConnHandle());
      return;
    }

    // Reject non-bonded peers unless pairing window is open
    if (!bonded && !windowOpen) {
      Serial.printf("[Charger] Rejecting unbonded %s\n", peer.toString().c_str());
      s->disconnect(connInfo.getConnHandle());
      return;
    }

    // Enforce same-peer cooldown
    if (g_hasLastPeer && peer.equals(g_lastPeer) &&
        (millis() - g_lastDiscMs) < RECONNECT_COOLDOWN_MS) {
      Serial.printf("[Charger] %s within cooldown -> disconnect.\n", peer.toString().c_str());
      s->disconnect(connInfo.getConnHandle());
      return;
    }

    g_connected = true;
    Serial.printf("[Charger] Connected: %s  bonded=%d\n",
                  peer.toString().c_str(), bonded);

    // Tight connection params: CI 7.5–15 ms, latency 0, timeout 500 ms
    s->updateConnParams(connInfo.getConnHandle(), 6, 12, 0, 50);

    // Stop advertising while connected to avoid other attempts
    if (gAdv) gAdv->stop();
  }

  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& connInfo, int reason) override {
    (void)s; (void)reason;
    g_connected   = false;
    g_lastPeer    = connInfo.getAddress();
    g_hasLastPeer = true;
    g_lastDiscMs  = millis();

    Serial.printf("[Charger] Disconnected from %s. Cooldown 10s.\n",
                  g_lastPeer.toString().c_str());

    startAdvertising(true);
  }

  void onMTUChange(uint16_t MTU, NimBLEConnInfo& connInfo) override {
    Serial.printf("[Charger] MTU -> %u from %s\n",
                  MTU, connInfo.getAddress().toString().c_str());
  }

  // --- Security (Numeric Comparison path) ---
  void onConfirmPassKey(NimBLEConnInfo& connInfo, uint32_t pass_key) override {
    Serial.printf("[Charger] Numeric comparison: %06u (auto-accept here)\n", pass_key);
    // TODO: replace with a real UI/button confirmation
    NimBLEDevice::injectConfirmPasskey(connInfo, true);
  }

  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    if (!connInfo.isEncrypted()) {
      Serial.println("[Charger] Encryption failed -> disconnecting");
      NimBLEDevice::getServer()->disconnect(connInfo.getConnHandle());
      return;
    }
    Serial.printf("[Charger] Secured link to %s  bonded=%d\n",
                  connInfo.getAddress().toString().c_str(), connInfo.isBonded());
  }
} serverCB;

void setup() {
  pinMode(PIN_PAIR_BTN, INPUT_PULLUP);
  Serial.begin(115200);
  delay(50);

  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setMTU(247);

  // Security: Bond + MITM + LE Secure Connections (Numeric Comparison)
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_YESNO);
  NimBLEDevice::setSecurityPasskey(g_passkey);

  // GATT
  gServer = NimBLEDevice::createServer();
  gServer->setCallbacks(&serverCB);

  NimBLEService* svc = gServer->createService(SVC_UUID);
  gCtrl = svc->createCharacteristic(CTRL_UUID,
            NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  gCtrl->setCallbacks(&ctrlCB);
  svc->start();

  // Advertising
  gAdv = NimBLEDevice::getAdvertising();
  gAdv->addServiceUUID(SVC_UUID);
  startAdvertising(true);
}

void loop() {
  // Pairing window: open on button press; auto-closes after 30 s
  static bool was = false;
  bool now = (digitalRead(PIN_PAIR_BTN) == LOW);
  if (now && !was) {
    g_pairMode = true;
    g_pairModeUntil = millis() + PAIR_WINDOW_MS;
    Serial.println("[Charger] Pairing window OPEN (30 s)");
  }
  if (g_pairMode && millis() > g_pairModeUntil) {
    g_pairMode = false;
    Serial.println("[Charger] Pairing window CLOSED");
  }
  was = now;

  // (Optional) slow advertising after ~8 s idle
  static uint32_t t0 = millis();
  if (!g_connected && (millis() - t0) > 8000) {
    startAdvertising(false);
    t0 = millis();
  }

  delay(10);
}

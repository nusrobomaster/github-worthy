// ===== WPT Device (Central/Client) =====
// 1-button:
//  - short press: toggle TX on/off (connect/disconnect)
//  - long press (>=1.5s): enter pairing-attempt mode for 30s
//    -> will connect to unknown chargers only if their adv "pair flag" == 1
// Remembers bonded charger addresses in NVS Preferences.

#include <Arduino.h>
#include <Preferences.h>
#include <NimBLEDevice.h>

static const char* DEVICE_NAME = "WPT_Device";

// ---- Pins ----
const int PIN_USER_BTN = 0;

// ---- Press timings ----
const uint32_t LONG_PRESS_MS   = 1500;
const uint32_t PAIR_ATTEMPT_MS = 30000;

// ---- GATT UUIDs ----
#define SVC_UUID  "180F"
#define CTRL_UUID "2A19"

// ---- State ----
NimBLEClient*               gClient = nullptr;
NimBLERemoteService*        gSvc    = nullptr;
NimBLERemoteCharacteristic* gCtrl   = nullptr;

bool         g_txEnabled   = false;   // set by short press
bool         g_pairAttempt = false;   // set by long press
uint32_t     g_pairUntil   = 0;

bool         g_connected   = false;
bool         g_scanning    = false;
bool         g_haveTarget  = false;
NimBLEAddress g_targetAddr;

uint32_t g_passkey = 123456;

Preferences prefs;

// ---- Remember bonded chargers (addresses as CSV) ----
bool isRemembered(const NimBLEAddress& a) {
  prefs.begin("wpt", true);
  String list = prefs.getString("peers", "");
  prefs.end();
  String t = String(a.toString().c_str());
  t.toUpperCase(); list.toUpperCase();
  return list.indexOf(t) >= 0;
}
void rememberBond(const NimBLEAddress& a) {
  prefs.begin("wpt", false);
  String list = prefs.getString("peers", "");
  String addr = String(a.toString().c_str());
  if (list.length() == 0) list = addr;
  else if (list.indexOf(addr) < 0) list += "," + addr;
  prefs.putString("peers", list);
  prefs.end();
}

// ---- Client callbacks (simple signatures) ----
class ClientCB : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* c) override {
    Serial.println("[Device] Connected");
    c->updateConnParams(6, 12, 0, 50); // 7.5–15 ms, 0, 500 ms
  }
  void onDisconnect(NimBLEClient* c, int reason) override {
    (void)c; Serial.printf("[Device] Disconnected, reason=%d\n", reason);
    g_connected  = false;
    g_haveTarget = false;
  }
  // Numeric Comparison auto-confirm (lowercase k in this API)
  void onConfirmPasskey(NimBLEConnInfo& ci, uint32_t pin) override {
    Serial.printf("[Device] Compare: %06u (auto-accept)\n", pin);
    NimBLEDevice::injectConfirmPasskey(ci, true);
  }
  void onAuthenticationComplete(NimBLEConnInfo& ci) override {
    if (!ci.isEncrypted()) {
      Serial.println("[Device] Encryption failed -> disconnect");
      NimBLEDevice::getClientByHandle(ci.getConnHandle())->disconnect();
      return;
    }
    if (ci.isBonded()) rememberBond(ci.getAddress());
    Serial.printf("[Device] Secured, bonded=%d\n", ci.isBonded());
  }
} clientCB;

// ---- Scanner (don’t connect in callback; just queue) ----
class ScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* adv) override {
    if (g_connected || g_haveTarget) return;
    if (!g_txEnabled) return; // only when TX is enabled
    if (!adv->isAdvertisingService(NimBLEUUID(SVC_UUID))) return;

    // Read charger pairing flag from manufacturer data (1 byte)
    bool pairingFlag = false;
    const std::string md = adv->getManufacturerData();
    if (!md.empty()) pairingFlag = ((uint8_t)md[0] == 1);

    const NimBLEAddress addr = adv->getAddress();
    const bool known = isRemembered(addr);

    // Rule: connect if known; OR if in pairAttempt window AND charger says pairing open
    if (!known) {
      if (!(g_pairAttempt && (millis() < g_pairUntil) && pairingFlag)) return;
    }

    g_targetAddr = addr;
    g_haveTarget = true;
    Serial.printf("[Device] Found charger %s (known=%d pairing=%d) -> queue connect\n",
                  addr.toString().c_str(), known, pairingFlag);

    NimBLEDevice::getScan()->stop();
    g_scanning = false;
  }
} scanCB;

static inline void startScanFast() {
  if (g_scanning) return;
  NimBLEScan* s = NimBLEDevice::getScan();
  s->setScanCallbacks(&scanCB, false); // no duplicates stored
  s->setActiveScan(true);
  s->setInterval(48); // 30 ms
  s->setWindow(48);   // 30 ms
  s->setMaxResults(0);
  s->start(0, false, true);           // forever
  g_scanning = true;
  Serial.println("[Device] Scanning...");
}
static inline void stopScan() {
  if (!g_scanning) return;
  NimBLEDevice::getScan()->stop();
  g_scanning = false;
}

// Connect outside callbacks
bool connectToTarget() {
  if (!g_haveTarget) return false;

  if (!gClient) {
    gClient = NimBLEDevice::createClient();
    gClient->setClientCallbacks(&clientCB);
    gClient->setConnectTimeout(5);
    gClient->setConnectionParams(6, 12, 0, 50);
  }

  Serial.printf("[Device] Connecting to %s ...\n", g_targetAddr.toString().c_str());
  if (!gClient->connect(g_targetAddr)) {
    Serial.println("[Device] Connect failed; will rescan");
    g_haveTarget = false; // forget and rescan (don’t delete client here)
    return false;
  }

  g_connected = true;
  Serial.println("[Device] Connected; securing link...");
  gClient->secureConnection(); // triggers pairing if needed

  gSvc = gClient->getService(SVC_UUID);
  if (!gSvc) { Serial.println("[Device] Service missing; disconnect."); gClient->disconnect(); g_connected=false; g_haveTarget=false; return false; }

  gCtrl = gSvc->getCharacteristic(CTRL_UUID);
  if (!gCtrl || !gCtrl->canWriteNoResponse()) {
    Serial.println("[Device] CTRL missing or no WNR; disconnect.");
    gClient->disconnect(); g_connected=false; g_haveTarget=false; return false;
  }

  Serial.println("[Device] Ready to stream control frames.");
  return true;
}

void setup() {
  pinMode(PIN_USER_BTN, INPUT_PULLUP);
  Serial.begin(115200);
  delay(50);

  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setMTU(247);

  NimBLEDevice::setSecurityAuth(true, true, true);          // bond+MITM+SC
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_YESNO);  // Numeric Comparison
  NimBLEDevice::setSecurityPasskey(g_passkey);
}

void loop() {
  static bool btnPrev = true;
  static uint32_t tPress = 0;

  bool btn = (digitalRead(PIN_USER_BTN) == LOW);

  // Edge detection
  if (btn && !btnPrev) tPress = millis();
  if (!btn && btnPrev) {
    uint32_t dur = millis() - tPress;
    if (dur >= LONG_PRESS_MS) {
      // LONG press: enter pairing-attempt window (30s)
      g_pairAttempt = true;
      g_pairUntil   = millis() + PAIR_ATTEMPT_MS;
      Serial.println("[Device] Pairing-attempt window OPEN (30 s)");
      if (!g_connected && g_txEnabled && !g_scanning) startScanFast();
    } else {
      // SHORT press: toggle TX on/off
      g_txEnabled = !g_txEnabled;
      Serial.printf("[Device] TX %s\n", g_txEnabled ? "ON" : "OFF");
      if (g_txEnabled) {
        if (!g_connected && !g_haveTarget) startScanFast();
      } else {
        stopScan();
        if (g_connected && gClient) { gClient->disconnect(); g_connected=false; }
        g_haveTarget = false;
      }
    }
  }
  btnPrev = btn;

  // Expire pairing-attempt window
  if (g_pairAttempt && millis() > g_pairUntil) {
    g_pairAttempt = false;
    Serial.println("[Device] Pairing-attempt window CLOSED");
  }

  // If we queued a target from scan, connect now
  if (!g_connected && g_haveTarget) {
    if (!connectToTarget()) {
      // connection failed; restart scan if TX is still on (short press toggled on)
      if (g_txEnabled) startScanFast();
    }
  }

  // Stream small frames at high rate while connected and TX on
  if (g_connected && gCtrl && g_txEnabled) {
    static uint32_t tLast = 0; uint32_t now = micros();
    if (now - tLast >= 3300) { // ~300 Hz
      tLast = now;
      uint8_t pkt[8];
      pkt[0]=0xA5; pkt[1]=0x5A;
      uint32_t ts = millis(); memcpy(&pkt[2], &ts, 4);
      pkt[6]=0; pkt[7]=0;
      gCtrl->writeValue(pkt, sizeof(pkt), /*response=*/false);
    }
  }

  delay(1);
}

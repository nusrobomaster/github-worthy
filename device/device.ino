#include <Arduino.h>
#include <NimBLEDevice.h>

static const char* DEVICE_NAME = "WPT_Device";
const int PIN_USER_BTN = 0;

#define SVC_UUID  "180F"
#define CTRL_UUID "2A19"

NimBLEClient*               gClient = nullptr;
NimBLERemoteService*        gSvc    = nullptr;
NimBLERemoteCharacteristic* gCtrl   = nullptr;

bool         g_connected   = false;
bool         g_scanning    = false;
bool         g_haveTarget  = false;
NimBLEAddress g_targetAddr;

uint32_t g_passkey = 123456;

// ---------- Client callbacks (simple signatures for your build) ----------
class ClientCB : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* c) override {
    Serial.println("[Device] Connected");
    c->updateConnParams(6, 12, 0, 50); // CI 7.5–15 ms, latency 0, timeout 500 ms
  }
  void onDisconnect(NimBLEClient* c, int reason) override {
    (void)c;
    Serial.printf("[Device] Disconnected, reason=%d\n", reason);
    g_connected  = false;
    g_haveTarget = false; // forget target so we can rescan
  }
  // Numeric Comparison (lowercase 'k' in this API)
  void onConfirmPasskey(NimBLEConnInfo& connInfo, uint32_t pass_key) override {
    Serial.printf("[Device] Compare: %06u (auto-accept)\n", pass_key);
    NimBLEDevice::injectConfirmPasskey(connInfo, true);
  }
  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    if (!connInfo.isEncrypted()) {
      Serial.println("[Device] Encryption failed -> disconnect");
      NimBLEDevice::getClientByHandle(connInfo.getConnHandle())->disconnect();
      return;
    }
    Serial.printf("[Device] Secured, bonded=%d\n", connInfo.isBonded());
  }
} clientCB;

// ---------- Scan callbacks (lightweight) ----------
class ScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* adv) override {
    if (g_connected || g_haveTarget) return;
    if (!adv->isAdvertisingService(NimBLEUUID(SVC_UUID))) return;

    g_targetAddr = adv->getAddress(); // copy address (don’t hold volatile pointers)
    g_haveTarget = true;

    Serial.printf("[Device] Found charger %s RSSI=%d (queuing connect)\n",
                  g_targetAddr.toString().c_str(), adv->getRSSI());

    NimBLEDevice::getScan()->stop(); // stop scanning; connect later in loop()
    g_scanning = false;
  }
} scanCB;

static inline void startScanFast() {
  if (g_scanning) return;
  NimBLEScan* s = NimBLEDevice::getScan();
  s->setScanCallbacks(&scanCB, false); // no duplicates
  s->setActiveScan(true);
  s->setInterval(48); // 30 ms
  s->setWindow(48);   // 30 ms
  s->setMaxResults(0);
  s->start(0, false, true);           // forever, not-continue, restart fresh
  g_scanning = true;
  Serial.println("[Device] Scanning...");
}
static inline void stopScan() {
  if (!g_scanning) return;
  NimBLEDevice::getScan()->stop();
  g_scanning = false;
}

// ---------- Safe connect routine (called from loop, not from callback) ----------
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
    // Do NOT delete client here; just reuse it next time.
    g_haveTarget = false;
    return false;
  }

  g_connected = true;
  Serial.println("[Device] Connected; securing link...");
  gClient->secureConnection(); // triggers pairing/bonding if needed

  gSvc = gClient->getService(SVC_UUID);
  if (!gSvc) {
    Serial.println("[Device] Service not found; disconnect.");
    gClient->disconnect(); g_connected = false; g_haveTarget = false;
    return false;
  }

  gCtrl = gSvc->getCharacteristic(CTRL_UUID);
  if (!gCtrl || !gCtrl->canWriteNoResponse()) {
    Serial.println("[Device] CTRL char missing or no WNR; disconnect.");
    gClient->disconnect(); g_connected = false; g_haveTarget = false;
    return false;
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
  NimBLEDevice::setMTU(247); // global MTU

  // Security: Bond + MITM + LE Secure Connections (Numeric Comparison)
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_YESNO);
  NimBLEDevice::setSecurityPasskey(g_passkey);
}

void loop() {
  static bool wasPressed = false;
  const bool pressed = (digitalRead(PIN_USER_BTN) == LOW);

  // Press -> start scan (if not already connected/target queued)
  if (pressed && !wasPressed && !g_connected && !g_haveTarget) {
    startScanFast();
  }

  // If we have a target queued by the scan callback, connect now (outside callback)
  if (!g_connected && g_haveTarget) {
    if (!connectToTarget()) {
      // connection failed; restart scan if still pressing
      if (pressed) startScanFast();
    }
  }

  // Release -> stop scanning / disconnect
  if (!pressed && wasPressed) {
    stopScan();
    if (g_connected && gClient) {
      gClient->disconnect();
      g_connected = false;
      Serial.println("[Device] Disconnected (button release).");
    }
    g_haveTarget = false; // clear any queued target
  }
  wasPressed = pressed;

  // Stream small control frames at high rate while connected
  if (g_connected && gCtrl) {
    static uint32_t tLast = 0;
    uint32_t now = micros();
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

#include <Arduino.h>
#include <NimBLEDevice.h>


// ---- top of NimBLE_Secure_Server.ino ----
bool isLockedDown = false; // set true to lock, false to allow new bonds

// --- Button → toggle lockdown mode (interrupt + debounce) ---
// Uses INPUT_PULLUP on PIN_PAIR_BTN; falling edge = press.
// NOTE: don’t hold GPIO0 low during reset on ESP32 (boot strap).

// --- Button → toggle lockdown mode (interrupt, debounce in loop) ---
const int PIN_PAIR_BTN = 0;
const uint32_t DEBOUNCE_MS = 50;

volatile bool gEdge = false;
uint32_t gLastToggleMs = 0;

void IRAM_ATTR onPairButtonISR() {
  gEdge = true;                  // ISR only sets a flag
}

void setup_button_toggle() {
  pinMode(PIN_PAIR_BTN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_PAIR_BTN), onPairButtonISR, FALLING);
}

void update_security_mode() {
  if (!gEdge) return;
  gEdge = false;

  // debounce in task context
  uint32_t now = millis();
  if (now - gLastToggleMs < DEBOUNCE_MS) return;
  if (digitalRead(PIN_PAIR_BTN) == LOW) {       // confirm it really was a press
    gLastToggleMs = now;
    isLockedDown = !isLockedDown;
    applyModeToSecurityAndAdv();
    NimBLEDevice::getAdvertising()->start(0);
    Serial.printf("Mode: %s\n", isLockedDown ? "LOCKDOWN" : "PAIRING");
  }
  if (NimBLEDevice::getServer()->getConnectedCount()) {
    for (auto h : NimBLEDevice::getServer()->getPeerDevices()) {
      NimBLEDevice::getServer()->disconnect(h, BLE_ERR_REM_USER_CONN_TERM);
    }
  }
  Serial.println("Clients disconnected");
}



static void buildWhitelistFromBonds() {
  // SAFELY clear: snapshot current WL then remove each once.
  const size_t wlCount = NimBLEDevice::getWhiteListCount();                                    
  for (size_t i = 0; i < wlCount; ++i) {
    NimBLEAddress a = NimBLEDevice::getWhiteListAddress(i);                                    
    NimBLEDevice::whiteListRemove(a);                                                          
  }

  // Rebuild from the bond table.
  for (int i = 0; i < (int)NimBLEDevice::getNumBonds(); ++i) {                                 
    NimBLEDevice::whiteListAdd(NimBLEDevice::getBondedAddress(i));                             
  }
}

static void applyModeToSecurityAndAdv() {
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  const bool wasAdv = adv->isAdvertising();
  if (wasAdv) adv->stop();                             

  // Keep scan response available and flip the advertised name
  adv->enableScanResponse(true);                                                               

  if (isLockedDown) {
    buildWhitelistFromBonds();                                  
    NimBLEDevice::setDeviceName("TEST_LOCKED");                                                
    adv->setName("TEST_LOCKED");                                                              
  } else {
    // no WL enforcement in pairing mode                  
    NimBLEDevice::setDeviceName("TEST_PAIR");                                                 
    adv->setName("TEST_PAIR");                                                                
  }

  // Push new ADV/scan-response payloads to the controller
  adv->refreshAdvertisingData();                                                               

  if (wasAdv) adv->start(0);
}


class ServerCallbacks : public NimBLEServerCallbacks {
 public:
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    Serial.println("Client connected");
    if (isLockedDown) {
      // Kick off re-encryption / key resolution immediately.
      NimBLEDevice::startSecurity(connInfo.getConnHandle());
    }
    // Advertising stops automatically on connect; don't restart here.
  }

  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    Serial.printf("Client disconnected (reason=%d), restarting advertising\n", reason);
    // Safe helper to resume advertising with current settings:
    NimBLEDevice::startAdvertising();
  }

  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    if (!connInfo.isEncrypted()) {
      Serial.println("Auth failed - disconnecting");
      NimBLEDevice::getServer()->disconnect(connInfo, BLE_ERR_REM_USER_CONN_TERM);
    } else {
      Serial.println("Auth OK");
    }
  }
  
  void onIdentity(NimBLEConnInfo& info) override {
    if (!isLockedDown) return;

    // After the stack resolves the peer’s identity, gate it here:
    NimBLEAddress idAddr = info.getIdAddress();  // identity address (not the current RPA)
    if (!NimBLEDevice::isBonded(idAddr)) {
      Serial.println("Lockdown: unbonded peer -> disconnect");
      NimBLEDevice::getServer()->disconnect(info, BLE_ERR_REM_USER_CONN_TERM);
    }
  }

  void onMTUChange(uint16_t MTU, NimBLEConnInfo& connInfo) override {
    Serial.printf("MTU updated to %u\n", MTU);
  }
};

void setup() {
  setup_button_toggle();



  Serial.begin(115200);
  Serial.println("Starting NimBLE Server");

  // 1) Set the GAP device name (shows up as the peripheral's name characteristic)
  NimBLEDevice::init("TEST_PAIR");               
  NimBLEDevice::setDeviceName("TEST_PAIR");      
  NimBLEDevice::setPower(3); // +3 dB



  // Security: bonding + MITM + passkey
  NimBLEDevice::setSecurityAuth(true, /*MITM*/true, /*SC*/false);   
  NimBLEDevice::setSecurityPasskey(123456);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);

  NimBLEServer* pServer = NimBLEDevice::createServer();
  static ServerCallbacks serverCallbacks;
  pServer->setCallbacks(&serverCallbacks);

  // Your installed NimBLE has advertiseOnDisconnect(bool):
  pServer->advertiseOnDisconnect(true); // also handled by our onDisconnect
  // (API exists per class declaration.) 

  NimBLEService* pService = pServer->createService("ABCD");

  NimBLECharacteristic* pNonSecure =
      pService->createCharacteristic("1234", NIMBLE_PROPERTY::READ);

  NimBLECharacteristic* pSecure =
      pService->createCharacteristic("1235",
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN);

  pService->start();

  pNonSecure->setValue("Hello Non Secure BLE");
  pSecure->setValue("Hello Secure BLE");

  // Advertising (API present in your header)
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();   // 
  adv->addServiceUUID("ABCD");
  adv->enableScanResponse(true);              // if adv packet is full, name goes in scan response
  // adv->setDiscoverableMode(BLE_GAP_DISC_MODE_GEN);
  adv->setMinInterval(160);  // 100 ms
  adv->setMaxInterval(160);
  adv->setScanFilter(true, false);   


  applyModeToSecurityAndAdv();
  
  adv->start();
}

void loop(){

  update_security_mode();

}

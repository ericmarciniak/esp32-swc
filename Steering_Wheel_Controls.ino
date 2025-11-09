#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>

#define CLEAR_BTN_PIN  18     // external "Clear Bonds" momentary button to GND
#define ADC_PIN        34     // SWC ADC input

// ---- RAW centers from your car ----
static const int RAW_NEXT_CENTER   = 707;   // Menu Up  -> Next
static const int RAW_PREV_CENTER   = 1530;  // Menu Down-> Prev
static const int RAW_IDLE_NEUTRAL  = 1900;  // lower to re-arm sooner

// ENTER vs EXIT hysteresis (RAW counts) — more catch-happy
static const int ENTER_TOL_NEXT = 160;      // was 120
static const int ENTER_TOL_PREV = 180;      // was 140
static const int EXIT_TOL_NEXT  = 200;      // keep exit tighter to avoid repeats
static const int EXIT_TOL_PREV  = 260;

// Power-off protection / release logic
static const int POWERDOWN_FLOOR_RAW            = 900;    // drop below => cancel
static const unsigned long MIN_INBAND_MS        = 2;      // fire sooner
static const unsigned long RELEASE_TO_NEUTRAL_MS = 600;   // allow quick taps to finish (kept for clarity)
static const unsigned long BAND_MAX_MS          = 900;    // cancel if lingering

// BLE / HID (bitfield on RID 2)
NimBLEServer*         server  = nullptr;
NimBLEHIDDevice*      hid     = nullptr;
NimBLECharacteristic* ccInput = nullptr;  // RID 2, 1 byte bitfield
enum { CC_NEXT_BIT = 0, CC_PREV_BIT = 1 };

// Clear-bonds long-press
static const unsigned long CLEAR_HOLD_MS = 6000;
bool           clearWasLow   = false;
unsigned long  clearLowStart = 0;
bool           clearingDone  = false;

// Band + pending-fire state
enum Band { BAND_NONE, BAND_NEXT, BAND_PREV };
static Band         bandState      = BAND_NONE;
static unsigned long bandEnterMs   = 0;
static bool          armed         = false;
static uint8_t       armedBit      = 0xFF;

static inline int readRaw() { return analogRead(ADC_PIN); }
static inline bool inNextEnter(int raw) {
  return raw >= (RAW_NEXT_CENTER - ENTER_TOL_NEXT) &&
         raw <= (RAW_NEXT_CENTER + ENTER_TOL_NEXT);
}
static inline bool inPrevEnter(int raw) {
  return raw >= (RAW_PREV_CENTER - ENTER_TOL_PREV) &&
         raw <= (RAW_PREV_CENTER + ENTER_TOL_PREV);
}
static inline bool outNextExit(int raw) {
  return raw < (RAW_NEXT_CENTER - EXIT_TOL_NEXT) ||
         raw > (RAW_NEXT_CENTER + EXIT_TOL_NEXT);
}
static inline bool outPrevExit(int raw) {
  return raw < (RAW_PREV_CENTER - EXIT_TOL_PREV) ||
         raw > (RAW_PREV_CENTER + EXIT_TOL_PREV);
}
static inline bool clearlyNeutralRaw(int raw) { return raw >= RAW_IDLE_NEUTRAL; }

static inline bool isConnected() { return server && server->getConnectedCount() > 0; }
static inline bool isAdvertising() {
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  return adv && adv->isAdvertising();
}

static void ccPressBit(uint8_t bit) {
  if (!ccInput || !isConnected()) return;
  uint8_t data = (1u << bit);
  ccInput->setValue(&data, 1);  ccInput->notify();  // press
  delay(12);
  data = 0x00;
  ccInput->setValue(&data, 1);  ccInput->notify();  // release
}

static void step_logic() {
  int raw = readRaw();
  unsigned long now = millis();

  // Cancel on power-down collapse
  if (armed && raw < POWERDOWN_FLOOR_RAW) {
    armed = false; armedBit = 0xFF;
    bandState = BAND_NONE;
    return;
  }

  switch (bandState) {
    case BAND_NONE:
      if (inNextEnter(raw)) {
        bandState = BAND_NEXT; bandEnterMs = now; armed = false;
      } else if (inPrevEnter(raw)) {
        bandState = BAND_PREV; bandEnterMs = now; armed = false;
      }
      break;

    case BAND_NEXT:
      if (!armed && (now - bandEnterMs >= MIN_INBAND_MS)) {
        armed = true; armedBit = CC_NEXT_BIT;
      }
      if (armed && clearlyNeutralRaw(raw)) {
        ccPressBit(armedBit);
        armed = false; armedBit = 0xFF; bandState = BAND_NONE;
      }
      if (outNextExit(raw) || (now - bandEnterMs) > BAND_MAX_MS) {
        armed = false; armedBit = 0xFF; bandState = BAND_NONE;
      }
      break;

    case BAND_PREV:
      if (!armed && (now - bandEnterMs >= MIN_INBAND_MS)) {
        armed = true; armedBit = CC_PREV_BIT;
      }
      if (armed && clearlyNeutralRaw(raw)) {
        ccPressBit(armedBit);
        armed = false; armedBit = 0xFF; bandState = BAND_NONE;
      }
      if (outPrevExit(raw) || (now - bandEnterMs) > BAND_MAX_MS) {
        armed = false; armedBit = 0xFF; bandState = BAND_NONE;
      }
      break;
  }
}

void setup() {
  pinMode(CLEAR_BTN_PIN, INPUT_PULLUP);   // external button to GND
  analogReadResolution(12);
  analogSetPinAttenuation(ADC_PIN, ADC_11db);

  NimBLEDevice::init("Steering Wheel Controls");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC);
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);

  server = NimBLEDevice::createServer();
  hid = new NimBLEHIDDevice(server);
  hid->setManufacturer("ESP32");
  hid->setBatteryLevel(100);
  hid->setPnp(0x02, 0x303A, 0x0002, 0x0100);

  // Report Map: minimal keyboard (RID1) + Consumer Control bitfield (RID2)
  static const uint8_t reportMap[] = {
    // Keyboard (RID 1)
    0x05,0x01,0x09,0x06,0xA1,0x01,
      0x85,0x01,
      0x05,0x07,0x19,0xE0,0x29,0xE7,
      0x15,0x00,0x25,0x01,
      0x75,0x01,0x95,0x08,0x81,0x02,
      0x95,0x01,0x75,0x08,0x81,0x01,
      0x95,0x06,0x75,0x08,0x15,0x00,
      0x25,0x65,0x05,0x07,0x19,0x00,0x29,0x65,0x81,0x00,
    0xC0,
    // Consumer Control (RID 2) bitfield: Next/Prev (2 bits)
    0x05,0x0C, 0x09,0x01, 0xA1,0x01, 0x85,0x02,
      0x09,0xB5, 0x09,0xB6,
      0x15,0x00, 0x25,0x01,
      0x75,0x01, 0x95,0x02, 0x81,0x02,
      0x95,0x06, 0x81,0x01,
    0xC0
  };
  hid->setReportMap((uint8_t*)reportMap, sizeof(reportMap));
  ccInput = hid->getInputReport(2);
  hid->startServices();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData ad;
  ad.setFlags((uint8_t)(0x02 | 0x04));
  ad.addServiceUUID(hid->getHidService()->getUUID());
  ad.setAppearance(961);
  ad.setName("Steering Wheel Controls");
  adv->setAdvertisementData(ad);
  NimBLEAdvertisementData sr; sr.setName("Steering Wheel Controls");
  adv->setScanResponseData(sr);
  adv->start(0);
}

void loop() {
  step_logic();

  // Clear bonds on long-press of the external button (pressed = LOW)
  bool clearNowLow = (digitalRead(CLEAR_BTN_PIN) == LOW);
  static bool clearWasLowLocal = false;
  static unsigned long clearLowStartLocal = 0;
  if (clearNowLow && !clearWasLowLocal) {
    clearLowStartLocal = millis();
  }
  if (clearNowLow && (millis() - clearLowStartLocal >= CLEAR_HOLD_MS) && !clearingDone) {
    clearingDone = true;
    NimBLEDevice::deleteAllBonds();
    delay(200);
    esp_restart();
  }
  clearWasLowLocal = clearNowLow;

  if (!isConnected() && !isAdvertising()) {
    NimBLEDevice::getAdvertising()->start(0);
  }

  delay(3);  // faster loop to catch very quick taps
}

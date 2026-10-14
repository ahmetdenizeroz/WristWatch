#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <NimBLEDevice.h>

// --- Pin Definitions ---
#define PIN_I2C_SDA     8
#define PIN_I2C_SCL     9
#define PIN_ENC_CLK     5   
#define PIN_ENC_DT      6   
#define PIN_ENC_SW      7   

// --- OLED Config ---
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64
#define OLED_RESET      -1
#define OLED_I2C_ADDR   0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- BLE UUIDs ---
#define SERVICE_MEDIA_UUID     "BB00"
#define CHAR_VOL_LEVEL_UUID    "BB01" // Phone writes volume (0-100)
#define CHAR_VOL_CMD_UUID      "BB02" // Watch notifies ("UP", "DOWN", "MUTE")
#define CHAR_TRACK_INFO_UUID   "BB03" // Phone writes track text

NimBLEServer* pServer = nullptr;
NimBLECharacteristic* pVolCmdChar = nullptr;

// --- State Variables ---
volatile int encoderRaw = 0;
volatile bool encoderMoved = false;
int lastEncoderVal = 0;

bool lastSwState = HIGH;
unsigned long lastSwTime = 0;

int currentPhoneVolume = 50; 
bool deviceConnected = false;
bool redrawNeeded = true;
String lastAction = "Ready";

// Track Metadata
String currentTrackTitle = "No Track";
String currentArtist = "VLC Music";

// --- BLE Callbacks ---
class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
        deviceConnected = true;
        lastAction = "Connected";
        redrawNeeded = true;
    }
    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
        deviceConnected = false;
        lastAction = "Disconnected";
        redrawNeeded = true;
        NimBLEDevice::startAdvertising();
    }
};

class VolumeLevelCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
        std::string val = pCharacteristic->getValue();
        if (val.length() > 0) {
            currentPhoneVolume = atoi(val.c_str());
            if (currentPhoneVolume > 100) currentPhoneVolume = 100;
            if (currentPhoneVolume < 0) currentPhoneVolume = 0;

            lastAction = "Vol Updated";
            redrawNeeded = true;
        }
    }
};

class TrackInfoCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
        std::string val = pCharacteristic->getValue();
        if (val.length() > 0) {
            String fullMeta = String(val.c_str());
            // Format can be "Artist - Title" or just "Title"
            int dashIndex = fullMeta.indexOf(" - ");
            if (dashIndex != -1) {
                currentArtist = fullMeta.substring(0, dashIndex);
                currentTrackTitle = fullMeta.substring(dashIndex + 3);
            } else {
                currentTrackTitle = fullMeta;
                currentArtist = "VLC Player";
            }
            lastAction = "Track Changed";
            redrawNeeded = true;
        }
    }
};

// --- Rotary Encoder ISR ---
void IRAM_ATTR handleEncoderISR() {
  static unsigned long lastInterruptTime = 0;
  unsigned long interruptTime = millis();
  
  if (interruptTime - lastInterruptTime > 5) {
    if (digitalRead(PIN_ENC_DT) == HIGH) {
      encoderRaw++;
    } else {
      encoderRaw--;
    }
    encoderMoved = true;
  }
  lastInterruptTime = interruptTime;
}

// --- Smartwatch OLED UI ---
void updateDisplay() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // 1. Top Status Header (y=0 to 10)
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(deviceConnected ? "[BLE]" : "[OFF]");

  display.setCursor(75, 0);
  display.printf("Vol: %3d%%", currentPhoneVolume);
  display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

  // 2. Middle: Track Info (y=14 to 45)
  // Song Title (Truncate to ~20 chars so it fits nicely on 128px)
  String dispTitle = currentTrackTitle.length() > 20 
                     ? currentTrackTitle.substring(0, 19) + ".." 
                     : currentTrackTitle;
  display.setCursor(0, 15);
  display.setTextSize(1);
  display.print(dispTitle);

  // Artist
  String dispArtist = currentArtist.length() > 20 
                      ? currentArtist.substring(0, 19) + ".." 
                      : currentArtist;
  display.setCursor(0, 27);
  display.setTextColor(SSD1306_WHITE);
  display.print(dispArtist);

  // Small status action
  display.setCursor(0, 39);
  display.print(lastAction);

  // 3. Bottom: Graphic Volume Bar (y=50 to 62)
  display.drawRect(0, 52, 128, 10, SSD1306_WHITE);
  int fillWidth = map(currentPhoneVolume, 0, 100, 0, 124);
  if (fillWidth > 0) {
    display.fillRect(2, 54, fillWidth, 6, SSD1306_WHITE);
  }

  display.display();
}

void setup() {
  Serial.begin(115200);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR)) {
    while (true) delay(100);
  }
  
  pinMode(PIN_ENC_CLK, INPUT_PULLUP);
  pinMode(PIN_ENC_DT, INPUT_PULLUP);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);
  
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_CLK), handleEncoderISR, FALLING);

  updateDisplay();

  NimBLEDevice::init("WristWatch-S3");
  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  NimBLEService* pMediaService = pServer->createService(SERVICE_MEDIA_UUID);

  // BB01: Volume Level Write
  NimBLECharacteristic* pVolLevelChar = pMediaService->createCharacteristic(
      CHAR_VOL_LEVEL_UUID, NIMBLE_PROPERTY::WRITE
  );
  pVolLevelChar->setCallbacks(new VolumeLevelCallbacks());

  // BB02: Volume Command Notify (UP / DOWN / MUTE)
  pVolCmdChar = pMediaService->createCharacteristic(
      CHAR_VOL_CMD_UUID, NIMBLE_PROPERTY::NOTIFY
  );

  // BB03: Track Info Write (Metadata from VLC)
  NimBLECharacteristic* pTrackInfoChar = pMediaService->createCharacteristic(
      CHAR_TRACK_INFO_UUID, NIMBLE_PROPERTY::WRITE
  );
  pTrackInfoChar->setCallbacks(new TrackInfoCallbacks());

  pMediaService->start();
  NimBLEDevice::getAdvertising()->addServiceUUID(SERVICE_MEDIA_UUID);
  NimBLEDevice::startAdvertising();
}

void loop() {
  unsigned long now = millis();

  // 1. Process Encoder Turns
  if (encoderMoved) {
    encoderMoved = false;
    if (encoderRaw != lastEncoderVal) {
      if (encoderRaw > lastEncoderVal) {
        lastAction = "VOL UP";
        if (deviceConnected) pVolCmdChar->setValue("UP");
      } else {
        lastAction = "VOL DOWN";
        if (deviceConnected) pVolCmdChar->setValue("DOWN");
      }
      if (deviceConnected) pVolCmdChar->notify();
      lastEncoderVal = encoderRaw;
      redrawNeeded = true;
    }
  }

  // 2. Process Encoder Button Press (Debounced)
  bool currentSwState = digitalRead(PIN_ENC_SW);
  if (currentSwState == HIGH && lastSwState == LOW) { // Button release
    if (now - lastSwTime > 200) {
      lastSwTime = now;
      lastAction = "MUTE";
      if (deviceConnected) {
        pVolCmdChar->setValue("MUTE");
        pVolCmdChar->notify();
      }
      redrawNeeded = true;
    }
  }
  lastSwState = currentSwState;

  if (redrawNeeded) {
    redrawNeeded = false;
    updateDisplay();
  }
  delay(10);
}
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

/*
 ============================================================================
  ESP32-C6FH8 1.9" LCD + QMI8658 6-Axis IMU Hardware Test
  Target Board: ESP32-C6 1.9inch Display Dev Board (170x320 ST7789, QMI8658)
 ============================================================================
 
 Features in this version:
   - ZERO-GLITCH Display: Uses (fg, bg) text rendering without full-box clears.
   - Separate update rates: Bubble at 30 FPS, numerical text at 5 FPS.
   - Screen-aligned axes: X = width (Roll), Y = height (Pitch).
   - One-Touch Calibration: Press the BOOT button (GPIO 9) or send 'c' via
     Serial to zero-out accelerometer & gyro bias when resting flat!
 ============================================================================
*/

// --- Pin Definitions for 1.9" ST7789 (SPI) ---
#define TFT_MOSI  4   // Shared SPI MOSI / SDA
#define TFT_SCLK  5   // Shared SPI SCLK
#define TFT_DC    6   // Data / Command
#define TFT_CS    7   // Chip Select
#define TFT_RST   14  // Reset
#define TFT_BL    15  // Backlight (Active LOW)

// --- Onboard BOOT Button for Calibration ---
#define PIN_BOOT_BTN 9 // Onboard BOOT button (active LOW)

// --- Pin Definitions for QMI8658 6-Axis IMU (I2C) ---
#define IMU_SDA   18  // I2C Data
#define IMU_SCL   8   // I2C Clock
#define IMU_ADDR  0x6B // Default I2C address

// --- QMI8658 Registers ---
#define QMI_REG_WHO_AM_I   0x00
#define QMI_REG_CTRL1      0x02
#define QMI_REG_CTRL2      0x03
#define QMI_REG_CTRL3      0x04
#define QMI_REG_CTRL7      0x08
#define QMI_REG_STATUS0    0x2E
#define QMI_REG_DATA_START 0x33 // 0x33 = TEMP_L
#define QMI_REG_RESET      0x60

// --- Color Helpers ---
#define COLOR_NAVY    0x18E3
#define COLOR_DARKGREY 0x4208
#define COLOR_GRID     0x3186

// Create Adafruit ST7789 object using Hardware SPI
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// --- IMU Telemetry State ---
struct IMUData {
  float ax, ay, az;      // Accelerometer in Gs (Calibrated)
  float gx, gy, gz;      // Gyroscope in degrees/sec (Calibrated)
  float temperature;     // Temperature in Celsius
  float pitch, roll;     // Tilt angles in degrees
  bool isConnected;
} imu;

// --- Calibration Offsets ---
float offsetAx = 0.0f;
float offsetAy = 0.0f;
float offsetAz = 0.0f; // Calibrated so Z = +1.0G (or -1.0G) when flat
float offsetGx = 0.0f;
float offsetGy = 0.0f;
float offsetGz = 0.0f;
bool isCalibrated = false;

// ==========================================
// QMI8658 Low-Level Helper Functions
// ==========================================
void qmiWriteReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(IMU_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

uint8_t qmiReadReg(uint8_t reg) {
  Wire.beginTransmission(IMU_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)IMU_ADDR, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0xFF;
}

bool initQMI8658() {
  Wire.begin(IMU_SDA, IMU_SCL, 100000);
  delay(50);

  uint8_t chipId = qmiReadReg(QMI_REG_WHO_AM_I);
  Serial.printf("[IMU] WHO_AM_I: 0x%02X (Expected: 0x05)\n", chipId);

  if (chipId != 0x05) {
    Serial.println("[IMU] ERROR: QMI8658 not detected at 0x6B!");
    return false;
  }

  // Soft Reset
  qmiWriteReg(QMI_REG_RESET, 0xB0);
  delay(30);

  // Disable sensors before configuration
  qmiWriteReg(QMI_REG_CTRL7, 0x00);
  delay(10);

  // Auto-Increment enable
  qmiWriteReg(QMI_REG_CTRL1, 0x60);

  // Accel: ±4g, 250Hz ODR
  qmiWriteReg(QMI_REG_CTRL2, 0x13);

  // Gyro: ±512 dps, 250Hz ODR
  qmiWriteReg(QMI_REG_CTRL3, 0x43);

  // Enable Accel & Gyro
  qmiWriteReg(QMI_REG_CTRL7, 0x03);
  delay(30);

  Wire.setClock(400000);
  Serial.println("[IMU] QMI8658 successfully initialized!");
  return true;
}

// Reads raw values without applying offsets
void readRawSensors(float &rawAx, float &rawAy, float &rawAz,
                    float &rawGx, float &rawGy, float &rawGz,
                    float &rawTemp) {
  Wire.beginTransmission(IMU_ADDR);
  Wire.write(QMI_REG_DATA_START);
  Wire.endTransmission(false);

  Wire.requestFrom((uint8_t)IMU_ADDR, (uint8_t)14);
  if (Wire.available() < 14) return;

  uint8_t buf[14];
  for (int i = 0; i < 14; i++) {
    buf[i] = Wire.read();
  }

  // Temperature
  int16_t tVal = (int16_t)((buf[1] << 8) | buf[0]);
  rawTemp = (float)tVal / 256.0f;

  // Accel (±4g -> 8192 LSB/g)
  int16_t aX = (int16_t)((buf[3] << 8) | buf[2]);
  int16_t aY = (int16_t)((buf[5] << 8) | buf[4]);
  int16_t aZ = (int16_t)((buf[7] << 8) | buf[6]);
  rawAx = (float)aX / 8192.0f;
  rawAy = (float)aY / 8192.0f;
  rawAz = (float)aZ / 8192.0f;

  // Gyro (±512 dps -> 64 LSB/dps)
  int16_t gX = (int16_t)((buf[9] << 8) | buf[8]);
  int16_t gY = (int16_t)((buf[11] << 8) | buf[10]);
  int16_t gZ = (int16_t)((buf[13] << 8) | buf[12]);
  rawGx = (float)gX / 64.0f;
  rawGy = (float)gY / 64.0f;
  rawGz = (float)gZ / 64.0f;
}

// Calibrate zero-g and gyro bias while lying flat
void calibrateIMU() {
  Serial.println("\n[CAL] Starting calibration... KEEP BOARD FLAT!");
  
  // Show on screen
  tft.fillRect(14, 185, 142, 110, COLOR_NAVY);
  tft.setTextColor(ST77XX_YELLOW, COLOR_NAVY);
  tft.setCursor(20, 210);
  tft.print("CALIBRATING...");
  tft.setCursor(20, 230);
  tft.setTextColor(ST77XX_WHITE, COLOR_NAVY);
  tft.print("KEEP FLAT ON DESK");

  float sumAx = 0, sumAy = 0, sumAz = 0;
  float sumGx = 0, sumGy = 0, sumGz = 0;
  float rAx, rAy, rAz, rGx, rGy, rGz, rT;

  const int samples = 50;
  for (int i = 0; i < samples; i++) {
    readRawSensors(rAx, rAy, rAz, rGx, rGy, rGz, rT);
    sumAx += rAx;
    sumAy += rAy;
    sumAz += rAz;
    sumGx += rGx;
    sumGy += rGy;
    sumGz += rGz;
    delay(15);
  }

  offsetAx = sumAx / (float)samples;
  offsetAy = sumAy / (float)samples;
  float avgAz = sumAz / (float)samples;
  // If board is face-up, gravity is either +1.0G or -1.0G depending on PCB layer
  offsetAz = (avgAz >= 0) ? (avgAz - 1.0f) : (avgAz + 1.0f);

  offsetGx = sumGx / (float)samples;
  offsetGy = sumGy / (float)samples;
  offsetGz = sumGz / (float)samples;

  isCalibrated = true;
  Serial.printf("[CAL] Complete! Offsets: Ax=%+.3f, Ay=%+.3f, Az=%+.3f G\n",
                offsetAx, offsetAy, offsetAz);

  // Clear prompt area and redraw labels
  tft.fillRect(14, 185, 142, 110, ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
  tft.setCursor(14, 168);
  tft.print("PITCH & ROLL TILT");
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setCursor(14, 230);
  tft.print("ACCEL (G):");
  tft.setCursor(14, 275);
  tft.print("TEMP / STATUS:");
}

void readQMI8658() {
  if (!imu.isConnected) return;

  float rawAx, rawAy, rawAz, rawGx, rawGy, rawGz, rawTemp;
  readRawSensors(rawAx, rawAy, rawAz, rawGx, rawGy, rawGz, rawTemp);

  imu.temperature = rawTemp;

  // Apply calibrated offsets
  imu.ax = rawAx - offsetAx;
  imu.ay = rawAy - offsetAy;
  imu.az = rawAz - offsetAz;
  imu.gx = rawGx - offsetGx;
  imu.gy = rawGy - offsetGy;
  imu.gz = rawGz - offsetGz;

  // Calculate Roll & Pitch matching screen coordinate frame:
  // - Roll tilts Left/Right (along screen width, driven by ax)
  // - Pitch tilts Forward/Backward (along screen height, driven by ay)
  float zSign = (offsetAz < 0) ? -1.0f : 1.0f; // Account for chip mounted on back
  float normAz = imu.az * zSign;
  float safeZ = (fabs(normAz) > 0.05f) ? normAz : (normAz >= 0 ? 0.05f : -0.05f);

  float targetRoll  = atan2(imu.ax, safeZ) * 180.0f / PI;
  float targetPitch = atan2(-imu.ay, safeZ) * 180.0f / PI;

  // Exponential low-pass smoothing (60% previous, 40% new)
  imu.roll  = (imu.roll  * 0.60f) + (targetRoll  * 0.40f);
  imu.pitch = (imu.pitch * 0.60f) + (targetPitch * 0.40f);
}

// ==========================================
// UI Rendering Functions (ST7789 170x320)
// ==========================================
void drawStaticUI() {
  tft.fillScreen(ST77XX_BLACK);

  // Header Bar
  tft.fillRect(0, 0, 170, 36, COLOR_NAVY);
  tft.setTextColor(ST77XX_WHITE, COLOR_NAVY);
  tft.setTextSize(1);
  tft.setCursor(12, 8);
  tft.println("ESP32-C6 WATCH");
  tft.setCursor(12, 20);
  tft.setTextColor(ST77XX_CYAN, COLOR_NAVY);
  tft.println("DISPLAY + IMU TEST");

  // Divider
  tft.drawFastHLine(0, 36, 170, ST77XX_WHITE);

  // Artificial Horizon / Bubble Level Box (Center Y: 46 - 156)
  tft.drawRoundRect(10, 44, 150, 112, 8, COLOR_DARKGREY);
  tft.drawFastHLine(20, 100, 130, COLOR_GRID);        // Horizontal crosshair
  tft.drawFastVLine(85, 54, 92, COLOR_GRID);          // Vertical crosshair
  tft.drawCircle(85, 100, 20, COLOR_GRID);            // Target circle

  // Telemetry Labels
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
  tft.setCursor(14, 168);
  tft.print("PITCH & ROLL TILT");

  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setCursor(14, 230);
  tft.print("ACCEL (G):");

  tft.setCursor(14, 275);
  tft.print("TEMP / STATUS:");

  // Footer: Instructions
  tft.drawFastHLine(0, 305, 170, COLOR_DARKGREY);
  tft.setTextColor(0x7BEF, ST77XX_BLACK); // Dim grey
  tft.setCursor(10, 310);
  tft.print("BOOT (IO9): CAL ZERO");
}

// High-speed Bubble Level (30 FPS)
void updateBubbleLevel() {
  static int prevBubbleX = 85;
  static int prevBubbleY = 100;

  // Roll moves X (±60px range), Pitch moves Y (±45px range)
  int bubbleX = 85 + (int)(imu.roll * 1.2f);
  int bubbleY = 100 + (int)(imu.pitch * 1.2f);
  bubbleX = constrain(bubbleX, 22, 148);
  bubbleY = constrain(bubbleY, 56, 144);

  if (prevBubbleX != bubbleX || prevBubbleY != bubbleY) {
    // Erase old bubble by drawing black circle
    tft.fillCircle(prevBubbleX, prevBubbleY, 6, ST77XX_BLACK);
    
    // Restore any grid lines that were erased
    tft.drawFastHLine(20, 100, 130, COLOR_GRID);
    tft.drawFastVLine(85, 54, 92, COLOR_GRID);
    tft.drawCircle(85, 100, 20, COLOR_GRID);

    // Draw new bubble
    tft.fillCircle(bubbleX, bubbleY, 6, ST77XX_GREEN);
    tft.drawCircle(bubbleX, bubbleY, 6, ST77XX_WHITE);

    prevBubbleX = bubbleX;
    prevBubbleY = bubbleY;
  }
}

// Flicker-Free Numerical Readouts (5 FPS)
// Uses (foreground, background) to directly overwrite character cells without fillRect
void updateNumericalText() {
  tft.setTextSize(1);

  // 1. Tilt angles
  tft.setTextColor(ST77XX_GREEN, ST77XX_BLACK);
  tft.setCursor(14, 188);
  tft.printf("Roll:  %+6.1f deg  ", imu.roll);
  tft.setCursor(14, 202);
  tft.printf("Pitch: %+6.1f deg  ", imu.pitch);

  // 2. Accelerometer (G)
  tft.setTextColor(ST77XX_CYAN, ST77XX_BLACK);
  tft.setCursor(14, 246);
  tft.printf("X:%+5.2f  Y:%+5.2f ", imu.ax, imu.ay);
  tft.setCursor(14, 258);
  tft.printf("Z:%+5.2f G        ", imu.az);

  // 3. Temperature & Status
  tft.setTextColor(ST77XX_MAGENTA, ST77XX_BLACK);
  tft.setCursor(14, 290);
  if (imu.isConnected) {
    tft.printf("%.1f C | %s  ", imu.temperature, isCalibrated ? "CAL:OK" : "UNCAL");
  } else {
    tft.setTextColor(ST77XX_RED, ST77XX_BLACK);
    tft.print("IMU: ERROR     ");
  }

  // 4. Heartbeat pulse in top corner
  static bool beat = false;
  tft.fillRect(156, 12, 6, 6, beat ? ST77XX_GREEN : COLOR_NAVY);
  beat = !beat;
}

// ==========================================
// Setup & Loop
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n==========================================");
  Serial.println(" ESP32-C6 1.9\" LCD + QMI8658 Hardware Test");
  Serial.println("==========================================");

  // BOOT button setup
  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);

  // Initialize Backlight
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, LOW);

  // Initialize Hardware SPI with custom ESP32-C6 pins
  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);

  // Initialize ST7789
  tft.init(170, 320);
  tft.setRotation(0);
  Serial.println("[TFT] ST7789 170x320 Initialized.");

  // Initialize IMU
  imu.isConnected = initQMI8658();

  // Initial Static UI
  drawStaticUI();

  // Auto-calibrate on boot if board is resting flat
  if (imu.isConnected) {
    calibrateIMU();
  }
}

void loop() {
  unsigned long now = millis();

  // Check BOOT button (IO9) or Serial command 'c' for recalibration
  if (digitalRead(PIN_BOOT_BTN) == LOW) {
    delay(50); // Debounce
    if (digitalRead(PIN_BOOT_BTN) == LOW) {
      calibrateIMU();
    }
  }
  if (Serial.available()) {
    char ch = Serial.read();
    if (ch == 'c' || ch == 'C') {
      calibrateIMU();
    }
  }

  // 1. High-speed loop (~30 FPS, every 33ms): Sensor read & bubble update
  static unsigned long lastBubbleTime = 0;
  if (now - lastBubbleTime >= 33) {
    lastBubbleTime = now;
    readQMI8658();
    updateBubbleLevel();
  }

  // 2. Low-speed loop (~5 FPS, every 200ms): Clean, flicker-free text update
  static unsigned long lastTextTime = 0;
  if (now - lastTextTime >= 200) {
    lastTextTime = now;
    updateNumericalText();

    // Telemetry output to Serial
    static unsigned long lastSerial = 0;
    if (now - lastSerial >= 500) {
      lastSerial = now;
      if (imu.isConnected) {
        Serial.printf("[TELEMETRY] Pitch: %+5.1f | Roll: %+5.1f | Acc: [%+4.2f, %+4.2f, %+4.2f] G | Temp: %.1f C\n",
                      imu.pitch, imu.roll, imu.ax, imu.ay, imu.az, imu.temperature);
      }
    }
  }
}

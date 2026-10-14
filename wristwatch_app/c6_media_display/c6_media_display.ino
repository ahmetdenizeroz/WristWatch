#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <NimBLEDevice.h>

/*
 ============================================================================
  ESP32-C6FH8 1.9" SMARTWATCH MEDIA DISPLAY (LANDSCAPE 320x170)
  Target Board: ESP32-C6 1.9inch Display Dev Board
 ============================================================================
 
 Features:
   - Horizontal / Landscape orientation (320x170).
   - Real-time Song Title and Artist metadata display over BLE (BB03).
   - Real-time Volume synchronization and dynamic progress bar (BB01).
   - 120x120 Album Cover Art streamed over BLE (BB04) in 100% LOSSLESS QOI (<100ms).
   - Built-in demo album art test pattern on boot.
   - Clean, flicker-free dual-color UI rendering.
   - NimBLE BLE peripheral (advertises as "WristWatch-C6").

 Required Libraries (Arduino Library Manager):
   1. "Adafruit GFX Library"
   2. "Adafruit ST7735 and ST7789 Library"
   3. "NimBLE-Arduino" (v2.x)

 Arduino IDE Settings:
   - Board: "ESP32C6 Dev Module"
   - USB CDC On Boot: "Enabled"
   - Flash Size: "8MB (64Mb)"
 ============================================================================
*/

// --- Pin Definitions for 1.9" ST7789 (SPI) ---
#define TFT_MOSI  4   // Shared SPI MOSI
#define TFT_SCLK  5   // Shared SPI SCLK
#define TFT_DC    6   // Data / Command
#define TFT_CS    7   // Chip Select
#define TFT_RST   14  // Reset
#define TFT_BL    15  // Backlight (Active LOW)

// Screen Orientation: 1 = Landscape (USB right), 3 = Landscape (USB left)
#define SCREEN_ROTATION 1 

// --- Color Palette ---
#define COLOR_NAVY      0x18E3
#define COLOR_DARKGREY  0x3186
#define COLOR_LIGHTGREY 0x7BEF
#define COLOR_CARD_BG   0x10A2

// --- Display Object ---
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// --- BLE UUID Definitions ---
#define SERVICE_MEDIA_UUID   "BB00"
#define CHAR_VOL_LEVEL_UUID  "BB01" // Phone writes volume (0-100)
#define CHAR_TRACK_INFO_UUID "BB03" // Phone writes track metadata ("Artist - Title")
#define CHAR_ART_CHUNK_UUID  "BB04" // Phone streams 120x120 lossless QOI art
#define CHAR_ALERT_UUID      "BB05" // Phone streams pop-up alerts (WhatsApp, SMS, Call)

// --- Cover Art Buffer & QOI Decoder (120 x 120 Lossless) ---
#define ART_WIDTH       120
#define ART_HEIGHT      120
#define ART_POS_X       10
#define ART_POS_Y       25
#define MAX_QOI_SIZE    65536

uint16_t artBuffer[ART_WIDTH * ART_HEIGHT];
uint8_t qoiBuffer[MAX_QOI_SIZE];
volatile size_t qoiBytesReceived = 0;
volatile bool qoiReceiving = false;
volatile bool qoiDataReady = false;
volatile bool newArtReady = false;

typedef union {
  struct { uint8_t r, g, b, a; } rgba;
  uint32_t v;
} qoi_rgba_t;

// Ultra-fast native QOI Decoder to RGB565 (zero dynamic heap allocation)
bool qoi_decode_to_rgb565(const uint8_t *data, size_t size, uint16_t *out_pixels, int expected_w, int expected_h) {
  if (!data || size < 22) return false; // 14-byte header + 8-byte padding

  // 1. Verify "qoif" magic
  if (data[0] != 'q' || data[1] != 'o' || data[2] != 'i' || data[3] != 'f') {
    return false;
  }

  // 2. Read dimensions (big-endian uint32)
  uint32_t w = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) | ((uint32_t)data[6] << 8) | data[7];
  uint32_t h = ((uint32_t)data[8] << 24) | ((uint32_t)data[9] << 16) | ((uint32_t)data[10] << 8) | data[11];

  if (w != (uint32_t)expected_w || h != (uint32_t)expected_h) {
    return false;
  }

  qoi_rgba_t index[64];
  memset(index, 0, sizeof(index));

  qoi_rgba_t px;
  px.rgba.r = 0;
  px.rgba.g = 0;
  px.rgba.b = 0;
  px.rgba.a = 255;

  size_t p = 14;
  size_t chunks_len = size - 8;
  int total_pixels = expected_w * expected_h;
  int pixel_idx = 0;
  int run = 0;

  while (pixel_idx < total_pixels) {
    if (run > 0) {
      run--;
    } else if (p < chunks_len) {
      uint8_t b1 = data[p++];

      if (b1 == 0xfe) { // QOI_OP_RGB (0b11111110)
        px.rgba.r = data[p++];
        px.rgba.g = data[p++];
        px.rgba.b = data[p++];
      } else if (b1 == 0xff) { // QOI_OP_RGBA (0b11111111)
        px.rgba.r = data[p++];
        px.rgba.g = data[p++];
        px.rgba.b = data[p++];
        px.rgba.a = data[p++];
      } else if ((b1 & 0xc0) == 0x00) { // QOI_OP_INDEX (0b00xxxxxx)
        px = index[b1 & 0x3f];
      } else if ((b1 & 0xc0) == 0x40) { // QOI_OP_DIFF (0b01xxxxxx)
        px.rgba.r += ((b1 >> 4) & 0x03) - 2;
        px.rgba.g += ((b1 >> 2) & 0x03) - 2;
        px.rgba.b += ( b1       & 0x03) - 2;
      } else if ((b1 & 0xc0) == 0x80) { // QOI_OP_LUMA (0b10xxxxxx)
        uint8_t b2 = data[p++];
        int vg = (b1 & 0x3f) - 32;
        px.rgba.r += vg - 8 + ((b2 >> 4) & 0x0f);
        px.rgba.g += vg;
        px.rgba.b += vg - 8 +  (b2       & 0x0f);
      } else if ((b1 & 0xc0) == 0xc0) { // QOI_OP_RUN (0b11xxxxxx)
        run = (b1 & 0x3f);
      }

      int index_pos = (px.rgba.r * 3 + px.rgba.g * 5 + px.rgba.b * 7 + px.rgba.a * 11) & 63;
      index[index_pos] = px;
    } else {
      break;
    }

    out_pixels[pixel_idx++] = ((px.rgba.r & 0xF8) << 8) | ((px.rgba.g & 0xFC) << 3) | (px.rgba.b >> 3);
  }

  return (pixel_idx == total_pixels);
}

// --- Screen States ---
enum ScreenState {
  SCREEN_MEDIA_PLAYER,
  SCREEN_POPUP_MESSAGE,
  SCREEN_INCOMING_CALL
};
ScreenState currentScreen = SCREEN_MEDIA_PLAYER;

// --- Pop-up Notification State ---
#define COLOR_POPUP_BG      0x0841  // Very dark charcoal
#define COLOR_WA_GREEN      0x25D3  // WhatsApp Green (#25D366)
#define COLOR_WA_HEADER_BG  0x0A64  // Dark green header
#define COLOR_SMS_BLUE      0x04FF  // SMS Azure Blue
#define COLOR_SMS_HEADER_BG 0x01D3  // Dark blue header

String alertApp = "WHATSAPP";
String alertSender = "";
String alertMessage = "";
uint16_t alertBorderColor = COLOR_WA_GREEN;
uint16_t alertHeaderBgColor = COLOR_WA_HEADER_BG;
unsigned long popupStartTime = 0;
const unsigned long POPUP_TIMEOUT_MS = 6000;

// --- Phone Call State ---
String callerName = "";
String callerNumber = "";
unsigned long lastCallPulse = 0;
bool callPulseState = false;

// Alert Queue from BLE Callback
String pendingAlertString = "";
volatile bool alertDataReady = false;

// --- Playback State ---
String currentTitle = "No Track Playing";
String currentArtist = "Connect Phone App";
volatile int currentVolume = 50;
volatile bool deviceConnected = false;
volatile bool metadataChanged = true;
volatile bool volumeChanged = true;

// ==========================================
// BLE Callbacks
// ==========================================
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    deviceConnected = true;
    metadataChanged = true;
    volumeChanged = true;
    Serial.println("[BLE] Phone Connected!");
  }

  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    deviceConnected = false;
    metadataChanged = true;
    Serial.println("[BLE] Phone Disconnected. Restarting advertising...");
    NimBLEDevice::startAdvertising();
  }
};

// Characteristic BB01: Volume Level (0-100)
class VolumeLevelCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
    std::string val = pCharacteristic->getValue();
    if (val.length() > 0) {
      currentVolume = atoi(val.c_str());
      if (currentVolume < 0) currentVolume = 0;
      if (currentVolume > 100) currentVolume = 100;
      volumeChanged = true; // Instant partial update!
    }
  }
};

// Characteristic BB03: Song Metadata ("Artist - Title" or just "Title")
class TrackInfoCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
    std::string val = pCharacteristic->getValue();
    if (val.length() > 0) {
      String fullMeta = String(val.c_str());
      fullMeta.trim();
      Serial.printf("[BLE] Track Metadata: %s\n", fullMeta.c_str());

      int dashIdx = fullMeta.indexOf(" - ");
      if (dashIdx != -1) {
        currentArtist = fullMeta.substring(0, dashIdx);
        currentTitle  = fullMeta.substring(dashIdx + 3);
      } else {
        currentTitle  = fullMeta;
        currentArtist = "Media Player";
      }
      metadataChanged = true;
    }
  }
};

// Characteristic BB04: Compressed Lossless QOI Cover Art Byte Stream
class ArtChunkCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
    const uint8_t* data = (const uint8_t*)pCharacteristic->getValue().data();
    size_t len = pCharacteristic->getValue().length();
    if (len == 0) return;

    // Header Packet: [0xAA, 0x55, 120, 120] -> Start of new QOI stream
    if (len == 4 && data[0] == 0xAA && data[1] == 0x55 && data[2] == ART_WIDTH && data[3] == ART_HEIGHT) {
      qoiBytesReceived = 0;
      qoiReceiving = true;
      qoiDataReady = false;
      return;
    }

    // End Packet: [0xAA, 0xFF] -> Finish transfer, trigger decode in loop()
    if (len == 2 && data[0] == 0xAA && data[1] == 0xFF) {
      qoiReceiving = false;
      qoiDataReady = true;
      return;
    }

    // Data Chunk: Append incoming QOI bytes directly to RAM buffer
    if (qoiReceiving) {
      if (qoiBytesReceived + len <= MAX_QOI_SIZE) {
        memcpy(&qoiBuffer[qoiBytesReceived], data, len);
        qoiBytesReceived += len;
      }
    }
  }
};

// Characteristic BB05: Pop-up Alerts ("TYPE|SENDER|MESSAGE")
class AlertCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
    std::string val = pCharacteristic->getValue();
    if (val.length() > 0) {
      pendingAlertString = String(val.c_str());
      alertDataReady = true;
    }
  }
};

// ==========================================
// Graphics & UI Functions
// ==========================================

// Generates a colorful synthwave sunset demo cover art for testing
void generateDemoCoverArt() {
  for (int y = 0; y < ART_HEIGHT; y++) {
    for (int x = 0; x < ART_WIDTH; x++) {
      int r = map(y, 0, ART_HEIGHT, 240, 40);
      int g = map(y, 0, ART_HEIGHT, 40, 10);
      int b = map(y, 0, ART_HEIGHT, 160, 80);

      int dx = x - 60;
      int dy = y - 55;
      if (dx * dx + dy * dy < 25 * 25) {
        if ((y / 4) % 2 == 0 || y < 55) {
          r = 255;
          g = map(y, 30, 80, 220, 50);
          b = 30;
        }
      }

      if (y > 80) {
        r = 15;
        g = 5;
        b = 35;
        if ((x + y) % 12 == 0 || (x - y) % 12 == 0 || y == 95 || y == 110) {
          r = 0;
          g = 220;
          b = 255;
        }
      }

      uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
      artBuffer[y * ART_WIDTH + x] = c;
    }
  }
}

void drawStaticLayout() {
  tft.fillScreen(ST77XX_BLACK);

  // 1. Top Header Bar (0 to 18 px)
  tft.fillRect(0, 0, 320, 20, COLOR_NAVY);
  tft.setTextColor(ST77XX_WHITE, COLOR_NAVY);
  tft.setTextSize(1);
  tft.setCursor(8, 6);
  tft.print("WRISTWATCH MEDIA");

  // 2. Cover Art Frame Border (Left side: 120x120 at (10, 25))
  tft.drawRoundRect(ART_POS_X - 2, ART_POS_Y - 2, ART_WIDTH + 4, ART_HEIGHT + 4, 4, COLOR_DARKGREY);

  // 3. Right Panel Container Card
  tft.drawRoundRect(140, 23, 172, 124, 6, COLOR_DARKGREY);
  tft.fillRect(141, 24, 170, 122, COLOR_CARD_BG);

  // Section Header inside card
  tft.setTextColor(ST77XX_YELLOW, COLOR_CARD_BG);
  tft.setCursor(150, 32);
  tft.print("NOW PLAYING");

  // Footer bar
  tft.drawFastHLine(0, 155, 320, COLOR_DARKGREY);
  tft.setTextColor(COLOR_LIGHTGREY, ST77XX_BLACK);
  tft.setCursor(8, 160);
  tft.print("BLE: WristWatch-C6 (170x320 IPS)");
}

void updateConnectionStatus() {
  tft.setTextSize(1);
  if (deviceConnected) {
    tft.setTextColor(ST77XX_GREEN, COLOR_NAVY);
    tft.setCursor(205, 6);
    tft.print("[CONNECTED]  ");
  } else {
    tft.setTextColor(ST77XX_ORANGE, COLOR_NAVY);
    tft.setCursor(205, 6);
    tft.print("[WAITING...] ");
  }
}

void drawCoverArt() {
  tft.drawRGBBitmap(ART_POS_X, ART_POS_Y, artBuffer, ART_WIDTH, ART_HEIGHT);
  tft.fillRect(ART_POS_X, ART_POS_Y + ART_HEIGHT + 2, ART_WIDTH, 6, ST77XX_BLACK);
}

void updateArtProgress() {
  if (qoiReceiving) {
    int barW = map(qoiBytesReceived, 0, 16000, 0, ART_WIDTH);
    if (barW > ART_WIDTH) barW = ART_WIDTH;
    tft.fillRect(ART_POS_X, ART_POS_Y + ART_HEIGHT + 2, barW, 4, ST77XX_CYAN);
  }
}

void updateVolumeUI() {
  // 1. Snappy Volume Percentage Text
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_YELLOW, COLOR_CARD_BG);
  tft.setCursor(150, 114);
  tft.printf("VOL: %3d%%", currentVolume);

  // 2. Fast Volume Bar Fill (Only update changed pixels!)
  int fillW = map(currentVolume, 0, 100, 0, 148);
  if (fillW > 0) {
    tft.fillRect(151, 127, fillW, 8, ST77XX_GREEN);
  }
  if (fillW < 148) {
    tft.fillRect(151 + fillW, 127, 148 - fillW, 8, COLOR_CARD_BG);
  }
}

void updateMetadataUI() {
  // Clear only the title & artist text area (not the volume bar!)
  tft.fillRect(150, 48, 152, 62, COLOR_CARD_BG);

  // Song Title (Truncate to fit 12 chars per line)
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_WHITE, COLOR_CARD_BG);
  tft.setCursor(150, 50);

  if (currentTitle.length() > 12) {
    String line1 = currentTitle.substring(0, 12);
    String line2 = currentTitle.substring(12);
    if (line2.length() > 11) line2 = line2.substring(0, 10) + "..";
    tft.println(line1);
    tft.setCursor(150, 68);
    tft.println(line2);
  } else {
    tft.println(currentTitle);
  }

  // Artist Name
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_CYAN, COLOR_CARD_BG);
  tft.setCursor(150, 94);
  String artistDisp = currentArtist;
  if (artistDisp.length() > 22) artistDisp = artistDisp.substring(0, 20) + "..";
  tft.println(artistDisp);

  // Render Volume bar border & initial bar state
  tft.drawRoundRect(150, 126, 150, 10, 3, COLOR_DARKGREY);
  updateVolumeUI();
}

// ==========================================
// Pop-up & Modal Alert UI Functions
// ==========================================

// Helper function to split and wrap text nicely within a maximum line length
void drawWrappedText(const String& text, int startX, int startY, int maxCharsPerLine, int maxLines) {
  int curX = startX;
  int curY = startY;
  int lineCount = 0;
  int startIdx = 0;
  int len = text.length();

  while (startIdx < len && lineCount < maxLines) {
    int endIdx = startIdx + maxCharsPerLine;
    if (endIdx >= len) {
      endIdx = len;
    } else {
      // Find last space to avoid breaking words
      int spaceIdx = text.lastIndexOf(' ', endIdx);
      if (spaceIdx > startIdx + (maxCharsPerLine / 2)) {
        endIdx = spaceIdx;
      }
    }

    String line = text.substring(startIdx, endIdx);
    line.trim();

    // If this is the last line and there is still text remaining, add ellipsis
    if (lineCount == maxLines - 1 && endIdx < len) {
      if (line.length() > maxCharsPerLine - 3) {
        line = line.substring(0, maxCharsPerLine - 3);
      }
      line += "...";
    }

    tft.setCursor(curX, curY);
    tft.print(line);

    curY += 12; // 8px font + 4px line spacing
    lineCount++;
    startIdx = endIdx;
    while (startIdx < len && text.charAt(startIdx) == ' ') {
      startIdx++; // Skip leading spaces on next line
    }
  }
}

void restoreMediaPlayerScreen() {
  currentScreen = SCREEN_MEDIA_PLAYER;
  drawStaticLayout();
  drawCoverArt();
  updateMetadataUI();
  updateVolumeUI();
  updateConnectionStatus();
}

void renderPopupMessage() {
  // Dimensions: 280 x 124 at (20, 23)
  const int cardX = 20;
  const int cardY = 23;
  const int cardW = 280;
  const int cardH = 124;

  // 1. Double-stroke rounded border
  tft.drawRoundRect(cardX, cardY, cardW, cardH, 6, alertBorderColor);
  tft.drawRoundRect(cardX + 1, cardY + 1, cardW - 2, cardH - 2, 5, alertBorderColor);

  // 2. Card body background
  tft.fillRoundRect(cardX + 2, cardY + 2, cardW - 4, cardH - 4, 4, COLOR_POPUP_BG);

  // 3. Header Bar (Height: 22 px)
  tft.fillRoundRect(cardX + 2, cardY + 2, cardW - 4, 20, 4, alertHeaderBgColor);
  tft.fillRect(cardX + 2, cardY + 12, cardW - 4, 10, alertHeaderBgColor); // Square off bottom
  tft.drawFastHLine(cardX + 2, cardY + 22, cardW - 4, alertBorderColor);

  // Header Title & Tag
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE, alertHeaderBgColor);
  tft.setCursor(cardX + 10, cardY + 6);
  if (alertApp == "WHATSAPP") {
    tft.print("[WA] WHATSAPP");
  } else if (alertApp == "SMS") {
    tft.print("[SMS] MESSAGING");
  } else if (alertApp == "MISSED CALL") {
    tft.print("[!] MISSED CALL");
  } else {
    tft.print(alertApp);
  }

  tft.setCursor(cardX + cardW - 60, cardY + 6);
  tft.print("Just now");

  // 4. Sender Name (Large bold text)
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_WHITE, COLOR_POPUP_BG);
  tft.setCursor(cardX + 10, cardY + 30);
  String dispSender = alertSender;
  if (dispSender.length() > 18) {
    dispSender = dispSender.substring(0, 16) + "..";
  }
  tft.print(dispSender);

  // 5. Message Body (Up to 3 lines, max 42 chars per line)
  tft.setTextSize(1);
  tft.setTextColor(COLOR_LIGHTGREY, COLOR_POPUP_BG);
  drawWrappedText(alertMessage, cardX + 10, cardY + 54, 42, 3);

  // 6. Initial countdown bar
  tft.fillRect(cardX + 4, cardY + cardH - 5, cardW - 8, 2, alertBorderColor);
}

void updatePopupTimer() {
  if (currentScreen != SCREEN_POPUP_MESSAGE) return;
  unsigned long elapsed = millis() - popupStartTime;
  if (elapsed >= POPUP_TIMEOUT_MS) {
    restoreMediaPlayerScreen();
    return;
  }

  const int cardX = 20;
  const int cardY = 23;
  const int cardW = 280;
  const int cardH = 124;
  const int totalBarW = cardW - 8; // 272 px

  int remainingW = map(POPUP_TIMEOUT_MS - elapsed, 0, POPUP_TIMEOUT_MS, 0, totalBarW);
  if (remainingW < 0) remainingW = 0;
  if (remainingW > totalBarW) remainingW = totalBarW;

  tft.fillRect(cardX + 4, cardY + cardH - 5, remainingW, 2, alertBorderColor);
  if (remainingW < totalBarW) {
    tft.fillRect(cardX + 4 + remainingW, cardY + cardH - 5, totalBarW - remainingW, 2, COLOR_POPUP_BG);
  }
}

void updateCallBanner(bool force) {
  unsigned long now = millis();
  if (force || now - lastCallPulse >= 400) {
    lastCallPulse = now;
    callPulseState = !callPulseState;
    uint16_t bannerColor = callPulseState ? 0xF800 : 0x8000; // Bright Red vs Dark Crimson
    tft.fillRect(0, 0, 320, 22, bannerColor);
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_WHITE, bannerColor);
    tft.setCursor(92, 7);
    tft.print(">>> INCOMING CALL <<<");
  }
}

void renderCallScreen() {
  tft.fillScreen(ST77XX_BLACK);

  // Ringing indicator
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
  tft.setCursor(112, 34);
  tft.print("( ( [PHONE] ) )");

  // Caller Name
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  if (callerName.length() <= 10) {
    tft.setTextSize(3);
    int x = (320 - (callerName.length() * 18)) / 2;
    tft.setCursor(x > 10 ? x : 10, 52);
  } else {
    tft.setTextSize(2);
    int x = (320 - (callerName.length() * 12)) / 2;
    tft.setCursor(x > 10 ? x : 10, 56);
  }
  tft.print(callerName);

  // Number / Label
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_CYAN, ST77XX_BLACK);
  String dispNum = callerNumber.length() > 0 ? callerNumber : "Incoming Call...";
  int subX = (320 - (dispNum.length() * 6)) / 2;
  tft.setCursor(subX > 10 ? subX : 10, 84);
  tft.print(dispNum);

  // Decline Button (Left)
  tft.fillRoundRect(35, 122, 105, 32, 6, 0x8800);
  tft.drawRoundRect(35, 122, 105, 32, 6, 0xF800);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE, 0x8800);
  tft.setCursor(65, 134);
  tft.print("DECLINE");

  // Accept Button (Right)
  tft.fillRoundRect(180, 122, 105, 32, 6, 0x0400);
  tft.drawRoundRect(180, 122, 105, 32, 6, 0x07E0);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE, 0x0400);
  tft.setCursor(215, 134);
  tft.print("ACCEPT");

  // Draw initial banner
  updateCallBanner(true);
}

void handleIncomingAlert(const String& payload) {
  int firstPipe = payload.indexOf('|');

  if (payload == "CALL_END") {
    // Only dismiss if the full-screen call takeover was active!
    // (Preserve active missed-call pop-up card if one is showing)
    if (currentScreen == SCREEN_INCOMING_CALL) {
      restoreMediaPlayerScreen();
    }
    return;
  }

  if (payload == "DISMISS") {
    if (currentScreen != SCREEN_MEDIA_PLAYER) {
      restoreMediaPlayerScreen();
    }
    return;
  }

  if (firstPipe == -1) return;

  String type = payload.substring(0, firstPipe);
  type.toUpperCase();

  int secondPipe = payload.indexOf('|', firstPipe + 1);
  String p1 = "";
  String p2 = "";

  if (secondPipe != -1) {
    p1 = payload.substring(firstPipe + 1, secondPipe);
    p2 = payload.substring(secondPipe + 1);
  } else {
    p1 = payload.substring(firstPipe + 1);
  }

  if (type == "CALL") {
    callerName = p1.length() > 0 ? p1 : "Unknown Caller";
    callerNumber = p2;
    currentScreen = SCREEN_INCOMING_CALL;
    renderCallScreen();
  } else if (type == "MISSED" || type == "MISSED_CALL") {
    // Close incoming call screen if active
    if (currentScreen == SCREEN_INCOMING_CALL) {
      currentScreen = SCREEN_MEDIA_PLAYER;
    }
    alertApp = "MISSED CALL";
    alertSender = p1.length() > 0 ? p1 : "Unknown Caller";
    alertMessage = p2.length() > 0 ? p2 : "Missed Call";
    alertBorderColor = 0xF800; // Bright Red
    alertHeaderBgColor = 0x8000; // Dark Crimson
    popupStartTime = millis();
    currentScreen = SCREEN_POPUP_MESSAGE;
    renderPopupMessage();
  } else if (type == "WA" || type == "WHATSAPP") {
    alertApp = "WHATSAPP";
    alertSender = p1.length() > 0 ? p1 : "WhatsApp Contact";
    alertMessage = p2;
    alertBorderColor = COLOR_WA_GREEN;
    alertHeaderBgColor = COLOR_WA_HEADER_BG;
    popupStartTime = millis();
    currentScreen = SCREEN_POPUP_MESSAGE;
    renderPopupMessage();
  } else if (type == "SMS") {
    alertApp = "SMS";
    alertSender = p1.length() > 0 ? p1 : "SMS Message";
    alertMessage = p2;
    alertBorderColor = COLOR_SMS_BLUE;
    alertHeaderBgColor = COLOR_SMS_HEADER_BG;
    popupStartTime = millis();
    currentScreen = SCREEN_POPUP_MESSAGE;
    renderPopupMessage();
  }
}

// ==========================================
// Setup & Loop
// ==========================================
void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0); // Non-blocking USB CDC output (prevents buffer timeout stall)
  delay(1000);

  Serial.println("\n==========================================");
  Serial.println(" ESP32-C6 1.9\" Landscape Media Player UI  ");
  Serial.println("==========================================");

  // 1. Initialize Display Backlight
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, LOW);

  // 2. Initialize Hardware SPI with ESP32-C6 pinout (40MHz High-Speed SPI)
  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  SPI.setFrequency(40000000);

  // 3. Initialize ST7789 in Landscape mode (320x170)
  tft.init(170, 320);
  tft.setRotation(SCREEN_ROTATION);
  Serial.println("[TFT] ST7789 Initialized in Landscape (320x170) @ 40MHz.");

  // 4. Generate built-in test cover art
  generateDemoCoverArt();

  // 5. Render Static Frame & Initial UI
  drawStaticLayout();
  updateConnectionStatus();
  drawCoverArt(); // Display demo cover art
  updateMetadataUI();

  // 6. Initialize NimBLE BLE Peripheral
  NimBLEDevice::init("WristWatch-C6");
  NimBLEDevice::setMTU(517);

  NimBLEServer* pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  NimBLEService* pService = pServer->createService(SERVICE_MEDIA_UUID);

  // BB01: Volume Level Write (0-100) - Enable WRITE and WRITE_NR (No Response) for instant streaming!
  NimBLECharacteristic* pVolChar = pService->createCharacteristic(
      CHAR_VOL_LEVEL_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  pVolChar->setCallbacks(new VolumeLevelCallbacks());

  // BB03: Metadata write - Enable WRITE and WRITE_NR
  NimBLECharacteristic* pTrackChar = pService->createCharacteristic(
      CHAR_TRACK_INFO_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  pTrackChar->setCallbacks(new TrackInfoCallbacks());

  // BB04: Cover Art stream write
  NimBLECharacteristic* pArtChar = pService->createCharacteristic(
      CHAR_ART_CHUNK_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  pArtChar->setCallbacks(new ArtChunkCallbacks());

  // BB05: Alert / Pop-up stream write
  NimBLECharacteristic* pAlertChar = pService->createCharacteristic(
      CHAR_ALERT_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  pAlertChar->setCallbacks(new AlertCallbacks());

  pService->start();
  NimBLEDevice::getAdvertising()->addServiceUUID(SERVICE_MEDIA_UUID);
  NimBLEDevice::startAdvertising();

  Serial.println("[BLE] Advertising as 'WristWatch-C6'. Ready for connection!");
}

void loop() {
  static unsigned long lastUiUpdate = 0;
  unsigned long now = millis();

  // 1. Process incoming alerts (WhatsApp, SMS, Phone Call)
  if (alertDataReady) {
    alertDataReady = false;
    handleIncomingAlert(pendingAlertString);
  }

  // 2. Active Screen State Handlers
  if (currentScreen == SCREEN_POPUP_MESSAGE) {
    updatePopupTimer();
  } else if (currentScreen == SCREEN_INCOMING_CALL) {
    updateCallBanner(false);
  }

  // 3. Decode & render QOI Cover Art as soon as stream finishes
  if (qoiDataReady) {
    qoiDataReady = false;
    unsigned long t0 = millis();
    if (qoi_decode_to_rgb565(qoiBuffer, qoiBytesReceived, artBuffer, ART_WIDTH, ART_HEIGHT)) {
      unsigned long decodeTime = millis() - t0;
      Serial.printf("[QOI] Decoded %d bytes in %lu ms! Lossless 120x120.\n", (int)qoiBytesReceived, decodeTime);
      if (currentScreen == SCREEN_MEDIA_PLAYER) {
        drawCoverArt();
      }
    } else {
      Serial.printf("[QOI] Decode failed for %d bytes!\n", (int)qoiBytesReceived);
    }
  }

  // Render new Cover Art as soon as stream finishes (for demo art or explicit flags)
  if (newArtReady) {
    newArtReady = false;
    if (currentScreen == SCREEN_MEDIA_PLAYER) {
      drawCoverArt();
    }
  }

  // Update reception progress bar if stream is active
  if (qoiReceiving && currentScreen == SCREEN_MEDIA_PLAYER) {
    updateArtProgress();
  }

  // Update text metadata when track title/artist changes
  if (metadataChanged) {
    metadataChanged = false;
    if (currentScreen == SCREEN_MEDIA_PLAYER) {
      updateConnectionStatus();
      updateMetadataUI();
    }
  }

  // Update volume bar instantly (< 1ms execution)
  if (volumeChanged) {
    volumeChanged = false;
    if (currentScreen == SCREEN_MEDIA_PLAYER) {
      updateVolumeUI();
    }
  }

  // Heartbeat & status refresh every 1000ms
  if (now - lastUiUpdate >= 1000) {
    lastUiUpdate = now;
    if (currentScreen == SCREEN_MEDIA_PLAYER) {
      updateConnectionStatus();
    }
  }

  delay(2);
}

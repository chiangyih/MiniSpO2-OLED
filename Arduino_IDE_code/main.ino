#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "MAX30105.h"
#include "heartRate.h"

#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

// OLED 顯示器尺寸與 I2C 位址設定
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDR 0x3C

// 建立 SSD1306 顯示器物件，使用 I2C 與設定的重置腳位
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// 蜂鳴器腳位，用於偵測到脈搏時發聲提示
const int Tonepin = 4;

// MAX30105 感測器物件
MAX30105 particleSensor;

// 顯示與感測器初始化狀態
bool oledReady = false;
bool sensorReady = false;
bool lastFingerOn = false;

// 判斷手指是否放在感測器上的紅外強度門檻
#define FINGER_ON_HIGH 9000
#define FINGER_ON_LOW 7000
// 最低可接受的血氧值，低於此視為異常
#define MINIMUM_SPO2 90.0
// BPM 至少累積幾筆再顯示，避免初始不穩
#define MIN_VALID_BEATS 3

// 儲存最近 BPM 的緩衝
const byte RATE_SIZE = 8;
byte rates[RATE_SIZE];
byte rateSpot = 0;
byte validRateCount = 0;
long lastBeat = 0;
float beatsPerMinute = 0;
int beatAvg = 0;

// SpO2 計算所需變數
int sampleCount = 0;
const int numSamples = 120; // 增加視窗降低抖動
double avered = 0, aveir = 0;
double sumirrms = 0, sumredrms = 0;
double SpO2 = 0, ESpO2 = 90.0;
const double FSpO2 = 0.7; // 平滑係數
const double frate = 0.95; // 平滑濾波係數

// 顯示更新控制
unsigned long lastDisplayUpdate = 0;
const unsigned long DISPLAY_INTERVAL = 250; // 每 250ms 更新 OLED

// 重置所有量測資料與計算狀態
void resetReadings() {
  for (byte i = 0; i < RATE_SIZE; i++) rates[i] = 0;
  rateSpot = 0;
  validRateCount = 0;
  lastBeat = 0;
  beatsPerMinute = 0;
  beatAvg = 0;

  sampleCount = 0;
  avered = 0;
  aveir = 0;
  sumirrms = 0;
  sumredrms = 0;
  SpO2 = 0;
  ESpO2 = 90.0;
}

// 文字置中輸出到 OLED
void printCentered(const char *text, int y, int textSize) {
  display.setTextSize(textSize);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, y);
  display.print(text);
}

// 顯示啟動畫面
void drawBootScreen(const char *line1, const char *line2) {
  if (!oledReady) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  printCentered(line1, 16, 1);
  printCentered(line2, 34, 1);
  display.display();
}

// 顯示主畫面內容，包含手指提示、SpO2 與 BPM
void drawMainScreen(bool fingerOn) {
  if (!oledReady) return;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);

  if (!fingerOn) {
    // 手指未放上感測器時，顯示提示文字
    printCentered("PULSE OXIMETER", 2, 1);
    display.drawFastHLine(0, 13, 128, SSD1306_WHITE);
    printCentered("PLACE FINGER", 26, 1);
    printCentered("ON SENSOR", 42, 1);
    display.display();
    return;
  }

  // 顯示標題與分隔線
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("SpO2");
  display.setCursor(93, 0);
  display.print("BPM");
  display.drawFastHLine(0, 10, 128, SSD1306_WHITE);

  // 顯示血氧值
  display.setTextSize(3);
  display.setCursor(0, 20);
  bool spo2Low = false;
  if (validRateCount >= MIN_VALID_BEATS) {
    if (ESpO2 >= MINIMUM_SPO2) {
      display.print((int)(ESpO2 + 0.5));
    } else {
      display.print("LOW");
      spo2Low = true;
    }
  } else {
    display.print("--");
  }

  display.setTextSize(2);
  display.setCursor(48, 28);
  if (!spo2Low) {
    display.print("%");
  } else {
    display.print(" ");
  }

  display.drawFastVLine(70, 12, 52, SSD1306_WHITE);

  // 顯示 BPM
  display.setTextSize(3);
  display.setCursor(78, 20);
  if (validRateCount >= MIN_VALID_BEATS) {
    if (beatAvg < 100) display.print(" ");
    display.print(beatAvg);
  } else {
    display.print("--");
  }

  // 顯示狀態提示
  display.setTextSize(1);
  display.setCursor(0, 56);
  if (validRateCount >= MIN_VALID_BEATS) {
    display.print("Keep still");
  } else {
    display.print("Measuring...");
  }

  // 顯示心搏閃爍圈
  if (millis() - lastBeat < 180) {
    display.fillCircle(122, 58, 3, SSD1306_WHITE);
  } else {
    display.drawCircle(122, 58, 3, SSD1306_WHITE);
  }

  display.display();
}

void setup() {
  Serial.begin(115200);

  // 初始化 OLED 顯示器
  oledReady = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (oledReady) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    drawBootScreen("Hello", "Starting...");
  } else {
    Serial.println("OLED not found. Check I2C address/wiring.");
  }

  // 初始化 MAX30105 感測器，使用 I2C 快速模式
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("MAX30105 was not found. Check wiring/power.");
    drawBootScreen("SENSOR ERROR", "CHECK WIRING");
    return;
  }

  sensorReady = true;

  // 設定感測器參數：亮度、平均、LED 模式、取樣率、脈波寬度、ADC 範圍
  particleSensor.setup(0x7F, 4, 2, 800, 215, 16384);
  particleSensor.enableDIETEMPRDY();
  particleSensor.setPulseAmplitudeRed(0x24);
  particleSensor.setPulseAmplitudeIR(0x24);
  particleSensor.setPulseAmplitudeGreen(0);

  delay(800);
  drawMainScreen(false);
}

// 主迴圈
void loop() {
  if (!sensorReady) {
    delay(1000);
    return;
  }

  // 讀取紅外線強度，判斷是否手指放上傳感器
  long irValue = particleSensor.getIR();
  // 使用滯回避免門檻附近抖動：ON 用高門檻，保持 ON 用低門檻
  bool fingerOn = lastFingerOn ? (irValue > FINGER_ON_LOW) : (irValue > FINGER_ON_HIGH);

  // 手指狀態切換時，重置資料並清空 FIFO，避免殘留舊資料干擾
  if (fingerOn != lastFingerOn) {
    resetReadings();
    particleSensor.clearFIFO();
    lastFingerOn = fingerOn;
  }

  if (fingerOn) {
    // 若偵測到脈搏，計算心跳速率
    if (checkForBeat(irValue)) {
      unsigned long now = millis();

      // 第一個脈搏只記錄時間，不做 BPM 計算
      if (lastBeat == 0) {
        lastBeat = now;
      } else {
        unsigned long delta = now - lastBeat;
        lastBeat = now;

        if (delta > 0) {
          beatsPerMinute = 60.0 / (delta / 1000.0);

          if (beatsPerMinute > 20 && beatsPerMinute < 255) {
            rates[rateSpot++] = (byte)beatsPerMinute;
            rateSpot %= RATE_SIZE;
            if (validRateCount < RATE_SIZE) validRateCount++;

            int total = 0;
            for (byte i = 0; i < validRateCount; i++) total += rates[i];
            beatAvg = total / validRateCount;

            tone(Tonepin, 1000, 10);
          }
        }
      }
    }

    // 讀取感測器 FIFO 緩衝資料
    particleSensor.check();
    while (particleSensor.available()) {
      uint32_t red = particleSensor.getFIFORed();
      uint32_t ir = particleSensor.getFIFOIR();

      double fred = (double)red;
      double fir = (double)ir;

      // 使用指數移動平均濾波
      avered = avered * frate + fred * (1.0 - frate);
      aveir = aveir * frate + fir * (1.0 - frate);

      // 累計平方差以計算 RMS
      sumredrms += (fred - avered) * (fred - avered);
      sumirrms += (fir - aveir) * (fir - aveir);

      sampleCount++;
      if (sampleCount >= numSamples) {
        if (avered > 0 && aveir > 0 && sumirrms > 0) {
          double R = (sqrt(sumredrms) / avered) / (sqrt(sumirrms) / aveir);
          SpO2 = -23.3 * (R - 0.4) + 100.0;
          ESpO2 = FSpO2 * ESpO2 + (1.0 - FSpO2) * SpO2;

          if (ESpO2 < 0.0) ESpO2 = 0.0;
          if (ESpO2 > 100.0) ESpO2 = 99.9;
        }

        // 重置樣本統計
        sumredrms = 0.0;
        sumirrms = 0.0;
        sampleCount = 0;
      }

      particleSensor.nextSample();
    }
  } else {
    // 手指不在感測器上，狀態切換時已重置
  }

  // 定時更新畫面
  if (millis() - lastDisplayUpdate >= DISPLAY_INTERVAL) {
    drawMainScreen(fingerOn);
    lastDisplayUpdate = millis();
  }
}

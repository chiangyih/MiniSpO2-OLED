#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "MAX30105.h"
#include "heartRate.h"


#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

//螢幕
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDR 0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

//初始
const int Tonepin = 4; 
MAX30105 particleSensor;
bool oledReady = false;
bool sensorReady = false;

// 血氧
#define FINGER_ON 7000          
#define MINIMUM_SPO2 90.0   

const byte RATE_SIZE = 8;
byte rates[RATE_SIZE];
byte rateSpot = 0;
byte validRateCount = 0;
long lastBeat = 0;
float beatsPerMinute = 0;
int beatAvg = 0;

int sampleCount = 0;
const int numSamples = 30;
double avered = 0, aveir = 0;
double sumirrms = 0, sumredrms = 0;
double SpO2 = 0, ESpO2 = 90.0;
const double FSpO2 = 0.7;
const double frate = 0.95;

unsigned long lastDisplayUpdate = 0;
const unsigned long DISPLAY_INTERVAL = 250; // 刷新250ms

// 工具
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

void printCentered(const char *text, int y, int textSize) {
  display.setTextSize(textSize);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, y);
  display.print(text);
}

void drawBootScreen(const char *line1, const char *line2) {
  if (!oledReady) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  printCentered(line1, 16, 1);
  printCentered(line2, 34, 1);
  display.display();
}

void drawMainScreen(bool fingerOn) {
  if (!oledReady) return;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);

  if (!fingerOn) {
    printCentered("PULSE OXIMETER", 2, 1);
    display.drawFastHLine(0, 13, 128, SSD1306_WHITE);
    printCentered("PLACE FINGER", 26, 1);
    printCentered("ON SENSOR", 42, 1);
    display.display();
    return;
  }

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("SpO2");
  display.setCursor(93, 0);
  display.print("BPM");
  display.drawFastHLine(0, 10, 128, SSD1306_WHITE);

  display.setTextSize(3);
  display.setCursor(0, 20);
  if (beatAvg > 30 && ESpO2 >= 90.0) {
    display.print((int)(ESpO2 + 0.5));
  } else {
    display.print("--");
  }

  display.setTextSize(2);
  display.setCursor(48, 28);
  display.print("%");

  display.drawFastVLine(70, 12, 52, SSD1306_WHITE);
  display.setTextSize(3);
  display.setCursor(78, 20);
  if (beatAvg > 0) {
    if (beatAvg < 100) display.print(" ");
    display.print(beatAvg);
  } else {
    display.print("--");
  }


  display.setTextSize(1);
  display.setCursor(0, 56);
  if (beatAvg > 30) {
    display.print("Keep still");
  } else {
    display.print("Measuring...");
  }


  if (millis() - lastBeat < 180) {
    display.fillCircle(122, 58, 3, SSD1306_WHITE);
  } else {
    display.drawCircle(122, 58, 3, SSD1306_WHITE);
  }

  display.display();
}

void setup() {
  Serial.begin(115200); 

  oledReady = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (oledReady) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    drawBootScreen("Hello", "Starting...");
  } else {
    Serial.println("OLED not found. Check I2C address/wiring.");
  }

  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("MAX30105 was not found. Check wiring/power.");
    drawBootScreen("SENSOR ERROR", "CHECK WIRING");
    return;
  }

  sensorReady = true;

  // ledBrightness, sampleAverage, ledMode, sampleRate, pulseWidth, adcRange
  particleSensor.setup(0x7F, 4, 2, 800, 215, 16384);
  particleSensor.enableDIETEMPRDY();
  particleSensor.setPulseAmplitudeRed(0x24);
  particleSensor.setPulseAmplitudeIR(0x24);
  particleSensor.setPulseAmplitudeGreen(0);

  delay(800);
  drawMainScreen(false);
}

// mainloop
void loop() {
  if (!sensorReady) {
    delay(1000);
    return;
  }

  long irValue = particleSensor.getIR();
  bool fingerOn = (irValue > FINGER_ON);

  if (fingerOn) {
    if (checkForBeat(irValue)) {
      long delta = millis() - lastBeat;
      lastBeat = millis();

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

    
    particleSensor.check();
    while (particleSensor.available()) {
      uint32_t red = particleSensor.getFIFORed();
      uint32_t ir = particleSensor.getFIFOIR();

      double fred = (double)red;
      double fir = (double)ir;

      avered = avered * frate + fred * (1.0 - frate);
      aveir = aveir * frate + fir * (1.0 - frate);

      sumredrms += (fred - avered) * (fred - avered);
      sumirrms += (fir - aveir) * (fir - aveir);

      sampleCount++;
      if (sampleCount >= numSamples) {
        if (avered > 0 && aveir > 0 && sumirrms > 0) {
          double R = (sqrt(sumredrms) / avered) / (sqrt(sumirrms) / aveir);
          SpO2 = -23.3 * (R - 0.4) + 100.0;
          ESpO2 = FSpO2 * ESpO2 + (1.0 - FSpO2) * SpO2;

          if (ESpO2 <= MINIMUM_SPO2) ESpO2 = MINIMUM_SPO2;
          if (ESpO2 > 100.0) ESpO2 = 99.9;
        }

        sumredrms = 0.0;
        sumirrms = 0.0;
        sampleCount = 0;
      }

      particleSensor.nextSample();
    }
  } else {
    resetReadings();
  }

  if (millis() - lastDisplayUpdate >= DISPLAY_INTERVAL) {
    drawMainScreen(fingerOn);
    lastDisplayUpdate = millis();
  }
}

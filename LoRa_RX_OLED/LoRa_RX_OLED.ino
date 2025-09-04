/*
  LoRa Receiver (ESP32 T-Display + SSD1306 + SX127x)
  - Updates OLED ONLY when a new packet comes in
  - Keeps last message visible until the next one arrives
  - SF = 8, Freq = 433 MHz
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SPI.h>
#include <LoRa.h>

#define FREQ_HZ      433E6
#define LORA_SYNC    0xA5
#define LORA_SF      8

// --- Wiring ---
#define SCK   5
#define MISO  19
#define MOSI  27
#define SS    18
#define RST   14
#define DIO0  26

// OLED
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

void showMsg(const String& msg, int rssi, float snr) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("RX <-");
  display.println(msg);
  display.print("RSSI: "); display.print(rssi); display.println(" dBm");
  display.print("SNR : "); display.print(snr, 1); display.println(" dB");
  display.display();                  // <-- draw once; no clearing after
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {}

  // OLED
  Wire.begin();
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 allocation failed");
    for(;;);
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("RX booting...");
  display.display();

  // LoRa
  SPI.begin(SCK, MISO, MOSI, SS);
  LoRa.setPins(SS, RST, DIO0);
  if (!LoRa.begin(FREQ_HZ)) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("LoRa init FAILED");
    display.display();
    Serial.println("LoRa init FAILED");
    for(;;);
  }
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSyncWord(LORA_SYNC);
  LoRa.enableCrc();

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("RX ready");
  display.println("433 MHz, SF=8");
  display.display();
  Serial.println("LoRa RX ready @ 433 MHz, SF=8");
}

void loop() {
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    String msg;
    while (LoRa.available()) {
      msg += (char)LoRa.read();
    }
    int rssi = LoRa.packetRssi();
    float snr = LoRa.packetSnr();

    Serial.printf("[RX] %s | RSSI %d dBm | SNR %.1f dB\n", msg.c_str(), rssi, snr);
    showMsg(msg, rssi, snr);   // draw once; leave it displayed
  }

  // No display clearing here — last message stays on screen
  // (Optional) yield();  // keep WiFi/RTOS happy if we later add other tasks
}

/*
  LoRa Ping-Pong Responder (ESP32 T-Display + SSD1306 + SX127x)
  - Listens for "Hello N!"
  - Replies with "Ack N"
  - Keeps last message on OLED until next arrives
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SPI.h>
#include <LoRa.h>

#define FREQ_HZ      433E6
#define LORA_SYNC    0xA5
#define LORA_SF      8

// --- Wiring (your map) ---
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

void showRx(const String& msg, int rssi, float snr) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("RX <- " + msg);
  display.print("RSSI: "); display.print(rssi); display.println(" dBm");
  display.print("SNR : "); display.print(snr, 1); display.println(" dB");
  display.display();
}

long extractNumber(const String& s) {
  long val = 0; bool seen = false;
  for (uint16_t i=0;i<s.length();++i) {
    char c = s[i];
    if (c >= '0' && c <= '9') { val = val*10 + (c - '0'); seen = true; }
    else if (seen) break;
  }
  return seen ? val : -1;
}

void sendAck(long n) {
  String ack = "Ack " + String(n);
  LoRa.beginPacket();
  LoRa.print(ack);
  LoRa.endPacket();
  Serial.println("[TX] " + ack);
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {}

  // OLED
  Wire.begin();
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { Serial.println("SSD1306 fail"); for(;;); }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Responder boot...");
  display.display();

  // LoRa
  SPI.begin(SCK, MISO, MOSI, SS);
  LoRa.setPins(SS, RST, DIO0);
  if (!LoRa.begin(FREQ_HZ)) { display.clearDisplay(); display.setCursor(0,0); display.println("LoRa init FAIL"); display.display(); for(;;); }
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSyncWord(LORA_SYNC);
  LoRa.enableCrc();
  LoRa.setTxPower(17, PA_OUTPUT_PA_BOOST_PIN);

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Responder ready");
  display.println("433 MHz, SF=8");
  display.display();
}

void loop() {
  int sz = LoRa.parsePacket();
  if (sz) {
    String msg;
    while (LoRa.available()) msg += (char)LoRa.read();
    int rssi = LoRa.packetRssi();
    float snr = LoRa.packetSnr();

    Serial.printf("[RX] %s | RSSI %d | SNR %.1f\n", msg.c_str(), rssi, snr);
    showRx(msg, rssi, snr);

    long n = extractNumber(msg);
    if (msg.startsWith("Hello") && n >= 0) {
      delay(10);            // brief guard to let RX settle before TX
      sendAck(n);           // reply with matching Ack N
      // after TX, LoRa lib returns to listening automatically
    }
  }
}

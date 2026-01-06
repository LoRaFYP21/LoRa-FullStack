/*
  LoRa Ping-Pong Initiator (ESP32 T-Display + SSD1306 + SX127x)
  - Sends "Hello {count}!" every ~2 s
  - Waits for "Ack {count}" with timeout + retry
  - Keeps last status on OLED until updated
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

// Timing
const unsigned long PERIOD_MS      = 2000;   // time between new Hello cycles
const unsigned long ACK_TIMEOUT_MS = 800;    // how long to wait for Ack
const uint8_t       MAX_RETRIES    = 2;

enum State { IDLE, WAIT_ACK };
State state = IDLE;

uint32_t count = 0;
uint8_t  retries = 0;
unsigned long nextSendAt = 0;
unsigned long ackDeadline = 0;

void oled3(const String& a, const String& b="", const String& c="") {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(a);
  if (b.length()) display.println(b);
  if (c.length()) display.println(c);
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

void sendHello(uint32_t n) {
  String msg = "Hello " + String(n) + "!";
  LoRa.beginPacket();
  LoRa.print(msg);
  LoRa.endPacket();
  Serial.println("[TX] " + msg);
  oled3("TX ->", msg, "Waiting Ack...");
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {}

  // OLED
  Wire.begin();
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { Serial.println("SSD1306 fail"); for(;;); }
  oled3("Initiator boot...", "OLED OK");

  // LoRa
  SPI.begin(SCK, MISO, MOSI, SS);
  LoRa.setPins(SS, RST, DIO0);
  if (!LoRa.begin(FREQ_HZ)) { oled3("LoRa init FAILED","Check wiring/freq"); for(;;); }
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSyncWord(LORA_SYNC);
  LoRa.enableCrc();
  LoRa.setTxPower(17, PA_OUTPUT_PA_BOOST_PIN);

  oled3("Initiator ready", "433 MHz, SF=8", "Sync=0xA5");
  nextSendAt = millis();   // start right away
}

void loop() {
  unsigned long now = millis();

  if (state == IDLE) {
    if ((long)(now - nextSendAt) >= 0) {
      retries = 0;
      sendHello(count);
      ackDeadline = now + ACK_TIMEOUT_MS;
      state = WAIT_ACK;
    }
  } else if (state == WAIT_ACK) {
    int sz = LoRa.parsePacket();
    if (sz) {
      String msg;
      while (LoRa.available()) msg += (char)LoRa.read();
      int rssi = LoRa.packetRssi();
      float snr = LoRa.packetSnr();
      Serial.printf("[RX] %s | RSSI %d | SNR %.1f\n", msg.c_str(), rssi, snr);

      long n = extractNumber(msg);
      if (msg.startsWith("Ack") && n == (long)count) {
        oled3("ACK OK", "Ack " + String(n), "RSSI " + String(rssi) + " dBm");
        // success → next cycle in ~PERIOD_MS
        count++;
        nextSendAt = millis() + PERIOD_MS;
        state = IDLE;
      } else {
        // Not the ack we expect; ignore and keep waiting
      }
    } else if ((long)(now - ackDeadline) >= 0) {
      if (retries < MAX_RETRIES) {
        retries++;
        Serial.println("ACK timeout, retrying...");
        sendHello(count);
        ackDeadline = millis() + ACK_TIMEOUT_MS;
      } else {
        Serial.println("ACK timeout, giving up this round.");
        oled3("ACK TIMEOUT", "Will continue...", "");
        // advance count anyway (or keep same if you prefer)
        count++;
        nextSendAt = millis() + PERIOD_MS;
        state = IDLE;
      }
    }
  }
}

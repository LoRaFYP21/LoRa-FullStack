/*
  LoRa Transmitter (ESP32 T-Display + SSD1306 + SX127x)
  - Sends "Hello {count}!" every ~2 s
  - SF = 8
  - Prints to Serial and OLED
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SPI.h>
#include <LoRa.h>

#define FREQ_HZ      433E6
#define LORA_SYNC    0xA5   // keep same on RX
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

void setup() {
  Serial.begin(115200);
  while (!Serial) {}

  // OLED
  Wire.begin();
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 allocation failed");
    for(;;);
  }
  oled3("TX booting...", "OLED OK");

  // LoRa
  SPI.begin(SCK, MISO, MOSI, SS);
  LoRa.setPins(SS, RST, DIO0);
  if (!LoRa.begin(FREQ_HZ)) {
    oled3("LoRa init FAILED", "Check wiring/freq");
    Serial.println("LoRa init FAILED");
    for(;;);
  }
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSyncWord(LORA_SYNC);
  LoRa.enableCrc();
  // Optional: adjust power (use PA_BOOST pin on most SX127x breakouts)
  LoRa.setTxPower(17, PA_OUTPUT_PA_BOOST_PIN);

  oled3("TX ready", "433 MHz", "SF=8 Sync=0xA5");
  Serial.println("LoRa TX ready @ 433 MHz, SF=8");
}

void loop() {
  static uint32_t count = 0;
  static unsigned long t0 = 0;
  const unsigned long PERIOD_MS = 2000;

  if (millis() - t0 >= PERIOD_MS) {
    t0 = millis();
    String msg = "Hello " + String(count) + "!";
    LoRa.beginPacket();
    LoRa.print(msg);
    LoRa.endPacket(); // blocking send

    Serial.println("[TX] " + msg);
    oled3("TX ->", msg, "(~2s)");
    count++;
  }
}

/*
  LoRa Transparent Relay with Hop Counting

  Place this node between Node A and Node C.
  It:
    - Listens on the same frequency/SF/sync as endpoints
    - Parses header H<hops>:payload
    - Increments hop and forwards
    - Logs hop count and RSSI/SNR

  Hardware: ESP32 T-Display + SX127x, same wiring as endpoints.
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SPI.h>
#include <LoRa.h>

// ---------- Radio config (must match endpoints) ----------
#define FREQ_HZ   923E6
#define LORA_SYNC 0xA5
#define LORA_SF   8

// Wiring (LilyGo T-Display -> SX127x)
#define SCK   5
#define MISO  19
#define MOSI  27
#define SS    18
#define RST   14
#define DIO0  26

// ---------- OLED ----------
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

static void oled3(const String& a, const String& b="", const String& c="") {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0);
  display.println(a);
  if(b.length()) display.println(b);
  if(c.length()) display.println(c);
  display.display();
}

// Hop header helpers: H<hops>:payload
static bool stripHopHeader(const String& in, String& outPayload, int& hopCount){
  hopCount = 0;
  if(in.length() < 3 || in.charAt(0)!='H'){
    outPayload = in;
    return false;
  }
  int colon = in.indexOf(':');
  if(colon <= 1){
    outPayload = in;
    return false;
  }
  String hopStr = in.substring(1, colon);
  int h = hopStr.toInt();
  if(h < 0) h = 0;
  hopCount = h;
  outPayload = in.substring(colon+1);
  return true;
}

static void sendLoRaRawWrapped(const String& payload, int hop){
  if(hop < 0) hop = 0;
  String wrap = "H" + String(hop) + ":" + payload;
  LoRa.beginPacket();
  LoRa.print(wrap);
  LoRa.endPacket();
}

void setup(){
  Serial.begin(115200);
  Serial.setTimeout(10);

  Wire.begin();
  if(!display.begin(SSD1306_SWITCHCAPVCC,0x3C)){
    Serial.println("SSD1306 fail");
    for(;;);
  }

  SPI.begin(SCK,MISO,MOSI,SS);
  LoRa.setPins(SS,RST,DIO0);
  if(!LoRa.begin(FREQ_HZ)){
    oled3("Relay: LoRa FAIL","Check wiring/freq","");
    for(;;);
  }
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSyncWord(LORA_SYNC);
  LoRa.enableCrc();
  LoRa.setTxPower(17, PA_OUTPUT_PA_BOOST_PIN);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);

  oled3("LoRa RELAY Ready","923 MHz, SF="+String(LORA_SF),"Transparent repeater");
  Serial.println("=== LoRa Transparent Relay with Hops ===");
}

void loop(){
  int packetSize = LoRa.parsePacket();
  if(packetSize){
    String rawPkt;
    while(LoRa.available()) rawPkt += (char)LoRa.read();

    int rssi = LoRa.packetRssi();
    float snr = LoRa.packetSnr();

    String payload;
    int hopIn = 0;
    stripHopHeader(rawPkt, payload, hopIn);
    
    // Check if this is an ACK or ACKF message
    bool isAck = payload.startsWith("ACK,");
    bool isAckF = payload.startsWith("ACKF,");
    
    // For ACK/ACKF: don't increment hop, keep original for routing
    // For MSG/MSGF: increment hop count
    int hopOut = (isAck || isAckF) ? hopIn : (hopIn + 1);

    Serial.print("[RELAY RX] hopIn=");
    Serial.print(hopIn);
    Serial.print(" | RSSI=");
    Serial.print(rssi);
    Serial.print(" SNR=");
    Serial.print(snr,1);
    Serial.print(" | type=");
    Serial.print(isAck ? "ACK" : (isAckF ? "ACKF" : "MSG"));
    Serial.print(" | payload=\"");
    Serial.print(payload);
    Serial.println("\"");

    oled3("Relay RX h="+String(hopIn),
          payload.substring(0,16),
          "RSSI "+String(rssi)+" SNR "+String(snr,1));

    delay(5); // small guard before re-tx

    sendLoRaRawWrapped(payload, hopOut);

    Serial.print("[RELAY TX] hopOut=");
    Serial.print(hopOut);
    Serial.print(" | type=");
    Serial.print(isAck ? "ACK" : (isAckF ? "ACKF" : "MSG"));
    Serial.print(" | payload=\"");
    Serial.print(payload);
    Serial.println("\"");
    Serial.println();
  }

  delay(1);
}
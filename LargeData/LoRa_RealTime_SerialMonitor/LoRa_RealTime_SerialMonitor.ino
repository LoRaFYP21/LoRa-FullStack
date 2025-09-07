send just 200 txt by serial monitor: 
/*
  LoRa Serial Chat + Auto-ACK + Sizes + Throughput
  ESP32 T-Display + SSD1306 + SX127x (Sandeep Mistry LoRa lib)

  Type in Serial Monitor (Newline) -> send as MSG
  Peer shows it (OLED+Serial) and auto-sends ACK including its rxBytesTotal.
  Sender shows ACK OK + computes "goodput" (acknowledged payload bits/sec).

  Packet formats:
    MSG,<srcId>,<dstId>,<seq>,<text>
    ACK,<srcId>,<dstId>,<seq>,<rxBytesTotal>

  Notes:
   - For two nodes we use <dstId>=FF (broadcast). ACKs are directed to sender.
   - We sanitize commas in user text (replace with space) so CSV parsing is safe.
   - Each device keeps independent local counters (txBytesTotal/rxBytesTotal).
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SPI.h>
#include <LoRa.h>

// ----------- CONFIG -----------
#define FREQ_HZ      433E6
#define LORA_SYNC    0xA5
#define LORA_SF      8

// Your wiring (as you used successfully)
#define SCK   5
#define MISO  19
#define MOSI  27
#define SS    18
#define RST   14
#define DIO0  26

// Optional: hardcode an ID if you still see duplicates after using MAC
// #define MY_NODE_ID "NODE_A"   // <= uncomment & set to force an ID

// ----------- OLED ------------
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

static void oled3(const String& a, const String& b="", const String& c="") {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0);
  display.println(a);
  if (b.length()) display.println(b);
  if (c.length()) display.println(c);
  display.display();
}

// ----------- IDs & state -----------
String myId   = "????????";   // 8-hex chars from MAC (4 bytes)
String dstAny = "FF";         // broadcast for two-node simplicity

// Per-device independent tallies (payload bytes of MSG text only)
uint32_t txPkts = 0, rxPkts = 0;
uint64_t txBytesTotal = 0, rxBytesTotal = 0;

// For ACK/throughput
uint32_t txSeq = 0;
bool     waitingAck = false;
uint32_t waitingSeq = 0;
unsigned long ackDeadline = 0;
unsigned long sessionStartMs = 0;

const unsigned long ACK_TIMEOUT_MS = 1500;
const size_t        MAX_TEXT_LEN   = 200;

// ----------- Helpers -----------
static String bytesToHuman(uint64_t B) {
  // simple SI units
  if (B >= 1000000ULL)  return String((double)B/1000000.0, 3) + " MB";
  if (B >= 1000ULL)     return String((double)B/1000.0, 3) + " kB";
  return String((uint64_t)B) + " B";
}

static String bitsToHuman(uint64_t bits) {
  if (bits >= 1000000ULL) return String((double)bits/1000000.0, 3) + " Mb";
  if (bits >= 1000ULL)    return String((double)bits/1000.0, 3) + " kb";
  return String((uint64_t)bits) + " b";
}

static String speedToHuman(double bps) {
  if (bps >= 1e6) return String(bps/1e6, 3) + " Mb/s";
  if (bps >= 1e3) return String(bps/1e3, 3) + " kb/s";
  return String(bps, 0) + " b/s";
}

static void macTo8Hex(String& out) {
  // use lower 4 bytes of efuse MAC -> 8 hex chars (much lower collision chance)
  uint64_t mac = ESP.getEfuseMac(); // 48-bit unique
  uint32_t low = (uint32_t)(mac & 0xFFFFFFFFULL);
  char buf[9];
  sprintf(buf, "%08X", low);
  out = String(buf);
}

// sanitize commas/newlines so CSV parsing stays simple
static void sanitizeText(String& s) {
  for (uint16_t i=0;i<s.length();++i) {
    char c = s[i];
    if (c == ',' || c == '\r' || c == '\n') s.setCharAt(i, ' ');
  }
}

static void sendLoRa(const String& payload) {
  LoRa.beginPacket();
  LoRa.print(payload);
  LoRa.endPacket();
}

static long toLong(const String& s) {
  long v=0; bool seen=false;
  for (uint16_t i=0;i<s.length();++i) {
    char c=s[i];
    if (c>='0' && c<='9') { v=v*10+(c-'0'); seen=true; }
    else if (seen) break;
  }
  return seen ? v : -1;
}

// MSG: "MSG,<src>,<dst>,<seq>,<text>"
static bool parseMSG(const String& in, String& src, String& dst, long& seq, String& text) {
  if (!in.startsWith("MSG,")) return false;
  int p1=in.indexOf(',',4);  if(p1<0) return false;
  int p2=in.indexOf(',',p1+1); if(p2<0) return false;
  int p3=in.indexOf(',',p2+1); if(p3<0) return false;
  src = in.substring(4,p1);
  dst = in.substring(p1+1,p2);
  String seqStr = in.substring(p2+1,p3);
  text = in.substring(p3+1);
  seq = toLong(seqStr);
  return true;
}

// ACK: "ACK,<src>,<dst>,<seq>,<rxBytesTotal>"
static bool parseACK(const String& in, String& src, String& dst, long& seq, uint64_t& rxTot) {
  if (!in.startsWith("ACK,")) return false;
  int p1=in.indexOf(',',4);  if(p1<0) return false;
  int p2=in.indexOf(',',p1+1); if(p2<0) return false;
  int p3=in.indexOf(',',p2+1); if(p3<0) return false;
  src = in.substring(4,p1);
  dst = in.substring(p1+1,p2);
  String seqStr = in.substring(p2+1,p3);
  String totStr = in.substring(p3+1);
  seq = toLong(seqStr);
  rxTot = (uint64_t) totStr.toDouble(); // totStr is integer text; toDouble handles big numbers ok
  return true;
}

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(10);

  Wire.begin();
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 init failed");
    for(;;);
  }

#ifdef MY_NODE_ID
  myId = String(MY_NODE_ID);
#else
  macTo8Hex(myId);           // unique 8-hex ID from MAC
#endif

  SPI.begin(SCK, MISO, MOSI, SS);
  LoRa.setPins(SS, RST, DIO0);
  if (!LoRa.begin(FREQ_HZ)) {
    oled3("LoRa init FAILED","Check wiring/freq");
    Serial.println("LoRa init FAILED");
    for(;;);
  }
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSyncWord(LORA_SYNC);
  LoRa.enableCrc();
  LoRa.setTxPower(17, PA_OUTPUT_PA_BOOST_PIN);

  sessionStartMs = millis();

  oled3("LoRa Chat Ready", "ID: " + myId, "433 MHz, SF=8");
  Serial.println("=== LoRa Chat Ready ===");
  Serial.println("Set Serial Monitor to 115200, Newline. Type and Enter.");
  Serial.print("Node ID: "); Serial.println(myId);
}

void loop() {
  // ----- 1) Serial -> LoRa -----
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      sanitizeText(line);                // commas/newlines -> spaces
      if (line.length() > MAX_TEXT_LEN) line = line.substring(0, MAX_TEXT_LEN);

      // Build MSG CSV
      String payload = "MSG," + myId + "," + dstAny + "," + String(txSeq) + "," + line;

      // Send
      sendLoRa(payload);

      // Local per-packet + totals (payload is whole CSV; we only count text for txBytesTotal)
      size_t pktBytes = payload.length();
      size_t textBytes = line.length();
      txPkts++;
      txBytesTotal += textBytes;

      // Prints
      Serial.printf("[TX] #%lu \"%s\"\n", (unsigned long)txSeq, line.c_str());
      Serial.printf("     Packet: %u bytes (%s, %s)\n",
                    (unsigned)pktBytes,
                    bitsToHuman((uint64_t)pktBytes*8).c_str(),
                    bytesToHuman(pktBytes).c_str());
      Serial.printf("     Text:   %u bytes (%s)\n",
                    (unsigned)textBytes,
                    bytesToHuman(textBytes).c_str());

      oled3("TX -> (" + String(txSeq) + ")", line,
            "pkt " + String(pktBytes) + "B | txt " + String(textBytes) + "B");

      // Start ACK wait
      waitingAck  = true;
      waitingSeq  = txSeq;
      ackDeadline = millis() + ACK_TIMEOUT_MS;
      txSeq++;
    }
  }

  // ----- 2) LoRa -> handle incoming -----
  int sz = LoRa.parsePacket();
  if (sz) {
    String pkt;
    while (LoRa.available()) pkt += (char)LoRa.read();
    int rssi = LoRa.packetRssi();
    float snr = LoRa.packetSnr();

    // Try ACK parse first
    String aSrc, aDst; long aSeq = -1; uint64_t peerRxTot = 0;
    if (parseACK(pkt, aSrc, aDst, aSeq, peerRxTot)) {
      if (aDst == myId && waitingAck && aSeq == (long)waitingSeq) {
        waitingAck = false;

        // Compute "goodput" from peer's reported rxBytesTotal (text bytes received)
        unsigned long elapsedMs = millis() - sessionStartMs;
        double bps = (elapsedMs > 0)
                     ? ( (double)peerRxTot * 8.0 * 1000.0 / (double)elapsedMs )
                     : 0.0;

        Serial.printf("[ACK OK] #%ld from %s | peerRxTot=%llu bytes | %s | RSSI %d | SNR %.1f\n",
                      aSeq, aSrc.c_str(),
                      (unsigned long long)peerRxTot,
                      speedToHuman(bps).c_str(),
                      rssi, snr);

        oled3("ACK OK (" + String(aSeq) + ")",
              "peer RX " + bytesToHuman(peerRxTot),
              speedToHuman(bps));
      } else {
        // stray/old ACK
        Serial.printf("[ACK] %s -> %s | #%ld | rxTot=%llu | RSSI %d | SNR %.1f\n",
                      aSrc.c_str(), aDst.c_str(), aSeq,
                      (unsigned long long)peerRxTot, rssi, snr);
      }
    }
    else {
      // Try MSG
      String mSrc, mDst, mText; long mSeq = -1;
      if (parseMSG(pkt, mSrc, mDst, mSeq, mText)) {
        // Per-packet counts (we treat "text" length as delivered app payload)
        size_t pktBytes = pkt.length();
        size_t textBytes = mText.length();
        rxPkts++;
        rxBytesTotal += textBytes;

        Serial.printf("[RX] #%ld from %s: \"%s\"\n", mSeq, mSrc.c_str(), mText.c_str());
        Serial.printf("     Packet: %u bytes (%s, %s)\n",
                      (unsigned)pktBytes,
                      bitsToHuman((uint64_t)pktBytes*8).c_str(),
                      bytesToHuman(pktBytes).c_str());
        Serial.printf("     Text:   %u bytes (%s) | rxTotal=%llu bytes\n",
                      (unsigned)textBytes,
                      bytesToHuman(textBytes).c_str(),
                      (unsigned long long)rxBytesTotal);

        oled3("RX <- (" + String(mSeq) + ")", mText,
              "pkt " + String(pktBytes) + "B | txt " + String(textBytes) + "B");

        // Auto-ACK back to sender with my rxBytesTotal (text bytes)
        String ack = "ACK," + myId + "," + mSrc + "," + String(mSeq) + "," + String((unsigned long long)rxBytesTotal);
        sendLoRa(ack);
        Serial.printf("[TX] %s\n", ack.c_str());
      } else {
        // Unknown payload
        Serial.printf("[RX RAW] %s | %u bytes | RSSI %d | SNR %.1f\n",
                      pkt.c_str(), (unsigned)pkt.length(), rssi, snr);
      }
    }
  }

  // ----- 3) ACK timeout -----
  if (waitingAck && (long)(millis() - ackDeadline) >= 0) {
    waitingAck = false;
    Serial.printf("[ACK TIMEOUT] seq #%lu (no ack)\n", (unsigned long)waitingSeq);
    oled3("ACK TIMEOUT", "seq " + String(waitingSeq), "");
  }

  delay(1);
}

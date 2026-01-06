/*
  LoRa Serial Chat (Reliable Fragments + Exact Tries) + PDR + Goodput + Distance
  === FIXES: Added RX Delay for ACK stability + Abort on Frag Fail (No Loop Retry) ===
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SPI.h>
#include <LoRa.h>

// ---------- Radio config (AS923) ----------
#define FREQ_HZ 923E6
#define LORA_SYNC 0xA5
#define LORA_SF 8                    // raise if link is weak (9..12)
const size_t LORA_MAX_PAYLOAD = 255; // SX127x FIFO/payload byte limit

// Wiring (LilyGo T-Display -> SX127x)
#define SCK 5
#define MISO 19
#define MOSI 27
#define SS 18
#define RST 14
#define DIO0 26

// ---------- OLED ----------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

static void oled3(const String &a, const String &b = "", const String &c = "")
{
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(a);
  if (b.length())
    display.println(b);
  if (c.length())
    display.println(c);
  display.display();
}
static void serialPrintLnChunked(const String &s, size_t ch = 128)
{
  for (size_t i = 0; i < s.length(); i += ch)
  {
    size_t n = min(ch, s.length() - i);
    Serial.write((const uint8_t *)s.c_str() + i, n);
  }
  Serial.write('\n');
}

// ---------- Distance estimation (RSSI-based) ----------
const float RSSI_REF_1M = -45.0f; // ADJUST THIS: avg RSSI at 1 meter
const float PATH_LOSS_N = 2.7f;   // 2.7 = Urban, 3.0 = Indoor
float rssiEma = NAN;
const float RSSI_ALPHA = 0.20f;

float estimateDistanceMetersFromRSSI(int rssi)
{
  float r = (float)rssi;
  if (isnan(rssiEma))
    rssiEma = r;
  rssiEma = RSSI_ALPHA * r + (1.0f - RSSI_ALPHA) * rssiEma;
  float exponent = (RSSI_REF_1M - rssiEma) / (10.0f * PATH_LOSS_N);
  return powf(10.0f, exponent);
}

// ---------- IDs & counters ----------
String myId = "????????????", dstAny = "FF";
uint64_t txDataPktsTotal = 0, rxDataPktsTotal = 0;
uint64_t txBytesTotal = 0, rxBytesTotal = 0;

// ---------- Timing / ARQ knobs ----------
size_t FRAG_CHUNK = 230;
const int FRAG_MAX_TRIES = 3;                   // FIXED: Try 3 times max
const unsigned long FRAG_ACK_TIMEOUT_MS = 1200; // Increased slightly for safety
const unsigned long FRAG_SPACING_MS = 20;

// NOTE: Receiver will wait this long before sending ACK to ensure Sender is listening
const int RX_ACK_DELAY_MS = 20;

uint32_t txSeq = 0;
unsigned long sessionStartMs = 0;

// ---------- Helpers ----------
static String bytesToHuman(uint64_t B)
{
  if (B >= 1000000ULL)
    return String((double)B / 1e6, 3) + " MB";
  if (B >= 1000ULL)
    return String((double)B / 1e3, 3) + " kB";
  return String((uint64_t)B) + " B";
}
static String bitsToHuman(uint64_t b)
{
  if (b >= 1000000ULL)
    return String((double)b / 1e6, 3) + " Mb";
  if (b >= 1000ULL)
    return String((double)b / 1e3, 3) + " kb";
  return String((uint64_t)b) + " b";
}
static String speedToHuman(double bps)
{
  if (bps >= 1e6)
    return String(bps / 1e6, 3) + " Mb/s";
  if (bps >= 1e3)
    return String(bps / 1e3, 3) + " kb/s";
  return String(bps, 0) + " b/s";
}
static void macTo12Hex(String &out)
{
  uint64_t mac = ESP.getEfuseMac();
  char buf[13];
  sprintf(buf, "%04X%08X", (uint16_t)(mac >> 32), (uint32_t)mac);
  out = String(buf);
}
static void sanitizeText(String &s)
{
  for (uint16_t i = 0; i < s.length(); ++i)
  {
    char c = s[i];
    if (c == ',' || c == '\r' || c == '\n')
      s.setCharAt(i, ' ');
  }
}
static void sendLoRa(const String &payload)
{
  LoRa.beginPacket();
  LoRa.print(payload);
  LoRa.endPacket();
}
static long toLong(const String &s)
{
  long v = 0;
  bool seen = false;
  for (uint16_t i = 0; i < s.length(); ++i)
  {
    char c = s[i];
    if (c >= '0' && c <= '9')
    {
      v = v * 10 + (c - '0');
      seen = true;
    }
    else if (seen)
      break;
  }
  return seen ? v : -1;
}
static int digitsU(unsigned long v)
{
  int d = 1;
  while (v >= 10)
  {
    v /= 10;
    d++;
  }
  return d;
}

// ---------- Parsers ----------
static bool parseMSG(const String &in, String &src, String &dst, long &seq, String &text)
{
  if (!in.startsWith("MSG,"))
    return false;
  int p1 = in.indexOf(',', 4);
  if (p1 < 0)
    return false;
  int p2 = in.indexOf(',', p1 + 1);
  if (p2 < 0)
    return false;
  int p3 = in.indexOf(',', p2 + 1);
  if (p3 < 0)
    return false;
  src = in.substring(4, p1);
  dst = in.substring(p1 + 1, p2);
  String seqStr = in.substring(p2 + 1, p3);
  text = in.substring(p3 + 1);
  seq = toLong(seqStr);
  return true;
}
static bool parseMSGF(const String &in, String &src, String &dst, long &seq, long &idx, long &tot, String &chunk)
{
  if (!in.startsWith("MSGF,"))
    return false;
  int p1 = in.indexOf(',', 5);
  if (p1 < 0)
    return false;
  int p2 = in.indexOf(',', p1 + 1);
  if (p2 < 0)
    return false;
  int p3 = in.indexOf(',', p2 + 1);
  if (p3 < 0)
    return false;
  int p4 = in.indexOf(',', p3 + 1);
  if (p4 < 0)
    return false;
  int p5 = in.indexOf(',', p4 + 1);
  if (p5 < 0)
    return false;
  src = in.substring(5, p1);
  dst = in.substring(p1 + 1, p2);
  String seqStr = in.substring(p2 + 1, p3);
  String idxStr = in.substring(p3 + 1, p4);
  String totStr = in.substring(p4 + 1, p5);
  chunk = in.substring(p5 + 1);
  seq = toLong(seqStr);
  idx = toLong(idxStr);
  tot = toLong(totStr);
  return true;
}
static bool parseACKF(const String &in, String &src, String &dst, long &seq, long &idx)
{
  if (!in.startsWith("ACKF,"))
    return false;
  int p1 = in.indexOf(',', 5);
  if (p1 < 0)
    return false;
  int p2 = in.indexOf(',', p1 + 1);
  if (p2 < 0)
    return false;
  int p3 = in.indexOf(',', p2 + 1);
  if (p3 < 0)
    return false;
  src = in.substring(5, p1);
  dst = in.substring(p1 + 1, p2);
  String seqStr = in.substring(p2 + 1, p3);
  String idxStr = in.substring(p3 + 1);
  seq = toLong(seqStr);
  idx = toLong(idxStr);
  return true;
}
static bool parseACK(const String &in, String &src, String &dst, long &seq, uint64_t &rxTotBytes, uint64_t &rxTotPkts)
{
  if (!in.startsWith("ACK,"))
    return false;
  int p1 = in.indexOf(',', 4);
  if (p1 < 0)
    return false;
  int p2 = in.indexOf(',', p1 + 1);
  if (p2 < 0)
    return false;
  int p3 = in.indexOf(',', p2 + 1);
  if (p3 < 0)
    return false;
  int p4 = in.indexOf(',', p3 + 1);
  if (p4 < 0)
    return false;
  src = in.substring(4, p1);
  dst = in.substring(p1 + 1, p2);
  String seqStr = in.substring(p2 + 1, p3);
  String bStr = in.substring(p3 + 1, p4);
  String kStr = in.substring(p4 + 1);
  seq = toLong(seqStr);
  rxTotBytes = (uint64_t)bStr.toDouble();
  rxTotPkts = (uint64_t)kStr.toDouble();
  return true;
}

// ---------- RX reassembly ----------
String reSrc = "";
long reSeq = -1;
long reTot = 0;
long reGot = 0;
String *reChunks = nullptr;
bool *reHave = nullptr;
void resetReasm()
{
  if (reChunks)
  {
    delete[] reChunks;
    reChunks = nullptr;
  }
  if (reHave)
  {
    delete[] reHave;
    reHave = nullptr;
  }
  reSrc = "";
  reSeq = -1;
  reTot = 0;
  reGot = 0;
}
void startReasm(const String &src, long seq, long tot)
{
  resetReasm();
  reSrc = src;
  reSeq = seq;
  reTot = tot;
  reGot = 0;
  reChunks = new String[tot];
  reHave = new bool[tot];
  for (long i = 0; i < tot; i++)
    reHave[i] = false;
}
bool addFrag(long idx, const String &chunk)
{
  if (idx < 0 || idx >= reTot)
    return false;
  if (!reHave[idx])
  {
    reHave[idx] = true;
    reChunks[idx] = chunk;
    reGot++;
    return true;
  }
  return false;
}
String joinReasm()
{
  String out;
  out.reserve(reTot * FRAG_CHUNK);
  for (long i = 0; i < reTot; i++)
    out += reChunks[i];
  return out;
}

// ---------- Blocking waits ----------
// NOTE: Added RX_ACK_DELAY_MS inside receiver logic, so sender logic stays same but works better
bool waitForAckF(long expectSeq, long expectIdx, unsigned long timeoutMs)
{
  unsigned long deadline = millis() + timeoutMs;
  while ((long)(millis() - deadline) < 0)
  {
    int psz = LoRa.parsePacket();
    if (psz)
    {
      String pkt;
      while (LoRa.available())
        pkt += (char)LoRa.read();
      String s, d, t;
      long seq = -1, idx = -1, tot = -1;
      uint64_t b = 0, k = 0;

      if (parseACKF(pkt, s, d, seq, idx))
      {
        if (d == myId && seq == expectSeq && idx == expectIdx)
          return true;
      }
      else if (parseACK(pkt, s, d, seq, b, k))
      { /* ignore */
      }
      else if (parseMSG(pkt, s, d, seq, t))
      {
        // Handle incoming MSG while waiting
        delay(RX_ACK_DELAY_MS); // Give sender time to switch to RX
        String ack = "ACK," + myId + "," + s + "," + String(seq) + "," + String((unsigned long long)(rxBytesTotal + t.length())) + "," + String((unsigned long long)(rxDataPktsTotal + 1));
        sendLoRa(ack);
        // Print it
        Serial.println("[RX Interrupted] MSG from " + s);
      }
      // ignoring fragments while waiting for ACKF to keep simple
    }
    delay(1);
  }
  return false;
}

bool waitForFinalAck(long expectSeq, unsigned long timeoutMs, double &pdrOut, double &bpsOut)
{
  unsigned long deadline = millis() + timeoutMs;
  pdrOut = 0;
  bpsOut = 0;
  while ((long)(millis() - deadline) < 0)
  {
    int psz = LoRa.parsePacket();
    if (psz)
    {
      String pkt;
      while (LoRa.available())
        pkt += (char)LoRa.read();
      String s, d, t;
      long seq = -1, idx = -1, tot = -1;
      uint64_t peerBytes = 0, peerPkts = 0;

      if (parseACK(pkt, s, d, seq, peerBytes, peerPkts))
      {
        if (d == myId && seq == expectSeq)
        {
          unsigned long elapsed = millis() - sessionStartMs;
          bpsOut = elapsed ? (peerBytes * 8.0 * 1000.0 / elapsed) : 0.0;
          pdrOut = (txDataPktsTotal > 0) ? (100.0 * (double)peerPkts / (double)txDataPktsTotal) : 0.0;
          int rssi = LoRa.packetRssi();
          float d_m = estimateDistanceMetersFromRSSI(rssi);

          Serial.printf("[ACK OK] #%ld from %s | PDR=%.1f%% | %s | d~%.1fm\n",
                        seq, s.c_str(), pdrOut, speedToHuman(bpsOut).c_str(), d_m);
          oled3("ACK OK (" + String(seq) + ")", "PDR " + String(pdrOut, 1) + "% " + bytesToHuman(peerBytes), "d~" + String(d_m, 1) + "m");
          return true;
        }
      }
      // Handle other packets if necessary, but keep simple for stability
    }
    delay(1);
  }
  return false;
}

// ---------- Send one message reliably ----------
// FIX: No whole-message loop. If frag fails 3 times -> ABORT.
bool sendMessageReliable(const String &lineIn)
{
  String line = lineIn;
  sanitizeText(line);
  const size_t L = line.length();
  const bool single = (L <= FRAG_CHUNK);
  const size_t total = single ? 1 : (L + FRAG_CHUNK - 1) / FRAG_CHUNK;

  uint32_t seq = txSeq;
  txSeq++;
  Serial.printf("[TX START] #%lu len=%u chunks=%u\n", (unsigned long)seq, L, total);

  if (single)
  {
    // ... same single logic ...
    size_t hdrLen = 4 + myId.length() + 1 + dstAny.length() + 1 + digitsU(seq) + 1;
    size_t maxText = (hdrLen < LORA_MAX_PAYLOAD) ? (LORA_MAX_PAYLOAD - hdrLen) : 0;
    String text = (L <= maxText) ? line : line.substring(0, maxText);
    String payload = "MSG," + myId + "," + dstAny + "," + String(seq) + "," + text;

    // Single Attempt Loop
    bool sentOk = false;
    for (int i = 0; i < FRAG_MAX_TRIES; i++)
    {
      sendLoRa(payload);
      txDataPktsTotal++;
      txBytesTotal += text.length();
      Serial.printf("  [TX SINGLE] Try %d/%d\n", i + 1, FRAG_MAX_TRIES);

      double pdr = 0, bps = 0;
      if (waitForFinalAck(seq, FRAG_ACK_TIMEOUT_MS, pdr, bps))
      {
        sentOk = true;
        break;
      }
      delay(FRAG_SPACING_MS);
    }

    if (!sentOk)
    {
      Serial.println("  -> FAILED: No ACK for single message.");
      oled3("TX FAILED", "No ACK", "");
      return false;
    }
  }
  else
  {
    // Fragment Logic
    for (size_t i = 0; i < total; i++)
    {
      size_t off = i * FRAG_CHUNK;
      String chunk = line.substring(off, min(L, off + FRAG_CHUNK));

      // Dynamic resize logic
      size_t hdrLen = 5 + myId.length() + 1 + dstAny.length() + 1 + digitsU(seq) + 1 + digitsU(i) + 1 + digitsU(total) + 1;
      size_t maxChunk = (hdrLen < LORA_MAX_PAYLOAD) ? (LORA_MAX_PAYLOAD - hdrLen) : 0;
      if (chunk.length() > maxChunk)
        chunk.remove(maxChunk);

      String payload = "MSGF," + myId + "," + dstAny + "," + String(seq) + "," + String(i) + "," + String(total) + "," + chunk;

      bool fragAcked = false;
      // Retry Loop for THIS fragment only
      for (int ftry = 1; ftry <= FRAG_MAX_TRIES; ++ftry)
      {
        sendLoRa(payload);
        txDataPktsTotal++;
        txBytesTotal += chunk.length();

        Serial.printf("  [TX FRAG %u/%u] Try %d/%d\n", (unsigned)(i + 1), (unsigned)total, ftry, FRAG_MAX_TRIES);

        if (waitForAckF((long)seq, (long)i, FRAG_ACK_TIMEOUT_MS))
        {
          fragAcked = true;
          break; // Success, move to next frag
        }
        delay(FRAG_SPACING_MS);
      }

      if (!fragAcked)
      {
        // FIX: ABORT IMMEDIATELY
        Serial.println("[ABORT] Frag failed after max retries. Dropping message.");
        oled3("TX FAILED", "Dropped Frag " + String(i + 1), "");
        return false;
      }
    }

    // All frags sent, wait for final stats
    double pdr = 0, bps = 0;
    // Wait a bit longer for the final full ACK
    waitForFinalAck(seq, 2000, pdr, bps);
  }

  return true;
}

// ---------- Setup / Loop ----------
void setup()
{
  Serial.begin(115200);
  Serial.setTimeout(10);
  Wire.begin();
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
  {
    Serial.println("SSD1306 fail");
    for (;;)
      ;
  }

  macTo12Hex(myId);
  SPI.begin(SCK, MISO, MOSI, SS);
  LoRa.setPins(SS, RST, DIO0);
  if (!LoRa.begin(FREQ_HZ))
  {
    oled3("LoRa init FAILED", "Check wiring/freq");
    for (;;)
      ;
  }

  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSyncWord(LORA_SYNC);
  LoRa.enableCrc();
  LoRa.setTxPower(17, PA_OUTPUT_PA_BOOST_PIN);
  LoRa.setSignalBandwidth(125E3);

  sessionStartMs = millis();
  resetReasm();

  oled3("LoRa Chat Ready", "ID: " + myId, "SF" + String(LORA_SF) + " (Wait Fix)");
  Serial.println("=== LoRa Chat FIX (Delay ACK + Abort on Fail) ===");
  Serial.println("Type and Enter to send.");
}

void loop()
{
  // 1) Send
  if (Serial.available())
  {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length())
      sendMessageReliable(line);
  }

  // 2) Receive
  int psz = LoRa.parsePacket();
  if (psz)
  {
    String pkt;
    while (LoRa.available())
      pkt += (char)LoRa.read();
    String s, d, t;
    long seq = -1, idx = -1, tot = -1;
    uint64_t b = 0, k = 0;

    // ACKs are handled here if they arrive unexpectedly, but mostly ignored
    if (parseACK(pkt, s, d, seq, b, k) || parseACKF(pkt, s, d, seq, idx))
      return;

    if (parseMSG(pkt, s, d, seq, t))
    {
      int rssi = LoRa.packetRssi();
      float d_m = estimateDistanceMetersFromRSSI(rssi);
      rxDataPktsTotal++;
      rxBytesTotal += t.length();

      Serial.printf("[RX MSG] #%ld | d~%.1fm\n", seq, d_m);
      serialPrintLnChunked(t);
      oled3("RX <- (" + String(seq) + ")", t.substring(0, 16), "d~" + String(d_m, 1) + "m");

      // CRITICAL FIX: Delay before sending ACK so sender is ready
      delay(RX_ACK_DELAY_MS);

      String ack = "ACK," + myId + "," + s + "," + String(seq) + "," + String((unsigned long long)rxBytesTotal) + "," + String((unsigned long long)rxDataPktsTotal);
      sendLoRa(ack);
      return;
    }

    if (parseMSGF(pkt, s, d, seq, idx, tot, t))
    {
      int rssi = LoRa.packetRssi();
      float d_m = estimateDistanceMetersFromRSSI(rssi);

      if (s != reSrc || seq != reSeq)
        startReasm(s, seq, tot);
      bool fresh = addFrag(idx, t);
      if (fresh)
      {
        rxDataPktsTotal++;
        rxBytesTotal += t.length();
      }

      // CRITICAL FIX: Delay before sending ACKF
      delay(RX_ACK_DELAY_MS);

      String ackf = "ACKF," + myId + "," + s + "," + String(seq) + "," + String(idx);
      sendLoRa(ackf);

      bool all = true;
      for (long i = 0; i < reTot; i++)
        if (!reHave[i])
        {
          all = false;
          break;
        }
      if (all)
      {
        String full = joinReasm();
        Serial.printf("[RX FULL] #%ld | Len %u | d~%.1fm\n", seq, (unsigned)full.length(), d_m);
        serialPrintLnChunked(full);
        oled3("RX FULL (" + String(seq) + ")", "Len " + String(full.length()), "d~" + String(d_m, 1) + "m");

        delay(RX_ACK_DELAY_MS); // One more safety delay for final ACK
        String ack = "ACK," + myId + "," + s + "," + String(seq) + "," + String((unsigned long long)rxBytesTotal) + "," + String((unsigned long long)rxDataPktsTotal);
        sendLoRa(ack);
        resetReasm();
      }
      return;
    }
  }
}
/*
  LoRa Serial Chat (Go-Back-N ARQ) + PDR + Goodput + TIMING CSV + LittleFS
  ESP32 T-Display + SSD1306 + SX127x (Sandeep Mistry LoRa lib)
  === AS923 variant (923 MHz) ===

  CSV line format (one per event):
  TIM,<nodeId>,<role>,<event>,<seq>,<idx>,<tot>,<bytes>,<rssi>,<snr>,<toa_ms>,<t_ms>,<dt_ms>

  role: TX | RX
  event: SESSION_START | MSG_TX | MSG_RX | MSGF_TX | MSGF_RX | ACK_TX | ACK_RX | ACKF_TX | ACKF_RX
         | WAIT_ACKF_START | WAIT_ACKF_OK | WAIT_ACKF_TO | TIMEOUT_WINDOW
         | WAIT_ACK_START  | WAIT_ACK_OK  | WAIT_ACK_TO
         | RETRY_MSG | RETRY_FRAG | ABORT

  Go-Back-N ARQ:
  - Transmitter sends multiple fragments within a sliding window (default size 4)
  - Receiver sends cumulative ACKF for highest consecutive fragment received
  - On ACK, transmitter slides window forward and sends next batch
  - On timeout, transmitter goes back N and retransmits from unACKed fragment
  - More efficient than Stop-and-Wait but still simple to implement

  Notes:
  - rssi/snr are only meaningful on RX; TX lines print rssi=-, snr=-
  - toa_ms is estimated airtime for the just-sent/just-received payload
  - t_ms is wall time since boot (millis), dt_ms is delta since previous event on this node
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SPI.h>
#include <LoRa.h>
#include <LittleFS.h>
#include <FS.h>
#include <WiFi.h>
#include <time.h>
#include <sys/time.h>

// ---------- WiFi & NTP config ----------
#define WIFI_SSID "Thaveesha"
#define WIFI_PASSWORD "101010101"
#define NTP_SERVER "pool.ntp.org"
#define GMT_OFFSET_SEC 0           // UTC timezone (adjust as needed)
#define DAYLIGHT_OFFSET_SEC 0      // Daylight saving (0 if not applicable)

// ---------- Radio config (AS923) ----------
#define FREQ_HZ 923E6
#define LORA_SYNC 0xA5
#define LORA_SF 8
#define LORA_BW_HZ 125000
#define LORA_CR_DEN 5 // 4/5 => 5
#define LORA_HAS_CRC 1

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

// ---------- WiFi & NTP state (declare before functions) ----------
bool wifiConnected = false;
bool ntpSynced = false;
unsigned long lastNtpSyncMs = 0;
const unsigned long NTP_SYNC_INTERVAL_MS = 3600000; // Sync every hour

// ---------- Forward declarations ----------
static void oled3(const String &a, const String &b = "", const String &c = "");
static void initWiFi();
static void syncNTP();
static void checkNtpResync();
static String getFormattedTime();

// ---------- WiFi & NTP functions ----------
static void initWiFi()
{
  oled3("Connecting WiFi...", WIFI_SSID, "");
  Serial.println("[WiFi] Connecting to " + String(WIFI_SSID));
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  unsigned long wifiStartMs = millis();
  int attempts = 0;
  
  while (WiFi.status() != WL_CONNECTED && (millis() - wifiStartMs) < 15000) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println();
    Serial.println("[WiFi] Connected!");
    Serial.println("[WiFi] IP: " + WiFi.localIP().toString());
    Serial.println("[WiFi] RSSI: " + String(WiFi.RSSI()) + " dBm");
    oled3("WiFi Connected", "IP: " + WiFi.localIP().toString().substring(0, 16), "RSSI: " + String(WiFi.RSSI()));
    delay(2000);
  } else {
    wifiConnected = false;
    Serial.println();
    Serial.println("[WiFi] Failed to connect");
    oled3("WiFi Failed", "Continuing offline", "");
    delay(2000);
  }
}

static void syncNTP()
{
  if (!wifiConnected) {
    Serial.println("[NTP] WiFi not connected, skipping NTP sync");
    return;
  }
  
  oled3("Syncing NTP...", NTP_SERVER, "");
  Serial.println("[NTP] Syncing with " + String(NTP_SERVER));
  
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  
  // Wait for time to be set
  unsigned long ntpStartMs = millis();
  time_t now = time(nullptr);
  
  while (now < 24 * 3600 && (millis() - ntpStartMs) < 15000) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  
  if (now > 24 * 3600) {
    ntpSynced = true;
    lastNtpSyncMs = millis();
    Serial.println();
    Serial.println("[NTP] Synced!");
    Serial.println("[NTP] Time: " + String(ctime(&now)));
    oled3("NTP Synced", String(ctime(&now)).substring(0, 20), "");
    delay(2000);
  } else {
    ntpSynced = false;
    Serial.println();
    Serial.println("[NTP] Sync failed, using local time");
    oled3("NTP Failed", "Using local time", "");
    delay(2000);
  }
}

static void checkNtpResync()
{
  if (wifiConnected && ntpSynced) {
    if ((millis() - lastNtpSyncMs) > NTP_SYNC_INTERVAL_MS) {
      Serial.println("[NTP] Periodic resync triggered");
      syncNTP();
    }
  }
}

static String getFormattedTime()
{
  time_t now = time(nullptr);
  struct tm *timeinfo = localtime(&now);
  char buffer[32];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeinfo);
  return String(buffer);
}

static void oled3(const String &a, const String &b, const String &c)
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

// ---------- IDs & counters ----------
String myId = "????????????", dstAny = "FF";       // 12-hex MAC, broadcast dst
uint64_t txDataPktsTotal = 0, rxDataPktsTotal = 0; // counts MSG + unique MSGF frags
uint64_t txBytesTotal = 0, rxBytesTotal = 0;       // app TEXT bytes totals

// ---------- Timing / ARQ knobs (Go-Back-N) ----------
const size_t FRAG_CHUNK = 200;                    // text bytes per fragment
const int GBN_WINDOW_SIZE = 4;                    // go-back-n window size
const unsigned long GBN_ACK_TIMEOUT_MS = 2000;   // wait for ACK on window
const unsigned long GBN_FRAG_SPACING_MS = 20;    // spacing between fragment sends
const unsigned long BASE_FINAL_ACK_TIMEOUT_MS = 1800; // final ACK wait baseline
const int MSG_MAX_TRIES = 3;                          // whole-message attempts

uint32_t txSeq = 0;
unsigned long sessionStartMs = 0;
unsigned long lastEventMs = 0; // for dt_ms

// ---------- File system paths ----------
String txCsvPath = "/tx_data.csv";
String rxCsvPath = "/rx_data.csv";
String timingCsvPath = "/timing_data.csv";
File txCsvFile, rxCsvFile, timingCsvFile;

// ---------- Serial CSV logging only ----------
// ---------- LittleFS CSV logging ----------
static void initCsvLogging()
{
  // Initialize LittleFS
  if (!LittleFS.begin(true))
  {
    Serial.println("[ERROR] Failed to mount LittleFS");
    return;
  }
  
  Serial.println("[LOG] LittleFS mounted successfully");
  
  // Create/open CSV files
  txCsvFile = LittleFS.open(txCsvPath, "w");
  rxCsvFile = LittleFS.open(rxCsvPath, "w");
  timingCsvFile = LittleFS.open(timingCsvPath, "w");
  
  if (txCsvFile && rxCsvFile && timingCsvFile)
  {
    // Write CSV headers
    txCsvFile.println("time_ms,packet_type,sequence_no,fragment_idx,total_fragments,packet_size_bytes");
    rxCsvFile.println("time_ms,packet_type,sequence_no,fragment_idx,total_fragments,packet_size_bytes");
    timingCsvFile.println("nodeId,role,event,seq,idx,tot,bytes,rssi,snr,toa_ms,t_ms,dt_ms");
    
    // Flush to ensure data is written
    txCsvFile.flush();
    rxCsvFile.flush();
    timingCsvFile.flush();
    
    Serial.println("[LOG] CSV files created on LittleFS:");
    Serial.println("  - /tx_data.csv");
    Serial.println("  - /rx_data.csv");
    Serial.println("  - /timing_data.csv");
  }
  else
  {
    Serial.println("[ERROR] Failed to create CSV files");
  }
}

static void writeCsvLine(const String &line)
{
  // Write timing data to file and serial
  if (timingCsvFile)
  {
    timingCsvFile.println(line);
    timingCsvFile.flush();
  }
  Serial.println(line);
}

static void writeTxCsv(unsigned long timeMs, const String &packetType, long seqNo, long fragIdx, long totalFrags, size_t packetSize)
{
  // Write to LittleFS file
  if (txCsvFile)
  {
    txCsvFile.print(timeMs);
    txCsvFile.print(",");
    txCsvFile.print(packetType);
    txCsvFile.print(",");
    txCsvFile.print(seqNo);
    txCsvFile.print(",");
    txCsvFile.print(fragIdx);
    txCsvFile.print(",");
    txCsvFile.print(totalFrags);
    txCsvFile.print(",");
    txCsvFile.println(packetSize);
    txCsvFile.flush();
  }
  
  // Also output to serial for monitoring
  Serial.print("TX_CSV:");
  Serial.print(timeMs);
  Serial.print(",");
  Serial.print(packetType);
  Serial.print(",");
  Serial.print(seqNo);
  Serial.print(",");
  Serial.print(fragIdx);
  Serial.print(",");
  Serial.print(totalFrags);
  Serial.print(",");
  Serial.println(packetSize);
}

static void writeRxCsv(unsigned long timeMs, const String &packetType, long seqNo, long fragIdx, long totalFrags, size_t packetSize)
{
  // Write to LittleFS file
  if (rxCsvFile)
  {
    rxCsvFile.print(timeMs);
    rxCsvFile.print(",");
    rxCsvFile.print(packetType);
    rxCsvFile.print(",");
    rxCsvFile.print(seqNo);
    rxCsvFile.print(",");
    rxCsvFile.print(fragIdx);
    rxCsvFile.print(",");
    rxCsvFile.print(totalFrags);
    rxCsvFile.print(",");
    rxCsvFile.println(packetSize);
    rxCsvFile.flush();
  }
  
  // Also output to serial for monitoring
  Serial.print("RX_CSV:");
  Serial.print(timeMs);
  Serial.print(",");
  Serial.print(packetType);
  Serial.print(",");
  Serial.print(seqNo);
  Serial.print(",");
  Serial.print(fragIdx);
  Serial.print(",");
  Serial.print(totalFrags);
  Serial.print(",");
  Serial.println(packetSize);
}

// File management and retrieval functions
static void showCsvInfo()
{
  Serial.println("[LOG] CSV data is stored on LittleFS:");
  
  // Show file sizes
  if (LittleFS.exists(txCsvPath))
  {
    File f = LittleFS.open(txCsvPath, "r");
    Serial.println("  TX CSV: " + txCsvPath + " (" + String(f.size()) + " bytes)");
    f.close();
  }
  
  if (LittleFS.exists(rxCsvPath))
  {
    File f = LittleFS.open(rxCsvPath, "r");
    Serial.println("  RX CSV: " + rxCsvPath + " (" + String(f.size()) + " bytes)");
    f.close();
  }
  
  if (LittleFS.exists(timingCsvPath))
  {
    File f = LittleFS.open(timingCsvPath, "r");
    Serial.println("  Timing CSV: " + timingCsvPath + " (" + String(f.size()) + " bytes)");
    f.close();
  }
  
  Serial.println("[CMD] Use 'download tx', 'download rx', 'download timing' to get files");
  Serial.println("[CMD] Use 'clear' to delete all CSV files");
}

static void downloadCsvFile(const String &filename, const String &filepath)
{
  if (!LittleFS.exists(filepath))
  {
    Serial.println("[ERROR] File not found: " + filepath);
    return;
  }
  
  File file = LittleFS.open(filepath, "r");
  if (!file)
  {
    Serial.println("[ERROR] Cannot open file: " + filepath);
    return;
  }
  
  String upperFilename = filename;
  upperFilename.toUpperCase();
  Serial.println("=== BEGIN " + upperFilename + " CSV FILE ===");
  while (file.available())
  {
    Serial.write(file.read());
  }
  Serial.println("\n=== END " + upperFilename + " CSV FILE ===");
  
  file.close();
}

static void clearCsvFiles()
{
  // Close files first
  if (txCsvFile) txCsvFile.close();
  if (rxCsvFile) rxCsvFile.close();
  if (timingCsvFile) timingCsvFile.close();
  
  // Delete files
  LittleFS.remove(txCsvPath);
  LittleFS.remove(rxCsvPath);
  LittleFS.remove(timingCsvPath);
  
  Serial.println("[LOG] All CSV files cleared from LittleFS");
  
  // Recreate files
  initCsvLogging();
}

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

// ---------- LoRa ToA estimator (ms) ----------
static double loraToaMs(size_t payloadLen)
{
  const double SF = LORA_SF;
  const double BW = (double)LORA_BW_HZ;
  const int CRden = LORA_CR_DEN;
  const int CRC = LORA_HAS_CRC ? 1 : 0;
  const int IH = 0; // explicit header
  const int DE = ((SF >= 11) && (BW == 125000)) ? 1 : 0;

  const double Ts = (double)((1 << (int)SF)) / BW; // s
  const double Tpre = (8.0 + 4.25) * Ts;

  int cr = (CRden - 4); // 1..4
  double payloadSymbNb = 8.0 + max(
                                   (int)ceil(
                                       ((double)(8 * payloadLen) - 4.0 * SF + 28.0 + 16.0 * CRC - 20.0 * IH) / (4.0 * (SF - 2.0 * DE))) *
                                       (cr + 4),
                                   0);

  double Tpl = payloadSymbNb * Ts;
  return (Tpre + Tpl) * 1000.0;
}

// ---------- CSV Timing logger ----------
static void logEvt(const char *role, const char *event,
                   long seq, long idx, long tot, long bytes,
                   const String &rssiStr, const String &snrStr,
                   double toa_ms)
{
  unsigned long now = millis();
  unsigned long dt = (lastEventMs == 0) ? 0 : (now - lastEventMs);
  lastEventMs = now;

  String line;
  line.reserve(128);
  line += "TIM,";
  line += myId;
  line += ",";
  line += role;
  line += ",";
  line += event;
  line += ",";
  line += String(seq);
  line += ",";
  line += String(idx);
  line += ",";
  line += String(tot);
  line += ",";
  line += String(bytes);
  line += ",";
  line += rssiStr;
  line += ",";
  line += snrStr;
  line += ",";
  line += String((long)round(toa_ms));
  line += ",";
  line += String(now);
  line += ",";
  line += String(dt);

  Serial.println(line);
  writeCsvLine(line);
}

// Helper function to extract packet type from event
static String getPacketTypeFromEvent(const char *event)
{
  String eventStr = String(event);
  if (eventStr.startsWith("MSG_")) return "MSG";
  else if (eventStr.startsWith("MSGF_")) return "MSGF";
  else if (eventStr.startsWith("ACK_")) return "ACK";
  else if (eventStr.startsWith("ACKF_")) return "ACKF";
  else return "OTHER";
}

static void logEvtTx(const char *event, long seq, long idx, long tot, long bytes, size_t payloadLen)
{
  double toa = loraToaMs(payloadLen);
  logEvt("TX", event, seq, idx, tot, bytes, "-", "-", toa);
  
  // Write to simplified Tx.csv with packet details
  String packetType = getPacketTypeFromEvent(event);
  writeTxCsv(millis(), packetType, seq, idx, tot, payloadLen);
}
static void logEvtRx(const char *event, long seq, long idx, long tot, long bytes, size_t payloadLen, int rssi, float snr)
{
  double toa = loraToaMs(payloadLen);
  logEvt("RX", event, seq, idx, tot, bytes, String(rssi), String(snr, 1), toa);
  
  // Write to simplified Rx.csv with packet details
  String packetType = getPacketTypeFromEvent(event);
  writeRxCsv(millis(), packetType, seq, idx, tot, payloadLen);
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

// ---------- Blocking waits (Go-Back-N) ----------
// Cumulative ACK tracking for Go-Back-N
struct GBNWindow {
  long seq = -1;
  long tot = 0;
  long base = 0;           // base of window (first unACKed fragment)
  unsigned long sendTime[GBN_WINDOW_SIZE];  // time each fragment was sent
  String fragments[GBN_WINDOW_SIZE];        // fragment payloads for retransmission
  bool sent[GBN_WINDOW_SIZE];               // whether fragment was sent
  
  void reset() {
    seq = -1;
    tot = 0;
    base = 0;
    for (int i = 0; i < GBN_WINDOW_SIZE; i++) {
      sendTime[i] = 0;
      fragments[i] = "";
      sent[i] = false;
    }
  }
  
  int windowSize() {
    return min((long)GBN_WINDOW_SIZE, tot - base);
  }
  
  bool allSent() {
    for (int i = 0; i < windowSize(); i++) {
      if (!sent[i]) return false;
    }
    return true;
  }
  
  bool windowEmpty() {
    return base >= tot;
  }
};

GBNWindow gbnWindow;

bool waitForWindowAck(long expectSeq, unsigned long timeoutMs)
{
  logEvt("TX", "WAIT_ACKF_START", expectSeq, gbnWindow.base, (long)gbnWindow.tot, 0, "-", "-", 0);
  unsigned long deadline = millis() + timeoutMs;
  
  while ((long)(millis() - deadline) < 0)
  {
    // Check timeout and resend window if needed
    unsigned long now = millis();
    for (int i = 0; i < gbnWindow.windowSize(); i++) {
      if (gbnWindow.sent[i] && (now - gbnWindow.sendTime[i]) > GBN_ACK_TIMEOUT_MS) {
        // Timeout on this window - go back N
        logEvt("TX", "TIMEOUT_WINDOW", expectSeq, gbnWindow.base, (long)gbnWindow.tot, 0, "-", "-", 0);
        return false;
      }
    }
    
    int psz = LoRa.parsePacket();
    if (psz)
    {
      String pkt;
      while (LoRa.available())
        pkt += (char)LoRa.read();
      int rssi = LoRa.packetRssi();
      float snr = LoRa.packetSnr();
      String s, d, t;
      long seq = -1, idx = -1, tot = -1;
      uint64_t b = 0, k = 0;

      if (parseACKF(pkt, s, d, seq, idx))
      {
        logEvtRx("ACKF_RX", seq, idx, -1, pkt.length(), pkt.length(), rssi, snr);
        if (d == myId && seq == expectSeq)
        {
          // Cumulative ACK: everything up to idx is ACKed
          if (idx >= gbnWindow.base && idx < gbnWindow.tot) {
            gbnWindow.base = idx + 1;  // slide window
            if (gbnWindow.windowEmpty()) {
              logEvt("TX", "WAIT_ACKF_OK", seq, idx, -1, 0, "-", "-", 0);
              return true;
            }
          }
        }
      }
      else if (parseACK(pkt, s, d, seq, b, k))
      {
        logEvtRx("ACK_RX", seq, -1, -1, pkt.length(), pkt.length(), rssi, snr);
      }
      else if (parseMSG(pkt, s, d, seq, t))
      {
        size_t pktBytes = pkt.length(), textBytes = t.length();
        rxDataPktsTotal++;
        rxBytesTotal += textBytes;
        logEvtRx("MSG_RX", seq, -1, -1, pktBytes, pktBytes, rssi, snr);
        serialPrintLnChunked(t);
        oled3("RX <- (" + String(seq) + ")", t.substring(0, 16), "txt " + String(textBytes) + "B");
        String ack = "ACK," + myId + "," + s + "," + String(seq) + "," + String((unsigned long long)rxBytesTotal) + "," + String((unsigned long long)rxDataPktsTotal);
        sendLoRa(ack);
        logEvtTx("ACK_TX", seq, -1, -1, ack.length(), ack.length());
      }
      else if (parseMSGF(pkt, s, d, seq, idx, tot, t))
      {
        if (s != reSrc || seq != reSeq)
          startReasm(s, seq, tot);
        bool fresh = addFrag(idx, t);
        if (fresh)
        {
          rxDataPktsTotal++;
          rxBytesTotal += t.length();
        }
        logEvtRx("MSGF_RX", seq, idx, tot, pkt.length(), pkt.length(), rssi, snr);

        String ackf = "ACKF," + myId + "," + s + "," + String(seq) + "," + String(idx);
        sendLoRa(ackf);
        logEvtTx("ACKF_TX", seq, idx, -1, ackf.length(), ackf.length());

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
          Serial.printf("[RX FULL] #%ld from %s | total text %uB\n", seq, s.c_str(), (unsigned)full.length());
          serialPrintLnChunked(full);
          oled3("RX <- (" + String(seq) + ")", "full msg", bytesToHuman(full.length()));
          String ack = "ACK," + myId + "," + s + "," + String(seq) + "," + String((unsigned long long)rxBytesTotal) + "," + String((unsigned long long)rxDataPktsTotal);
          sendLoRa(ack);
          logEvtTx("ACK_TX", seq, -1, -1, ack.length(), ack.length());
          resetReasm();
        }
      }
    }
    delay(1);
  }
  logEvt("TX", "WAIT_ACKF_TO", expectSeq, gbnWindow.base, (long)gbnWindow.tot, 0, "-", "-", 0);
  return false; // timeout
}

bool waitForFinalAck(long expectSeq, unsigned long timeoutMs, double &pdrOut, double &bpsOut)
{
  logEvt("TX", "WAIT_ACK_START", expectSeq, -1, -1, 0, "-", "-", 0);
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
      int rssi = LoRa.packetRssi();
      float snr = LoRa.packetSnr();
      String s, d, t;
      long seq = -1, idx = -1, tot = -1;
      uint64_t peerBytes = 0, peerPkts = 0;

      if (parseACK(pkt, s, d, seq, peerBytes, peerPkts))
      {
        logEvtRx("ACK_RX", seq, -1, -1, pkt.length(), pkt.length(), rssi, snr);
        if (d == myId && seq == expectSeq)
        {
          unsigned long elapsed = millis() - sessionStartMs;
          bpsOut = elapsed ? (peerBytes * 8.0 * 1000.0 / elapsed) : 0.0;
          pdrOut = (txDataPktsTotal > 0) ? (100.0 * (double)peerPkts / (double)txDataPktsTotal) : 0.0;
          int rssi2 = LoRa.packetRssi();
          float snr2 = LoRa.packetSnr();
          Serial.printf("[ACK OK] #%ld from %s | peerRxBytes=%llu | peerRxPkts=%llu | PDR=%.1f%% | %s | RSSI %d | SNR %.1f\n",
                        seq, s.c_str(), (unsigned long long)peerBytes, (unsigned long long)peerPkts,
                        pdrOut, speedToHuman(bpsOut).c_str(), rssi2, snr2);
          oled3("ACK OK (" + String(seq) + ")", "PDR " + String(pdrOut, 1) + "%  " + bytesToHuman(peerBytes), speedToHuman(bpsOut));
          logEvt("TX", "WAIT_ACK_OK", seq, -1, -1, 0, "-", "-", 0);
          return true;
        }
      }
      else
      {
        String s2, d2;
        long seq2 = -1, idx2 = -1, tot2 = -1;
        uint64_t b2 = 0, k2 = 0;
        if (parseMSG(pkt, s2, d2, seq2, t))
        {
          size_t pktBytes = pkt.length(), textBytes = t.length();
          rxDataPktsTotal++;
          rxBytesTotal += textBytes;
          logEvtRx("MSG_RX", seq2, -1, -1, pktBytes, pktBytes, rssi, snr);
          serialPrintLnChunked(t);
          oled3("RX <- (" + String(seq2) + ")", t.substring(0, 16), "txt " + String(textBytes) + "B");
          String ack = "ACK," + myId + "," + s2 + "," + String(seq2) + "," + String((unsigned long long)rxBytesTotal) + "," + String((unsigned long long)rxDataPktsTotal);
          sendLoRa(ack);
          logEvtTx("ACK_TX", seq2, -1, -1, ack.length(), ack.length());
        }
        else if (parseMSGF(pkt, s2, d2, seq2, idx2, tot2, t))
        {
          if (s2 != reSrc || seq2 != reSeq)
            startReasm(s2, seq2, tot2);
          bool fresh = addFrag(idx2, t);
          if (fresh)
          {
            rxDataPktsTotal++;
            rxBytesTotal += t.length();
          }
          logEvtRx("MSGF_RX", seq2, idx2, tot2, pkt.length(), pkt.length(), rssi, snr);

          String ackf = "ACKF," + myId + "," + s2 + "," + String(seq2) + "," + String(idx2);
          sendLoRa(ackf);
          logEvtTx("ACKF_TX", seq2, idx2, -1, ackf.length(), ackf.length());
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
            Serial.printf("[RX FULL] #%ld from %s | total text %uB\n", seq2, s2.c_str(), (unsigned)full.length());
            serialPrintLnChunked(full);
            oled3("RX <- (" + String(seq2) + ")", "full msg", bytesToHuman(full.length()));
            String ack = "ACK," + myId + "," + s2 + "," + String(seq2) + "," + String((unsigned long long)rxBytesTotal) + "," + String((unsigned long long)rxDataPktsTotal);
            sendLoRa(ack);
            logEvtTx("ACK_TX", seq2, -1, -1, ack.length(), ack.length());
            resetReasm();
          }
        }
        else if (parseACKF(pkt, s2, d2, seq2, idx2))
        {
          logEvtRx("ACKF_RX", seq2, idx2, -1, pkt.length(), pkt.length(), rssi, snr);
        }
      }
    }
    delay(1);
  }
  logEvt("TX", "WAIT_ACK_TO", expectSeq, -1, -1, 0, "-", "-", 0);
  return false; // timeout
}

// ---------- Send one message reliably (Go-Back-N) ----------
bool sendMessageReliable(const String &lineIn)
{
  String line = lineIn;
  sanitizeText(line);
  const bool single = (line.length() <= FRAG_CHUNK);
  const size_t L = line.length();
  const size_t total = single ? 1 : (L + FRAG_CHUNK - 1) / FRAG_CHUNK;

  uint32_t seq = txSeq;
  txSeq++;

  for (int attempt = 1; attempt <= MSG_MAX_TRIES; ++attempt)
  {
    Serial.printf("[ATTEMPT %d/%d] seq #%lu\n", attempt, MSG_MAX_TRIES, (unsigned long)seq);

    if (single)
    {
      String payload = "MSG," + myId + "," + dstAny + "," + String(seq) + "," + line;
      sendLoRa(payload);
      size_t pktBytes = payload.length(), textBytes = line.length();
      txDataPktsTotal++;
      txBytesTotal += textBytes;
      logEvtTx("MSG_TX", seq, -1, -1, pktBytes, pktBytes);

      oled3("TX -> (" + String(seq) + ")", line.substring(0, 16), "txt " + String(textBytes) + "B");

      double pdr = 0, bps = 0;
      if (waitForFinalAck(seq, BASE_FINAL_ACK_TIMEOUT_MS, pdr, bps))
        return true;

      Serial.println("  -> final ACK timeout, will retry whole message");
      logEvt("TX", "RETRY_MSG", seq, -1, -1, 0, "-", "-", 0);
      delay(100);
    }
    else
    {
      // Go-Back-N fragmented transmission
      gbnWindow.reset();
      gbnWindow.seq = seq;
      gbnWindow.tot = total;
      gbnWindow.base = 0;
      
      // Prepare all fragments
      for (size_t i = 0; i < total; i++)
      {
        size_t off = i * FRAG_CHUNK;
        String chunk = line.substring(off, min(L, off + FRAG_CHUNK));
        String payload = "MSGF," + myId + "," + dstAny + "," + String(seq) + "," + String(i) + "," + String(total) + "," + chunk;
        gbnWindow.fragments[i] = payload;
      }

      bool msgFailed = false;
      unsigned long windowStartTime = millis();

      while (gbnWindow.base < gbnWindow.tot)
      {
        // Send fragments within the window
        for (int i = 0; i < gbnWindow.windowSize(); i++)
        {
          int fragIdx = gbnWindow.base + i;
          if (!gbnWindow.sent[i] && fragIdx < gbnWindow.tot)
          {
            String &payload = gbnWindow.fragments[fragIdx];
            sendLoRa(payload);
            gbnWindow.sent[i] = true;
            gbnWindow.sendTime[i] = millis();
            
            size_t pktBytes = payload.length();
            size_t chunkSize = payload.length() - 50; // approximate chunk size (rough estimate)
            txDataPktsTotal++;
            txBytesTotal += chunkSize;
            logEvtTx("MSGF_TX", seq, (long)fragIdx, (long)total, pktBytes, pktBytes);
            
            delay(GBN_FRAG_SPACING_MS);
          }
        }

        // Wait for ACKs
        if (gbnWindow.allSent())
        {
          if (waitForWindowAck(seq, GBN_ACK_TIMEOUT_MS))
          {
            break;  // All fragments ACKed, window complete
          }
          else
          {
            // Timeout - go back N
            Serial.println("  -> Window timeout, retransmitting from fragment " + String(gbnWindow.base));
            logEvt("TX", "RETRY_FRAG", seq, gbnWindow.base, (long)gbnWindow.tot, 0, "-", "-", 0);
            
            // Reset window sent flags to retransmit
            for (int i = 0; i < gbnWindow.windowSize(); i++) {
              gbnWindow.sent[i] = false;
            }
            
            if ((millis() - windowStartTime) > (GBN_ACK_TIMEOUT_MS * 3))
            {
              msgFailed = true;
              break;
            }
          }
        }
        
        delay(10);
      }

      if (gbnWindow.windowEmpty() && !msgFailed)
      {
        // All fragments ACKed
        double pdr = 0, bps = 0;
        unsigned long finalWait = BASE_FINAL_ACK_TIMEOUT_MS + total * 300;
        if (waitForFinalAck(seq, finalWait, pdr, bps))
        {
          gbnWindow.reset();
          return true;
        }

        Serial.println("  -> final ACK timeout, will retry whole message");
        logEvt("TX", "RETRY_MSG", seq, -1, -1, 0, "-", "-", 0);
        delay(150);
      }
      else if (msgFailed)
      {
        Serial.println("  -> fragments failed after retries, will retry whole message");
        logEvt("TX", "RETRY_MSG", seq, -1, -1, 0, "-", "-", 0);
        delay(150);
      }
    }
  }

  Serial.println("[ABORT] message failed after MSG_MAX_TRIES");
  logEvt("TX", "ABORT", (long)(txSeq - 1), -1, -1, 0, "-", "-", 0);
  oled3("SEND FAILED", "after retries", "");
  gbnWindow.reset();
  return false;
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

#ifdef MY_NODE_ID
  myId = String(MY_NODE_ID);
#else
  macTo12Hex(myId);
#endif

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
  // Optional AS923 channel shaping:
  // LoRa.setSignalBandwidth(125E3);
  // LoRa.setCodingRate4(5);

  sessionStartMs = millis();
  lastEventMs = sessionStartMs;
  resetReasm();

  initCsvLogging(); // <-- start serial CSV output
  
  // Initialize WiFi and NTP
  initWiFi();
  if (wifiConnected) {
    syncNTP();
  }

  // CSV header for live serial viewing
  Serial.println("TIM_HDR,nodeId,role,event,seq,idx,tot,bytes,rssi,snr,toa_ms,t_ms,dt_ms");
  logEvt("TX", "SESSION_START", -1, -1, -1, 0, "-", "-", 0);
  
  // Display status
  String wifiStatus = wifiConnected ? ("WiFi: " + WiFi.localIP().toString()) : "WiFi: Offline";
  String ntpStatus = ntpSynced ? ("NTP: " + getFormattedTime()) : "NTP: Not synced";

  oled3("LoRa Chat Ready", "ID: " + myId, "923 MHz, SF=" + String(LORA_SF));
  Serial.println("=== LoRa Chat (Go-Back-N ARQ) — AS923 (923 MHz) ===");
  Serial.println("[WiFi] " + wifiStatus);
  Serial.println("[NTP] " + ntpStatus);
  Serial.println("115200, Newline. Type and Enter.");
  Serial.println("CSV data stored on ESP32 LittleFS - retrieve after communication");
  Serial.println("Special commands:");
  Serial.println("  'info' - Show CSV file info");
  Serial.println("  'download tx' - Download TX CSV data");
  Serial.println("  'download rx' - Download RX CSV data");
  Serial.println("  'download timing' - Download timing CSV data");
  Serial.println("  'clear' - Clear all CSV files");
  Serial.println("  'wifi' - Show WiFi status");
  Serial.println("  'time' - Show current time (if NTP synced");
  Serial.print("Node ID: ");
  Serial.println(myId);
}

void loop()
{
  // Check for NTP resync every iteration
  checkNtpResync();
  
  // 1) Send when user types (plain text lines become a message)
  if (Serial.available())
  {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length())
    {
      // Handle special commands
      if (line.equals("info"))
      {
        showCsvInfo();
        return;
      }
      else if (line.equals("download tx"))
      {
        downloadCsvFile("tx", txCsvPath);
        return;
      }
      else if (line.equals("download rx"))
      {
        downloadCsvFile("rx", rxCsvPath);
        return;
      }
      else if (line.equals("download timing"))
      {
        downloadCsvFile("timing", timingCsvPath);
        return;
      }
      else if (line.equals("clear"))
      {
        clearCsvFiles();
        return;
      }
      else if (line.equals("wifi"))
      {
        if (wifiConnected) {
          Serial.println("[WiFi] Status: Connected");
          Serial.println("[WiFi] SSID: " + String(WIFI_SSID));
          Serial.println("[WiFi] IP: " + WiFi.localIP().toString());
          Serial.println("[WiFi] RSSI: " + String(WiFi.RSSI()) + " dBm");
        } else {
          Serial.println("[WiFi] Status: Disconnected");
          Serial.println("[WiFi] Attempting to reconnect...");
          initWiFi();
        }
        return;
      }
      else if (line.equals("time"))
      {
        if (ntpSynced) {
          Serial.println("[NTP] Current time: " + getFormattedTime());
          Serial.println("[NTP] Last sync: " + String((millis() - lastNtpSyncMs) / 1000) + " seconds ago");
        } else {
          Serial.println("[NTP] Time not synced");
          if (wifiConnected) {
            Serial.println("[NTP] Attempting NTP sync...");
            syncNTP();
          } else {
            Serial.println("[NTP] WiFi not connected, cannot sync");
          }
        }
        return;
      }
      else
      {
        sendMessageReliable(line);
      }
    }
  }

  // 2) Otherwise receive
  int psz = LoRa.parsePacket();
  if (psz)
  {
    String pkt;
    while (LoRa.available())
      pkt += (char)LoRa.read();
    int rssi = LoRa.packetRssi();
    float snr = LoRa.packetSnr();
    String s, d, t;
    long seq = -1, idx = -1, tot = -1;
    uint64_t b = 0, k = 0;

    if (parseACK(pkt, s, d, seq, b, k))
    {
      logEvtRx("ACK_RX", seq, -1, -1, pkt.length(), pkt.length(), rssi, snr);
      return;
    }
    if (parseACKF(pkt, s, d, seq, idx))
    {
      logEvtRx("ACKF_RX", seq, idx, -1, pkt.length(), pkt.length(), rssi, snr);
      return;
    }
    if (parseMSG(pkt, s, d, seq, t))
    {
      size_t pktBytes = pkt.length(), textBytes = t.length();
      rxDataPktsTotal++;
      rxBytesTotal += textBytes;
      logEvtRx("MSG_RX", seq, -1, -1, pktBytes, pktBytes, rssi, snr);

      serialPrintLnChunked(t);
      oled3("RX <- (" + String(seq) + ")", t.substring(0, 16), "txt " + String(textBytes) + "B");
      String ack = "ACK," + myId + "," + s + "," + String(seq) + "," + String((unsigned long long)rxBytesTotal) + "," + String((unsigned long long)rxDataPktsTotal);
      sendLoRa(ack);
      logEvtTx("ACK_TX", seq, -1, -1, ack.length(), ack.length());
      return;
    }
    if (parseMSGF(pkt, s, d, seq, idx, tot, t))
    {
      if (s != reSrc || seq != reSeq)
        startReasm(s, seq, tot);
      bool fresh = addFrag(idx, t);
      if (fresh)
      {
        rxDataPktsTotal++;
        rxBytesTotal += t.length();
      }
      logEvtRx("MSGF_RX", seq, idx, tot, pkt.length(), pkt.length(), rssi, snr);

      String ackf = "ACKF," + myId + "," + s + "," + String(seq) + "," + String(idx);
      sendLoRa(ackf);
      logEvtTx("ACKF_TX", seq, idx, -1, ackf.length(), ackf.length());

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
        serialPrintLnChunked(full);
        oled3("RX <- (" + String(seq) + ")", "full msg", bytesToHuman(full.length()));
        String ack = "ACK," + myId + "," + s + "," + String(seq) + "," + String((unsigned long long)rxBytesTotal) + "," + String((unsigned long long)rxDataPktsTotal);
        sendLoRa(ack);
        logEvtTx("ACK_TX", seq, -1, -1, ack.length(), ack.length());
        resetReasm();
      }
      return;
    }
  }

  delay(1);
}
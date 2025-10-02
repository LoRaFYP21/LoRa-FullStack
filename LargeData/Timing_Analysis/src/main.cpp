/*
  LoRa Serial Chat (Reliable Fragments + Exact Tries) + PDR + Goodput
  ESP32 T-Display + SSD1306 + SX127x (Sandeep Mistry LoRa lib)
  === AS923 variant (923 static void sanitizeText(String& s){ for(uint16_t i=0;i<s.length();++i){ char c=s[i]; if(c==','||c=='\r'||c=='\n') s.setCharAt(i,' '); } }
static long toLong(const String& s){ long v=0; bool seen=false; for(uint16_t i=0;i<s.length();++i){ char c=s[i];
  if(c>='0'&&c<='9'){ v=v*10+(c-'0'); seen=true; } else if(seen) break; } return seen? v:-1; }

// ---------- Forward declarations ----------=
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SPI.h>
#include <LoRa.h>
#include "FS.h"
#include "SPIFFS.h"

// ---------- Radio config (AS923) ----------
#define FREQ_HZ   923E6      // <- 923 MHz center (make sure your hardware supports it)
#define LORA_SYNC 0xA5
#define LORA_SF   8          // Try 9..12 if your link is weak

// Wiring (LilyGo T-Display -> SX127x)
#define SCK   5
#define MISO  19
#define MOSI  27
#define SS    18
#define RST   14
#define DIO0  26

// Optional: force distinct IDs on each board if desired
// #define MY_NODE_ID "A"    // put "B" on the other board

// ---------- OLED ----------
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

static void oled3(const String& a, const String& b="", const String& c="") {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0); display.println(a); if(b.length()) display.println(b); if(c.length()) display.println(c);
  display.display();
}

static void serialPrintLnChunked(const String& s, size_t ch=128){
  for(size_t i=0;i<s.length();i+=ch){ size_t n=min(ch, s.length()-i); Serial.write((const uint8_t*)s.c_str()+i, n); }
  Serial.write('\n');
}

// ---------- IDs & counters ----------
String myId="????????????", dstAny="FF";           // 12-hex MAC, broadcast dst
uint64_t txDataPktsTotal=0, rxDataPktsTotal=0;     // counts MSG + unique MSGF frags
uint64_t txBytesTotal=0,   rxBytesTotal=0;         // app TEXT bytes totals

// ---------- Timing / ARQ knobs ----------
const size_t  FRAG_CHUNK = 200;                    // text bytes per fragment
const int     FRAG_MAX_TRIES = 8;                  // per-fragment attempts
const unsigned long FRAG_ACK_TIMEOUT_MS = 1000;    // wait for ACKF
const unsigned long FRAG_SPACING_MS     = 15;      // small guard between tries

const unsigned long BASE_FINAL_ACK_TIMEOUT_MS = 1800; // final ACK wait baseline
const int     MSG_MAX_TRIES = 3;                   // whole-message attempts

uint32_t txSeq=0;
unsigned long sessionStartMs=0;

// ---------- CSV Logging for timing analysis ----------
File txCsvFile, rxCsvFile;
bool csvLoggingEnabled = false;
String csvTimestamp = "";

// Helper function to format timestamp for filenames
String getTimestampString() {
  unsigned long ms = millis();
  unsigned long seconds = ms / 1000;
  unsigned long minutes = seconds / 60;
  unsigned long hours = minutes / 60;
  
  seconds = seconds % 60;
  minutes = minutes % 60;
  hours = hours % 24;
  
  char timeStr[20];
  sprintf(timeStr, "%02lu%02lu%02lu_%02lu%02lu%02lu", 
          (unsigned long)(ms/86400000) % 100, // days (mod 100 for 2 digits)
          hours, minutes, seconds, (ms/1000) % 60, (ms/10) % 100);
  return String(timeStr);
}

// Initialize CSV logging files
bool initCSVLogging() {
  if (!SPIFFS.begin()) {
    Serial.println("SPIFFS initialization failed!");
    return false;
  }
  
  csvTimestamp = getTimestampString();
  String txFilename = "/tx_data_" + csvTimestamp + ".csv";
  String rxFilename = "/rx_data_" + csvTimestamp + ".csv";
  
  txCsvFile = SPIFFS.open(txFilename, FILE_WRITE);
  rxCsvFile = SPIFFS.open(rxFilename, FILE_WRITE);
  
  if (!txCsvFile || !rxCsvFile) {
    Serial.println("Failed to create CSV files!");
    return false;
  }
  
  // Write headers
  txCsvFile.println("time_ms,packet_type,sequence_no,fragment_idx,total_fragments,packet_size_bytes");
  rxCsvFile.println("time_ms,packet_type,sequence_no,fragment_idx,total_fragments,packet_size_bytes");
  
  txCsvFile.flush();
  rxCsvFile.flush();
  
  Serial.println("CSV Logging initialized:");
  Serial.println("TX: " + txFilename);
  Serial.println("RX: " + rxFilename);
  
  csvLoggingEnabled = true;
  return true;
}

// Log TX data to CSV
void logTxData(unsigned long timestamp, const String& packetType, long seqNo, long fragIdx, long totalFrags, size_t packetSize) {
  if (!csvLoggingEnabled || !txCsvFile) return;
  
  String line = String(timestamp) + "," + packetType + "," + String(seqNo) + "," + 
                String(fragIdx) + "," + String(totalFrags) + "," + String(packetSize);
  txCsvFile.println(line);
  txCsvFile.flush();
}

// Log RX data to CSV
void logRxData(unsigned long timestamp, const String& packetType, long seqNo, long fragIdx, long totalFrags, size_t packetSize) {
  if (!csvLoggingEnabled || !rxCsvFile) return;
  
  String line = String(timestamp) + "," + packetType + "," + String(seqNo) + "," + 
                String(fragIdx) + "," + String(totalFrags) + "," + String(packetSize);
  rxCsvFile.println(line);
  rxCsvFile.flush();
}

// Format timestamp for display
String formatTimestamp(unsigned long ms) {
  unsigned long seconds = ms / 1000;
  unsigned long minutes = seconds / 60;
  unsigned long hours = minutes / 60;
  
  seconds = seconds % 60;
  minutes = minutes % 60;
  hours = hours % 24;
  
  char timeStr[16];
  sprintf(timeStr, "%02lu:%02lu:%02lu.%03lu", hours, minutes, seconds, ms % 1000);
  return String(timeStr);
}

// ---------- Helpers ----------
static String bytesToHuman(uint64_t B){ if(B>=1000000ULL) return String((double)B/1e6,3)+" MB";
  if(B>=1000ULL) return String((double)B/1e3,3)+" kB"; return String((uint64_t)B)+" B"; }
static String bitsToHuman(uint64_t b){ if(b>=1000000ULL) return String((double)b/1e6,3)+" Mb";
  if(b>=1000ULL)    return String((double)b/1e3,3)+" kb"; return String((uint64_t)b)+" b"; }
static String speedToHuman(double bps){ if(bps>=1e6) return String(bps/1e6,3)+" Mb/s";
  if(bps>=1e3) return String(bps/1e3,3)+" kb/s"; return String(bps,0)+" b/s"; }
static void macTo12Hex(String& out){ uint64_t mac=ESP.getEfuseMac(); char buf[13];
  sprintf(buf,"%04X%08X",(uint16_t)(mac>>32),(uint32_t)mac); out=String(buf); }
static void sanitizeText(String& s){ for(uint16_t i=0;i<s.length();++i){ char c=s[i]; if(c==','||c=='\r'||c=='\n') s.setCharAt(i,' '); } }
static long toLong(const String& s){ long v=0; bool seen=false; for(uint16_t i=0;i<s.length();++i){ char c=s[i];
  if(c>='0'&&c<='9'){ v=v*10+(c-'0'); seen=true; } else if(seen) break; } return seen? v:-1; }

// ---------- Forward declarations ----------
static bool parseMSG (const String& in, String& src,String& dst,long& seq,String& text);
static bool parseMSGF(const String& in, String& src,String& dst,long& seq,long& idx,long& tot,String& chunk);
static bool parseACKF(const String& in, String& src,String& dst,long& seq,long& idx);
static bool parseACK (const String& in, String& src,String& dst,long& seq,uint64_t& rxTotBytes,uint64_t& rxTotPkts);

static void sendLoRa(const String& payload){ 
  unsigned long txTimestamp = millis();
  
  LoRa.beginPacket(); 
  LoRa.print(payload); 
  LoRa.endPacket(); 
  
  // Parse payload to extract packet details for logging
  String packetType = "";
  long seqNo = -1, fragIdx = -1, totalFrags = -1;
  
  if (payload.startsWith("MSG,")) {
    packetType = "MSG";
    String src, dst, text;
    if (parseMSG(payload, src, dst, seqNo, text)) {
      fragIdx = -1;
      totalFrags = -1;
    }
  } else if (payload.startsWith("MSGF,")) {
    packetType = "MSGF";
    String src, dst, chunk;
    if (parseMSGF(payload, src, dst, seqNo, fragIdx, totalFrags, chunk)) {
      // fragIdx and totalFrags already parsed
    }
  } else if (payload.startsWith("ACK,")) {
    packetType = "ACK";
    String src, dst;
    uint64_t rxBytes, rxPkts;
    if (parseACK(payload, src, dst, seqNo, rxBytes, rxPkts)) {
      fragIdx = -1;
      totalFrags = -1;
    }
  } else if (payload.startsWith("ACKF,")) {
    packetType = "ACKF";
    String src, dst;
    if (parseACKF(payload, src, dst, seqNo, fragIdx)) {
      totalFrags = -1;
    }
  }
  
  // Log transmission data
  logTxData(txTimestamp, packetType, seqNo, fragIdx, totalFrags, payload.length());
  
  // Enhanced serial output with timestamp
  Serial.printf("[TX %s] %s seq #%ld", formatTimestamp(txTimestamp).c_str(), packetType.c_str(), seqNo);
  if (fragIdx >= 0) Serial.printf(" frag %ld/%ld", fragIdx, totalFrags);
  Serial.printf(" | %u bytes\n", (unsigned)payload.length());
}

// ---------- Parsers ----------

// ---------- Parsers ----------
static bool parseMSG (const String& in, String& src,String& dst,long& seq,String& text){
  if(!in.startsWith("MSG,")) return false; int p1=in.indexOf(',',4); if(p1<0) return false;
  int p2=in.indexOf(',',p1+1); if(p2<0) return false; int p3=in.indexOf(',',p2+1); if(p3<0) return false;
  src=in.substring(4,p1); dst=in.substring(p1+1,p2); String seqStr=in.substring(p2+1,p3); text=in.substring(p3+1); seq=toLong(seqStr); return true; }

static bool parseMSGF(const String& in, String& src,String& dst,long& seq,long& idx,long& tot,String& chunk){
  if(!in.startsWith("MSGF,")) return false; int p1=in.indexOf(',',5); if(p1<0) return false;
  int p2=in.indexOf(',',p1+1); if(p2<0) return false; int p3=in.indexOf(',',p2+1); if(p3<0) return false;
  int p4=in.indexOf(',',p3+1); if(p4<0) return false; int p5=in.indexOf(',',p4+1); if(p5<0) return false;
  src=in.substring(5,p1); dst=in.substring(p1+1,p2); String seqStr=in.substring(p2+1,p3);
  String idxStr=in.substring(p3+1,p4); String totStr=in.substring(p4+1,p5); chunk=in.substring(p5+1);
  seq=toLong(seqStr); idx=toLong(idxStr); tot=toLong(totStr); return true; }

static bool parseACKF(const String& in, String& src,String& dst,long& seq,long& idx){
  if(!in.startsWith("ACKF,")) return false; int p1=in.indexOf(',',5); if(p1<0) return false;
  int p2=in.indexOf(',',p1+1); if(p2<0) return false; int p3=in.indexOf(',',p2+1); if(p3<0) return false;
  src=in.substring(5,p1); dst=in.substring(p1+1,p2); String seqStr=in.substring(p2+1,p3); String idxStr=in.substring(p3+1);
  seq=toLong(seqStr); idx=toLong(idxStr); return true; }

static bool parseACK (const String& in, String& src,String& dst,long& seq,uint64_t& rxTotBytes,uint64_t& rxTotPkts){
  if(!in.startsWith("ACK,")) return false; int p1=in.indexOf(',',4); if(p1<0) return false;
  int p2=in.indexOf(',',p1+1); if(p2<0) return false; int p3=in.indexOf(',',p2+1); if(p3<0) return false; int p4=in.indexOf(',',p3+1); if(p4<0) return false;
  src=in.substring(4,p1); dst=in.substring(p1+1,p2); String seqStr=in.substring(p2+1,p3); String bStr=in.substring(p3+1,p4); String kStr=in.substring(p4+1);
  seq=toLong(seqStr); rxTotBytes=(uint64_t)bStr.toDouble(); rxTotPkts=(uint64_t)kStr.toDouble(); return true; }

// ---------- RX reassembly (one in-flight per peer) ----------
String   reSrc=""; long reSeq=-1; long reTot=0; long reGot=0;
String*  reChunks=nullptr; bool* reHave=nullptr;

void resetReasm(){ if(reChunks){ delete[] reChunks; reChunks=nullptr; } if(reHave){ delete[] reHave; reHave=nullptr; } reSrc=""; reSeq=-1; reTot=0; reGot=0; }
void startReasm(const String& src, long seq, long tot){ resetReasm(); reSrc=src; reSeq=seq; reTot=tot; reGot=0; reChunks=new String[tot]; reHave=new bool[tot]; for(long i=0;i<tot;i++) reHave[i]=false; }
bool addFrag(long idx, const String& chunk){ if(idx<0||idx>=reTot) return false; if(!reHave[idx]){ reHave[idx]=true; reChunks[idx]=chunk; reGot++; return true; } return false; }
String joinReasm(){ String out; out.reserve(reTot*FRAG_CHUNK); for(long i=0;i<reTot;i++) out += reChunks[i]; return out; }

// ---------- Blocking waits (consume radio while waiting) ----------
bool waitForAckF(long expectSeq, long expectIdx, unsigned long timeoutMs){
  unsigned long deadline=millis()+timeoutMs;
  while((long)(millis()-deadline)<0){
    int psz=LoRa.parsePacket();
    if(psz){
      unsigned long rxTimestamp = millis();
      String pkt; while(LoRa.available()) pkt+=(char)LoRa.read();
      String s,d,t; long seq=-1,idx=-1,tot=-1; uint64_t b=0,k=0;

      if(parseACKF(pkt,s,d,seq,idx)){
        logRxData(rxTimestamp, "ACKF", seq, idx, -1, pkt.length());
        Serial.printf("[RX %s] ACKF seq #%ld frag %ld | %u bytes\n", 
                      formatTimestamp(rxTimestamp).c_str(), seq, idx, (unsigned)pkt.length());
        if(d==myId && seq==expectSeq && idx==expectIdx) return true; // got it
      }
      else if(parseACK(pkt,s,d,seq,b,k)){ 
        logRxData(rxTimestamp, "ACK", seq, -1, -1, pkt.length());
        Serial.printf("[RX %s] ACK seq #%ld | %u bytes\n", 
                      formatTimestamp(rxTimestamp).c_str(), seq, (unsigned)pkt.length());
      }
      else if(parseMSG(pkt,s,d,seq,t)){
        size_t pktBytes=pkt.length(), textBytes=t.length();
        rxDataPktsTotal++; rxBytesTotal+=textBytes;
        logRxData(rxTimestamp, "MSG", seq, -1, -1, pktBytes);
        Serial.printf("[RX %s] MSG seq #%ld from %s (single while waiting)\n", 
                      formatTimestamp(rxTimestamp).c_str(), seq, s.c_str());
        Serial.printf("     Packet: %u bytes (%s, %s)\n",(unsigned)pktBytes, bitsToHuman((uint64_t)pktBytes*8).c_str(), bytesToHuman(pktBytes).c_str());
        Serial.printf("     Text:   %u bytes (%s) | rxTotal=%llu | rxPkts=%llu\n",(unsigned)textBytes, bytesToHuman(textBytes).c_str(), (unsigned long long)rxBytesTotal, (unsigned long long)rxDataPktsTotal);
        serialPrintLnChunked(t);
        oled3("RX <- ("+String(seq)+")", t.substring(0,16), "txt "+String(textBytes)+"B");
        String ack="ACK,"+myId+","+s+","+String(seq)+","+String((unsigned long long)rxBytesTotal)+","+String((unsigned long long)rxDataPktsTotal);
        sendLoRa(ack);
      }
      else if(parseMSGF(pkt,s,d,seq,idx,tot,t)){
        if(s!=reSrc||seq!=reSeq) startReasm(s,seq,tot);
        bool fresh=addFrag(idx,t);
        if(fresh){ rxDataPktsTotal++; rxBytesTotal+=t.length(); } // unique only
        logRxData(rxTimestamp, "MSGF", seq, idx, tot, pkt.length());
        Serial.printf("[RX %s] MSGF seq #%ld frag %ld/%ld from %s\n", 
                      formatTimestamp(rxTimestamp).c_str(), seq, idx, tot, s.c_str());
        String ackf="ACKF,"+myId+","+s+","+String(seq)+","+String(idx); sendLoRa(ackf);

        bool all=true; for(long i=0;i<reTot;i++) if(!reHave[i]){ all=false; break; }
        if(all){
          String full=joinReasm();
          Serial.printf("[RX FULL] #%ld from %s | total text %uB\n", seq, s.c_str(), (unsigned)full.length());
          serialPrintLnChunked(full);
          oled3("RX <- ("+String(seq)+")","full msg", bytesToHuman(full.length()));
          String ack="ACK,"+myId+","+s+","+String(seq)+","+String((unsigned long long)rxBytesTotal)+","+String((unsigned long long)rxDataPktsTotal);
          sendLoRa(ack); resetReasm();
        }
      }
    }
    delay(1);
  }
  return false; // timeout
}

bool waitForFinalAck(long expectSeq, unsigned long timeoutMs, double &pdrOut, double &bpsOut){
  unsigned long deadline=millis()+timeoutMs; pdrOut=0; bpsOut=0;
  while((long)(millis()-deadline)<0){
    int psz=LoRa.parsePacket();
    if(psz){
      unsigned long rxTimestamp = millis();
      String pkt; while(LoRa.available()) pkt+=(char)LoRa.read();
      String s,d,t; long seq=-1,idx=-1,tot=-1; uint64_t peerBytes=0, peerPkts=0;

      if(parseACK(pkt,s,d,seq,peerBytes,peerPkts)){
        logRxData(rxTimestamp, "ACK", seq, -1, -1, pkt.length());
        if(d==myId && seq==expectSeq){
          unsigned long elapsed=millis()-sessionStartMs;
          bpsOut = elapsed? (peerBytes*8.0*1000.0/elapsed) : 0.0;
          pdrOut = (txDataPktsTotal>0)? (100.0*(double)peerPkts/(double)txDataPktsTotal) : 0.0;
          int rssi=LoRa.packetRssi(); float snr=LoRa.packetSnr();
          Serial.printf("[RX %s] ACK OK seq #%ld from %s | peerRxBytes=%llu | peerRxPkts=%llu | PDR=%.1f%% | %s | RSSI %d | SNR %.1f\n",
                        formatTimestamp(rxTimestamp).c_str(), seq, s.c_str(), (unsigned long long)peerBytes, (unsigned long long)peerPkts,
                        pdrOut, speedToHuman(bpsOut).c_str(), rssi, snr);
          oled3("ACK OK ("+String(seq)+")", "PDR "+String(pdrOut,1)+"%  "+bytesToHuman(peerBytes), speedToHuman(bpsOut));
          return true;
        } else {
          Serial.printf("[RX %s] ACK seq #%ld from %s (not for us)\n", 
                        formatTimestamp(rxTimestamp).c_str(), seq, s.c_str());
        }
      } else {
        // process other traffic while we wait
        String s2,d2; long seq2=-1,idx2=-1,tot2=-1; uint64_t b2=0,k2=0;
        if(parseMSG(pkt,s2,d2,seq2,t)){
          size_t pktBytes=pkt.length(), textBytes=t.length();
          rxDataPktsTotal++; rxBytesTotal+=textBytes;
          logRxData(rxTimestamp, "MSG", seq2, -1, -1, pktBytes);
          Serial.printf("[RX %s] MSG seq #%ld from %s (single while waiting ACK)\n", 
                        formatTimestamp(rxTimestamp).c_str(), seq2, s2.c_str());
          Serial.printf("     Packet: %u bytes (%s, %s)\n",(unsigned)pktBytes, bitsToHuman((uint64_t)pktBytes*8).c_str(), bytesToHuman(pktBytes).c_str());
          Serial.printf("     Text:   %u bytes (%s) | rxTotal=%llu | rxPkts=%llu\n",(unsigned)textBytes, bytesToHuman(textBytes).c_str(), (unsigned long long)rxBytesTotal, (unsigned long long)rxDataPktsTotal);
          serialPrintLnChunked(t);
          oled3("RX <- ("+String(seq2)+")", t.substring(0,16), "txt "+String(textBytes)+"B");
          String ack="ACK,"+myId+","+s2+","+String(seq2)+","+String((unsigned long long)rxBytesTotal)+","+String((unsigned long long)rxDataPktsTotal);
          sendLoRa(ack);
        } else if(parseMSGF(pkt,s2,d2,seq2,idx2,tot2,t)){
          if(s2!=reSrc||seq2!=reSeq) startReasm(s2,seq2,tot2);
          bool fresh=addFrag(idx2,t);
          if(fresh){ rxDataPktsTotal++; rxBytesTotal+=t.length(); }
          logRxData(rxTimestamp, "MSGF", seq2, idx2, tot2, pkt.length());
          Serial.printf("[RX %s] MSGF seq #%ld frag %ld/%ld from %s\n", 
                        formatTimestamp(rxTimestamp).c_str(), seq2, idx2, tot2, s2.c_str());
          String ackf="ACKF,"+myId+","+s2+","+String(seq2)+","+String(idx2); sendLoRa(ackf);
          bool all=true; for(long i=0;i<reTot;i++) if(!reHave[i]){ all=false; break; }
          if(all){
            String full=joinReasm();
            Serial.printf("[RX FULL] #%ld from %s | total text %uB\n", seq2, s2.c_str(), (unsigned)full.length());
            serialPrintLnChunked(full);
            oled3("RX <- ("+String(seq2)+")","full msg", bytesToHuman(full.length()));
            String ack="ACK,"+myId+","+s2+","+String(seq2)+","+String((unsigned long long)rxBytesTotal)+","+String((unsigned long long)rxDataPktsTotal);
            sendLoRa(ack); resetReasm();
          }
        } else if(parseACKF(pkt,s2,d2,seq2,idx2)){
          logRxData(rxTimestamp, "ACKF", seq2, idx2, -1, pkt.length());
          Serial.printf("[RX %s] ACKF seq #%ld frag %ld (stray)\n", 
                        formatTimestamp(rxTimestamp).c_str(), seq2, idx2);
        }
      }
    }
    delay(1);
  }
  return false; // timeout
}

// ---------- Send one message reliably (with EXACT tries) ----------
bool sendMessageReliable(const String& lineIn){
  String line=lineIn; sanitizeText(line);
  const bool single = (line.length() <= FRAG_CHUNK);
  const size_t L=line.length();
  const size_t total = single ? 1 : (L + FRAG_CHUNK - 1)/FRAG_CHUNK;

  // Reserve a seq for this logical message and KEEP IT across attempts
  uint32_t seq = txSeq;
  txSeq++;

  for(int attempt=1; attempt<=MSG_MAX_TRIES; ++attempt){
    Serial.printf("[ATTEMPT %d/%d] seq #%lu\n", attempt, MSG_MAX_TRIES, (unsigned long)seq);

    if(single){
      String payload="MSG,"+myId+","+dstAny+","+String(seq)+","+line;
      sendLoRa(payload);
      size_t pktBytes=payload.length(), textBytes=line.length();
      txDataPktsTotal++; txBytesTotal+=textBytes;

      Serial.printf("  Text: %u bytes (%s) | txTotal=%llu | txPkts=%llu\n",
                    (unsigned)textBytes, bytesToHuman(textBytes).c_str(),
                    (unsigned long long)txBytesTotal, (unsigned long long)txDataPktsTotal);
      oled3("TX -> ("+String(seq)+")", line.substring(0,16), "txt "+String(textBytes)+"B");

      double pdr=0,bps=0;
      if(waitForFinalAck(seq, BASE_FINAL_ACK_TIMEOUT_MS, pdr, bps)) return true;

      Serial.println("  -> final ACK timeout, will retry whole message");
      delay(100);
    } else {
      bool fragFailed=false;

      for(size_t i=0;i<total;i++){
        size_t off=i*FRAG_CHUNK;
        String chunk=line.substring(off, min(L, off+FRAG_CHUNK));
        String payload="MSGF,"+myId+","+dstAny+","+String(seq)+","+String(i)+","+String(total)+","+chunk;

        bool ok=false;
        for(int ftry=1; ftry<=FRAG_MAX_TRIES; ++ftry){
          sendLoRa(payload);
          size_t pktBytes=payload.length();
          txDataPktsTotal++; txBytesTotal+=chunk.length();

          Serial.printf("  Text chunk: %u bytes (%s) | try %d/%d\n",
                        (unsigned)chunk.length(), bytesToHuman(chunk.length()).c_str(),
                        ftry, FRAG_MAX_TRIES);

          ok = waitForAckF((long)seq, (long)i, FRAG_ACK_TIMEOUT_MS);
          if(ok) break;

          if(ftry < FRAG_MAX_TRIES) Serial.println("   -> no ACKF, retrying...");
          else                      Serial.println("   -> no ACKF, giving up fragment");
          delay(FRAG_SPACING_MS);
        }

        if(!ok){ fragFailed=true; break; }
      }

      if(fragFailed){
        Serial.println("  -> fragment failed after retries, will retry whole message");
        delay(150);
      } else {
        double pdr=0,bps=0;
        unsigned long finalWait = BASE_FINAL_ACK_TIMEOUT_MS + total*500;
        if(waitForFinalAck(seq, finalWait, pdr, bps)) return true;

        Serial.println("  -> final ACK timeout, will retry whole message");
        delay(150);
      }
    }
  }

  Serial.println("[ABORT] message failed after MSG_MAX_TRIES");
  oled3("SEND FAILED","after retries","");
  return false;
}

// ---------- Serial Command Processing ----------

// List all files in SPIFFS
void listSPIFFSFiles() {
  Serial.println("=== SPIFFS File List ===");
  
  File root = SPIFFS.open("/");
  if (!root) {
    Serial.println("Failed to open directory");
    return;
  }
  
  File file = root.openNextFile();
  while (file) {
    Serial.print("FILE: ");
    Serial.print(file.name());
    Serial.print(" (");
    Serial.print(file.size());
    Serial.println(" bytes)");
    file = root.openNextFile();
  }
  Serial.println("=== End File List ===");
}

// Download a specific file via serial
void downloadFile(const String& filename) {
  if (!SPIFFS.exists(filename)) {
    Serial.println("ERROR: File not found: " + filename);
    return;
  }
  
  File file = SPIFFS.open(filename, "r");
  if (!file) {
    Serial.println("ERROR: Failed to open file: " + filename);
    return;
  }
  
  Serial.println("=== FILE_START: " + filename + " ===");
  Serial.println("SIZE: " + String(file.size()) + " bytes");
  Serial.println("--- DATA_START ---");
  
  // Read and output file contents
  while (file.available()) {
    String line = file.readStringUntil('\n');
    Serial.println(line);
  }
  
  Serial.println("--- DATA_END ---");
  Serial.println("=== FILE_END: " + filename + " ===");
  
  file.close();
}

// Download current session TX file
void downloadCurrentTxFile() {
  if (csvTimestamp.length() == 0) {
    Serial.println("ERROR: No CSV session active");
    return;
  }
  String filename = "/tx_data_" + csvTimestamp + ".csv";
  downloadFile(filename);
}

// Download current session RX file
void downloadCurrentRxFile() {
  if (csvTimestamp.length() == 0) {
    Serial.println("ERROR: No CSV session active");
    return;
  }
  String filename = "/rx_data_" + csvTimestamp + ".csv";
  downloadFile(filename);
}

// Show help for serial commands
void showHelp() {
  Serial.println("=== Serial Commands ===");
  Serial.println("HELP          - Show this help");
  Serial.println("LIST          - List all files in SPIFFS");
  Serial.println("DOWNLOAD_TX   - Download current TX CSV file");
  Serial.println("DOWNLOAD_RX   - Download current RX CSV file");
  Serial.println("DOWNLOAD:<filename> - Download specific file");
  Serial.println("STATS         - Show current session statistics");
  Serial.println("Normal text   - Send as LoRa message");
  Serial.println("=========================");
}

// Show current session statistics
void showStats() {
  Serial.println("=== Session Statistics ===");
  Serial.println("Session time: " + formatTimestamp(millis() - sessionStartMs));
  Serial.println("Node ID: " + myId);
  Serial.println("TX packets: " + String(txDataPktsTotal));
  Serial.println("RX packets: " + String(rxDataPktsTotal));
  Serial.println("TX bytes: " + String(txBytesTotal));
  Serial.println("RX bytes: " + String(rxBytesTotal));
  Serial.println("CSV logging: " + String(csvLoggingEnabled ? "ENABLED" : "DISABLED"));
  if (csvLoggingEnabled) {
    Serial.println("Current TX file: /tx_data_" + csvTimestamp + ".csv");
    Serial.println("Current RX file: /rx_data_" + csvTimestamp + ".csv");
  }
  Serial.println("==========================");
}

// Process serial commands
void processSerialCommand(const String& command) {
  String cmd = command;
  cmd.toUpperCase();
  cmd.trim();
  
  if (cmd == "HELP" || cmd == "?") {
    showHelp();
  } else if (cmd == "LIST") {
    listSPIFFSFiles();
  } else if (cmd == "DOWNLOAD_TX") {
    downloadCurrentTxFile();
  } else if (cmd == "DOWNLOAD_RX") {
    downloadCurrentRxFile();
  } else if (cmd.startsWith("DOWNLOAD:")) {
    String filename = cmd.substring(9);
    if (!filename.startsWith("/")) filename = "/" + filename;
    downloadFile(filename);
  } else if (cmd == "STATS") {
    showStats();
  } else {
    // Not a command, treat as normal LoRa message
    if (command.length()) sendMessageReliable(command);
  }
}

// ---------- Setup / Loop ----------
void setup(){
  Serial.begin(115200); Serial.setTimeout(10);
  Wire.begin(); if(!display.begin(SSD1306_SWITCHCAPVCC,0x3C)){ Serial.println("SSD1306 fail"); for(;;); }

#ifdef MY_NODE_ID
  myId = String(MY_NODE_ID);
#else
  macTo12Hex(myId);  // full 48-bit MAC -> 12 hex
#endif

  SPI.begin(SCK,MISO,MOSI,SS); LoRa.setPins(SS,RST,DIO0);
  if(!LoRa.begin(FREQ_HZ)){ oled3("LoRa init FAILED","Check wiring/freq"); for(;;); }
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSyncWord(LORA_SYNC);
  LoRa.enableCrc();
  LoRa.setTxPower(17, PA_OUTPUT_PA_BOOST_PIN);
  // Optional AS923-friendly robustness:
  // LoRa.setSignalBandwidth(125E3);   // AS923 uses 125 kHz channels
  // LoRa.setCodingRate4(5);           // 4/5 (default); 4/6..4/8 add redundancy

  sessionStartMs = millis();
  resetReasm();

  // Initialize CSV logging for timing analysis
  if (initCSVLogging()) {
    Serial.println("✅ CSV timing logging enabled");
  } else {
    Serial.println("⚠  CSV timing logging disabled (SPIFFS error)");
  }

  oled3("LoRa Chat Ready","ID: "+myId,"923 MHz, SF="+String(LORA_SF));
  Serial.println("=== LoRa Chat (Reliable + Exact Tries + Timing Analysis) — AS923 (923 MHz) ===");
  Serial.println("115200, Newline. Type and Enter.");
  Serial.print("Node ID: "); Serial.println(myId);
  Serial.println("");
  Serial.println("📋 Serial Commands Available:");
  Serial.println("   HELP          - Show command help");
  Serial.println("   LIST          - List CSV files");
  Serial.println("   DOWNLOAD_TX   - Download TX data");
  Serial.println("   DOWNLOAD_RX   - Download RX data");
  Serial.println("   STATS         - Show statistics");
  Serial.println("   Or type any message to send via LoRa");
  Serial.println("");
}

void loop(){
  // 1) Process serial input (commands or messages)
  if(Serial.available()){
    String line=Serial.readStringUntil('\n'); line.trim();
    processSerialCommand(line);
  }

  // 2) Otherwise receive and serve peers
  int psz=LoRa.parsePacket();
  if(psz){
    unsigned long rxTimestamp = millis();
    String pkt; while(LoRa.available()) pkt+=(char)LoRa.read();
    String s,d,t; long seq=-1,idx=-1,tot=-1; uint64_t b=0,k=0;

    if(parseACK(pkt,s,d,seq,b,k)){
      logRxData(rxTimestamp, "ACK", seq, -1, -1, pkt.length());
      Serial.printf("[RX %s] ACK stray seq #%ld from %s | rxBytes=%llu rxPkts=%llu\n",
                    formatTimestamp(rxTimestamp).c_str(), seq, s.c_str(), (unsigned long long)b, (unsigned long long)k);
      return;
    }
    if(parseACKF(pkt,s,d,seq,idx)){
      logRxData(rxTimestamp, "ACKF", seq, idx, -1, pkt.length());
      Serial.printf("[RX %s] ACKF stray seq #%ld frag %ld\n", 
                    formatTimestamp(rxTimestamp).c_str(), seq, idx);
      return;
    }
    if(parseMSG(pkt,s,d,seq,t)){
      size_t pktBytes=pkt.length(), textBytes=t.length();
      rxDataPktsTotal++; rxBytesTotal+=textBytes;
      logRxData(rxTimestamp, "MSG", seq, -1, -1, pktBytes);
      Serial.printf("[RX %s] MSG seq #%ld from %s (single)\n", 
                    formatTimestamp(rxTimestamp).c_str(), seq, s.c_str());
      Serial.printf("     Packet: %u bytes (%s, %s)\n",(unsigned)pktBytes, bitsToHuman((uint64_t)pktBytes*8).c_str(), bytesToHuman(pktBytes).c_str());
      Serial.printf("     Text:   %u bytes (%s) | rxTotal=%llu | rxPkts=%llu\n",
                    (unsigned)textBytes, bytesToHuman(textBytes).c_str(),
                    (unsigned long long)rxBytesTotal, (unsigned long long)rxDataPktsTotal);
      serialPrintLnChunked(t);
      oled3("RX <- ("+String(seq)+")", t.substring(0,16), "txt "+String(textBytes)+"B");
      String ack="ACK,"+myId+","+s+","+String(seq)+","+String((unsigned long long)rxBytesTotal)+","+String((unsigned long long)rxDataPktsTotal);
      sendLoRa(ack); return;
    }
    if(parseMSGF(pkt,s,d,seq,idx,tot,t)){
      if(s!=reSrc||seq!=reSeq) startReasm(s,seq,tot);
      bool fresh=addFrag(idx,t);
      if(fresh){ rxDataPktsTotal++; rxBytesTotal+=t.length(); }
      logRxData(rxTimestamp, "MSGF", seq, idx, tot, pkt.length());
      Serial.printf("[RX %s] MSGF seq #%ld frag %ld/%ld from %s\n", 
                    formatTimestamp(rxTimestamp).c_str(), seq, idx, tot, s.c_str());
      String ackf="ACKF,"+myId+","+s+","+String(seq)+","+String(idx); sendLoRa(ackf);

      bool all=true; for(long i=0;i<reTot;i++) if(!reHave[i]){ all=false; break; }
      if(all){
        String full=joinReasm();
        Serial.printf("[RX FULL] #%ld from %s | total text %uB\n", seq, s.c_str(), (unsigned)full.length());
        serialPrintLnChunked(full);
        oled3("RX <- ("+String(seq)+")","full msg", bytesToHuman(full.length()));
        String ack="ACK,"+myId+","+s+","+String(seq)+","+String((unsigned long long)rxBytesTotal)+","+String((unsigned long long)rxDataPktsTotal);
        sendLoRa(ack); resetReasm();
      }
      return;
    }
    // else: unknown payload, ignore
  }

  delay(1);
}
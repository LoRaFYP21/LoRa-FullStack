/*
  LoRa Serial Chat (Reliable Fragments + Exact Tries) + PDR + Goodput + Distance
  ESP32 T-Display + SSD1306 + SX127x (Sandeep Mistry LoRa lib)
  === AS923 variant (923 MHz) with header-aware fragment sizing (<= 255 B) ===

  New: RSSI-based distance estimate (meters). Calibrate RSSI_REF_1M and PATH_LOSS_N.
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SPI.h>
#include <LoRa.h>

// ---------- Radio config (AS923) ----------
#define FREQ_HZ   923E6
#define LORA_SYNC 0xA5
#define LORA_SF   8          // raise if link is weak (9..12)
const size_t LORA_MAX_PAYLOAD = 255;  // SX127x FIFO/payload byte limit

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

// ---------- Distance estimation (RSSI-based) ----------
// Calibrate these:
const float RSSI_REF_1M   = -45.0f;   // avg RSSI at ~1 m (measure for your hardware)
const float PATH_LOSS_N   = 2.7f;     // ~2.0 LOS outdoor, 2.7–3.5 urban, 3–5 indoor
// Smoothing
float rssiEma = NAN;
const float RSSI_ALPHA = 0.20f;       // 0..1 (higher = less smoothing)

// RSSI → distance (meters) using log-distance model with 1 m reference.
float estimateDistanceMetersFromRSSI(int rssi) {
  float r = (float)rssi;
  if (isnan(rssiEma)) rssiEma = r;
  rssiEma = RSSI_ALPHA * r + (1.0f - RSSI_ALPHA) * rssiEma;
  float exponent = (RSSI_REF_1M - rssiEma) / (10.0f * PATH_LOSS_N);
  float d = powf(10.0f, exponent);
  return d;
}

// OPTIONAL FSPL variant if you know true TX power & antenna gains
float estimateDistanceMetersFSPL(int rssi, float txPowerDbm, float gTx_dBi=0, float gRx_dBi=0) {
  const float f_MHz = 923.0f;
  float FSPL_dB = txPowerDbm + gTx_dBi + gRx_dBi - (float)rssi;
  float d_km = powf(10.0f, (FSPL_dB - 32.44f - 20.0f*log10f(f_MHz)) / 20.0f);
  return d_km * 1000.0f;
}

// ---------- IDs & counters ----------
String myId="????????????", dstAny="FF";           // 12-hex MAC, broadcast dst
uint64_t txDataPktsTotal=0, rxDataPktsTotal=0;     // counts MSG + unique MSGF frags
uint64_t txBytesTotal=0,   rxBytesTotal=0;         // app TEXT bytes totals

// ---------- Timing / ARQ knobs ----------
size_t  FRAG_CHUNK = 230;                          // desired text bytes/frag (auto-shrinks)
const int     FRAG_MAX_TRIES = 8;                  // per-fragment attempts
const unsigned long FRAG_ACK_TIMEOUT_MS = 1000;    // wait for ACKF
const unsigned long FRAG_SPACING_MS     = 15;      // small guard between tries

const unsigned long BASE_FINAL_ACK_TIMEOUT_MS = 1800; // final ACK wait baseline
const int     MSG_MAX_TRIES = 3;                   // whole-message attempts

uint32_t txSeq=0;
unsigned long sessionStartMs=0;

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
static void sendLoRa(const String& payload){ LoRa.beginPacket(); LoRa.print(payload); LoRa.endPacket(); }
static long toLong(const String& s){ long v=0; bool seen=false; for(uint16_t i=0;i<s.length();++i){ char c=s[i];
  if(c>='0'&&c<='9'){ v=v*10+(c-'0'); seen=true; } else if(seen) break; } return seen? v:-1; }
static int digitsU(unsigned long v){ int d=1; while(v>=10){ v/=10; d++; } return d; }

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
      String pkt; while(LoRa.available()) pkt+=(char)LoRa.read();
      String s,d,t; long seq=-1,idx=-1,tot=-1; uint64_t b=0,k=0;

      if(parseACKF(pkt,s,d,seq,idx)){
        if(d==myId && seq==expectSeq && idx==expectIdx) return true;
      }
      else if(parseACK(pkt,s,d,seq,b,k)){ /* ignore here */ }
      else if(parseMSG(pkt,s,d,seq,t)){
        int rssi = LoRa.packetRssi(); float snr = LoRa.packetSnr();
        float d_m = estimateDistanceMetersFromRSSI(rssi);

        size_t pktBytes=pkt.length(), textBytes=t.length();
        rxDataPktsTotal++; rxBytesTotal+=textBytes;
        Serial.printf("[RX] #%ld from %s (single while waiting)\n", seq, s.c_str());
        Serial.printf("     Packet: %u bytes (%s, %s)\n",(unsigned)pktBytes, bitsToHuman((uint64_t)pktBytes*8).c_str(), bytesToHuman(pktBytes).c_str());
        Serial.printf("     Text:   %u bytes (%s) | rxTotal=%llu | rxPkts=%llu | RSSI %d | SNR %.1f | d~%.1fm\n",
                      (unsigned)textBytes, bytesToHuman(textBytes).c_str(),
                      (unsigned long long)rxBytesTotal, (unsigned long long)rxDataPktsTotal,
                      rssi, snr, d_m);
        serialPrintLnChunked(t);
        oled3("RX <- ("+String(seq)+")", t.substring(0,16),
              "d~"+String(d_m,1)+"m | RSSI "+String(rssi));
        String ack="ACK,"+myId+","+s+","+String(seq)+","+String((unsigned long long)rxBytesTotal)+","+String((unsigned long long)rxDataPktsTotal);
        sendLoRa(ack);
      }
      else if(parseMSGF(pkt,s,d,seq,idx,tot,t)){
        int rssi = LoRa.packetRssi(); float snr = LoRa.packetSnr();
        float d_m = estimateDistanceMetersFromRSSI(rssi);

        if(s!=reSrc||seq!=reSeq) startReasm(s,seq,tot);
        bool fresh=addFrag(idx,t);
        if(fresh){ rxDataPktsTotal++; rxBytesTotal+=t.length(); }
        String ackf="ACKF,"+myId+","+s+","+String(seq)+","+String(idx); sendLoRa(ackf);

        bool all=true; for(long i=0;i<reTot;i++) if(!reHave[i]){ all=false; break; }
        if(all){
          String full=joinReasm();
          Serial.printf("[RX FULL] #%ld from %s | total text %uB | RSSI %d | SNR %.1f | d~%.1fm\n",
                        seq, s.c_str(), (unsigned)full.length(), rssi, snr, d_m);
          serialPrintLnChunked(full);
          oled3("RX <- ("+String(seq)+")","full msg",
                "d~"+String(d_m,1)+"m | RSSI "+String(rssi));
          String ack="ACK,"+myId+","+s+","+String(seq)+","+String((unsigned long long)rxBytesTotal)+","+String((unsigned long long)rxDataPktsTotal);
          sendLoRa(ack); resetReasm();
        }
      }
    }
    delay(1);
  }
  return false;
}

bool waitForFinalAck(long expectSeq, unsigned long timeoutMs, double &pdrOut, double &bpsOut){
  unsigned long deadline=millis()+timeoutMs; pdrOut=0; bpsOut=0;
  while((long)(millis()-deadline)<0){
    int psz=LoRa.parsePacket();
    if(psz){
      String pkt; while(LoRa.available()) pkt+=(char)LoRa.read();
      String s,d,t; long seq=-1,idx=-1,tot=-1; uint64_t peerBytes=0, peerPkts=0;

      if(parseACK(pkt,s,d,seq,peerBytes,peerPkts)){
        if(d==myId && seq==expectSeq){
          unsigned long elapsed=millis()-sessionStartMs;
          bpsOut = elapsed? (peerBytes*8.0*1000.0/elapsed) : 0.0;
          pdrOut = (txDataPktsTotal>0)? (100.0*(double)peerPkts/(double)txDataPktsTotal) : 0.0;

          int rssi=LoRa.packetRssi(); float snr=LoRa.packetSnr();
          float d_m = estimateDistanceMetersFromRSSI(rssi);

          Serial.printf("[ACK OK] #%ld from %s | peerRxBytes=%llu | peerRxPkts=%llu | PDR=%.1f%% | %s | RSSI %d | SNR %.1f | d~%.1fm\n",
                        seq, s.c_str(), (unsigned long long)peerBytes, (unsigned long long)peerPkts,
                        pdrOut, speedToHuman(bpsOut).c_str(), rssi, snr, d_m);
          oled3("ACK OK ("+String(seq)+")",
                "PDR "+String(pdrOut,1)+"%  "+bytesToHuman(peerBytes),
                "d~"+String(d_m,1)+"m  "+speedToHuman(bpsOut));
          return true;
        }
      } else {
        // process other traffic while we wait
        String s2,d2; long seq2=-1,idx2=-1,tot2=-1; uint64_t b2=0,k2=0;
        if(parseMSG(pkt,s2,d2,seq2,t)){
          int rssi = LoRa.packetRssi(); float snr = LoRa.packetSnr();
          float d_m = estimateDistanceMetersFromRSSI(rssi);

          size_t pktBytes=pkt.length(), textBytes=t.length();
          rxDataPktsTotal++; rxBytesTotal+=textBytes;
          Serial.printf("[RX] #%ld from %s (single while waiting ACK)\n", seq2, s2.c_str());
          Serial.printf("     Packet: %u bytes (%s, %s)\n",(unsigned)pktBytes, bitsToHuman((uint64_t)pktBytes*8).c_str(), bytesToHuman(pktBytes).c_str());
          Serial.printf("     Text:   %u bytes (%s) | rxTotal=%llu | rxPkts=%llu | RSSI %d | SNR %.1f | d~%.1fm\n",
                        (unsigned)textBytes, bytesToHuman(textBytes).c_str(),
                        (unsigned long long)rxBytesTotal, (unsigned long long)rxDataPktsTotal,
                        rssi, snr, d_m);
          serialPrintLnChunked(t);
          oled3("RX <- ("+String(seq2)+")", t.substring(0,16),
                "d~"+String(d_m,1)+"m | RSSI "+String(rssi));
          String ack="ACK,"+myId+","+s2+","+String(seq2)+","+String((unsigned long long)rxBytesTotal)+","+String((unsigned long long)rxDataPktsTotal);
          sendLoRa(ack);
        } else if(parseMSGF(pkt,s2,d2,seq2,idx2,tot2,t)){
          int rssi = LoRa.packetRssi(); float snr = LoRa.packetSnr();
          float d_m = estimateDistanceMetersFromRSSI(rssi);

          if(s2!=reSrc||seq2!=reSeq) startReasm(s2,seq2,tot2);
          bool fresh=addFrag(idx2,t);
          if(fresh){ rxDataPktsTotal++; rxBytesTotal+=t.length(); }
          String ackf="ACKF,"+myId+","+s2+","+String(seq2)+","+String(idx2); sendLoRa(ackf);
          bool all=true; for(long i=0;i<reTot;i++) if(!reHave[i]){ all=false; break; }
          if(all){
            String full=joinReasm();
            Serial.printf("[RX FULL] #%ld from %s | total text %uB | RSSI %d | SNR %.1f | d~%.1fm\n",
                          seq2, s2.c_str(), (unsigned)full.length(), rssi, snr, d_m);
            serialPrintLnChunked(full);
            oled3("RX <- ("+String(seq2)+")","full msg",
                  "d~"+String(d_m,1)+"m | RSSI "+String(rssi));
            String ack="ACK,"+myId+","+s2+","+String(seq2)+","+String((unsigned long long)rxBytesTotal)+","+String((unsigned long long)rxDataPktsTotal);
            sendLoRa(ack); resetReasm();
          }
        } else if(parseACKF(pkt,s2,d2,seq2,idx2)){
          // ignore here; fragment sender's loop is waiting for its own ACKF
        }
      }
    }
    delay(1);
  }
  return false;
}

// ---------- Send one message reliably (with EXACT tries) ----------
bool sendMessageReliable(const String& lineIn){
  String line=lineIn; sanitizeText(line);
  const size_t L=line.length();
  const bool single = (L <= FRAG_CHUNK);
  const size_t total = single ? 1 : (L + FRAG_CHUNK - 1)/FRAG_CHUNK;

  // Reserve a seq for this logical message and KEEP IT across attempts
  uint32_t seq = txSeq; txSeq++;

  for(int attempt=1; attempt<=MSG_MAX_TRIES; ++attempt){
    Serial.printf("[ATTEMPT %d/%d] seq #%lu\n", attempt, MSG_MAX_TRIES, (unsigned long)seq);

    if(single){
      // Ensure single MSG stays <= 255 if someone raised FRAG_CHUNK a lot
      size_t hdrLen = 4 + myId.length() + 1 + dstAny.length() + 1 + digitsU(seq) + 1; // "MSG," + src + "," + dst + "," + seq + ","
      size_t maxText = (hdrLen < LORA_MAX_PAYLOAD) ? (LORA_MAX_PAYLOAD - hdrLen) : 0;
      String text = (L <= maxText) ? line : line.substring(0, maxText);

      String payload="MSG,"+myId+","+dstAny+","+String(seq)+","+text;
      sendLoRa(payload);

      size_t pktBytes=payload.length(), textBytes=text.length();
      txDataPktsTotal++; txBytesTotal+=textBytes;

      Serial.printf("  [TX] single %u bytes (%s, %s) | txt %uB\n",
                    (unsigned)pktBytes, bitsToHuman((uint64_t)pktBytes*8).c_str(), bytesToHuman(pktBytes).c_str(),
                    (unsigned)textBytes);
      oled3("TX -> ("+String(seq)+")", text.substring(0,16), "txt "+String(textBytes)+"B");

      double pdr=0,bps=0;
      if(waitForFinalAck(seq, BASE_FINAL_ACK_TIMEOUT_MS, pdr, bps)) return true;

      Serial.println("  -> final ACK timeout, will retry whole message");
      delay(100);
    } else {
      bool fragFailed=false;

      for(size_t i=0;i<total;i++){
        size_t off=i*FRAG_CHUNK;
        // Base desired chunk
        String chunk=line.substring(off, min(L, off+FRAG_CHUNK));
        // Compute THIS fragment's header length and shrink to fit <= 255
        size_t hdrLen = 5                                        // "MSGF,"
                        + myId.length() + 1                      // src + ','
                        + dstAny.length() + 1                    // dst + ','
                        + digitsU(seq) + 1                       // seq + ','
                        + digitsU(i) + 1                         // idx + ','
                        + digitsU(total) + 1;                    // tot + ','
        size_t maxChunk = (hdrLen < LORA_MAX_PAYLOAD) ? (LORA_MAX_PAYLOAD - hdrLen) : 0;
        if(chunk.length() > maxChunk) chunk.remove(maxChunk);    // auto-shrink

        String payload="MSGF,"+myId+","+dstAny+","+String(seq)+","+String(i)+","+String(total)+","+chunk;

        bool ok=false;
        for(int ftry=1; ftry<=FRAG_MAX_TRIES; ++ftry){
          sendLoRa(payload);
          size_t pktBytes=payload.length();
          txDataPktsTotal++; txBytesTotal+=chunk.length();

          Serial.printf("  [TX FRAG %u/%u try %d] %u bytes (%s, %s) | txt %uB\n",
                        (unsigned)(i+1), (unsigned)total, ftry, (unsigned)pktBytes,
                        bitsToHuman((uint64_t)pktBytes*8).c_str(), bytesToHuman(pktBytes).c_str(),
                        (unsigned)chunk.length());

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
  // AS923-friendly defaults:
  LoRa.setSignalBandwidth(125E3);   // typical AS923 channel BW
  LoRa.setCodingRate4(5);           // 4/5 (default) — raise for robustness if needed

  sessionStartMs = millis();
  resetReasm();

  oled3("LoRa Chat Ready","ID: "+myId,"923 MHz, SF="+String(LORA_SF));
  Serial.println("=== LoRa Chat (Reliable + Exact Tries) — AS923 (923 MHz) ===");
  Serial.println("115200, Newline. Type and Enter.");
  Serial.print("Node ID: "); Serial.println(myId);
}

void loop(){
  // 1) Send when user types
  if(Serial.available()){
    String line=Serial.readStringUntil('\n'); line.trim();
    if(line.length()) sendMessageReliable(line);
  }

  // 2) Otherwise receive and serve peers
  int psz=LoRa.parsePacket();
  if(psz){
    String pkt; while(LoRa.available()) pkt+=(char)LoRa.read();
    String s,d,t; long seq=-1,idx=-1,tot=-1; uint64_t b=0,k=0;

    if(parseACK(pkt,s,d,seq,b,k)){
      int rssi = LoRa.packetRssi(); float snr = LoRa.packetSnr();
      float d_m = estimateDistanceMetersFromRSSI(rssi);
      Serial.printf("[ACK stray] #%ld from %s | rxBytes=%llu rxPkts=%llu | RSSI %d | SNR %.1f | d~%.1fm\n",
                    seq, s.c_str(), (unsigned long long)b, (unsigned long long)k, rssi, snr, d_m);
      return;
    }
    if(parseACKF(pkt,s,d,seq,idx)){
      // stray ACKF (we aren't waiting here)
      return;
    }
    if(parseMSG(pkt,s,d,seq,t)){
      int rssi = LoRa.packetRssi(); float snr = LoRa.packetSnr();
      float d_m = estimateDistanceMetersFromRSSI(rssi);

      size_t pktBytes=pkt.length(), textBytes=t.length();
      rxDataPktsTotal++; rxBytesTotal+=textBytes;
      Serial.printf("[RX] #%ld from %s (single)\n", seq, s.c_str());
      Serial.printf("     Packet: %u bytes (%s, %s)\n",(unsigned)pktBytes, bitsToHuman((uint64_t)pktBytes*8).c_str(), bytesToHuman(pktBytes).c_str());
      Serial.printf("     Text:   %u bytes (%s) | rxTotal=%llu | rxPkts=%llu | RSSI %d | SNR %.1f | d~%.1fm\n",
                    (unsigned)textBytes, bytesToHuman(textBytes).c_str(),
                    (unsigned long long)rxBytesTotal, (unsigned long long)rxDataPktsTotal,
                    rssi, snr, d_m);
      serialPrintLnChunked(t);
      oled3("RX <- ("+String(seq)+")", t.substring(0,16),
            "d~"+String(d_m,1)+"m | RSSI "+String(rssi));
      String ack="ACK,"+myId+","+s+","+String(seq)+","+String((unsigned long long)rxBytesTotal)+","+String((unsigned long long)rxDataPktsTotal);
      sendLoRa(ack); return;
    }
    if(parseMSGF(pkt,s,d,seq,idx,tot,t)){
      int rssi = LoRa.packetRssi(); float snr = LoRa.packetSnr();
      float d_m = estimateDistanceMetersFromRSSI(rssi);

      if(s!=reSrc||seq!=reSeq) startReasm(s,seq,tot);
      bool fresh=addFrag(idx,t);
      if(fresh){ rxDataPktsTotal++; rxBytesTotal+=t.length(); }
      String ackf="ACKF,"+myId+","+s+","+String(seq)+","+String(idx); sendLoRa(ackf);

      bool all=true; for(long i=0;i<reTot;i++) if(!reHave[i]){ all=false; break; }
      if(all){
        String full=joinReasm();
        Serial.printf("[RX FULL] #%ld from %s | total text %uB | RSSI %d | SNR %.1f | d~%.1fm\n",
                      seq, s.c_str(), (unsigned)full.length(), rssi, snr, d_m);
        serialPrintLnChunked(full);
        oled3("RX <- ("+String(seq)+")","full msg",
              "d~"+String(d_m,1)+"m | RSSI "+String(rssi));
        String ack="ACK,"+myId+","+s+","+String(seq)+","+String((unsigned long long)rxBytesTotal)+","+String((unsigned long long)rxDataPktsTotal);
        sendLoRa(ack); resetReasm();
      }
      return;
    }
    // else: unknown payload, ignore
  }

  delay(1);
}

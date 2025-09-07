/*
  LoRa Serial Chat (Reliable Fragments + Exact Tries) + PDR + Goodput
  ESP32 T-Display + SSD1306 + SX127x (Sandeep Mistry LoRa lib)

  - If text <= FRAG_CHUNK: send one MSG, wait for final ACK.
  - If text  > FRAG_CHUNK: split into MSGF fragments.
      For each fragment i:
        send -> wait ACKF(seq,i) with timeout; retry up to FRAG_MAX_TRIES (exact).
      After last fragment is ACKF’d, wait for final ACK.
      If final ACK fails, retry the WHOLE message up to MSG_MAX_TRIES (exact).

  Packet formats:
    MSG,  <src>,<dst>,<seq>,<text>
    MSGF, <src>,<dst>,<seq>,<fragIdx>,<fragTot>,<chunk>
    ACKF, <src>,<dst>,<seq>,<fragIdx>                      (per-fragment ack)
    ACK,  <src>,<dst>,<seq>,<rxBytesTotal>,<rxPktsTotal>  (final ack)

  Sender prints per-packet sizes, PDR (%), and goodput (b/s).
  Receiver prints per-packet sizes and cumulative totals.

  Notes:
  - Unique IDs: full 48-bit efuse MAC (12 hex). If needed, hard-set MY_NODE_ID below.
  - PDR is computed at the sender as peerRxPktsTotal / txDataPktsTotal (session-wide).
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SPI.h>
#include <LoRa.h>

// ---------- Radio config ----------
#define FREQ_HZ   433E6
#define LORA_SYNC 0xA5
#define LORA_SF   8         // Try 9..12 if your link is weak

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
const int     FRAG_MAX_TRIES = 8;                  // per-fragment attempts (exact: 1..3)
const unsigned long FRAG_ACK_TIMEOUT_MS = 1000;    // wait for ACKF
const unsigned long FRAG_SPACING_MS     = 15;      // small guard between tries

const unsigned long BASE_FINAL_ACK_TIMEOUT_MS = 1800; // final ACK wait baseline
const int     MSG_MAX_TRIES = 3;                   // whole-message attempts (exact: 1..3)

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
        if(d==myId && seq==expectSeq && idx==expectIdx) return true; // got it
      }
      else if(parseACK(pkt,s,d,seq,b,k)){ /* ignore here */ }
      else if(parseMSG(pkt,s,d,seq,t)){
        size_t pktBytes=pkt.length(), textBytes=t.length();
        rxDataPktsTotal++; rxBytesTotal+=textBytes;
        Serial.printf("[RX] #%ld from %s (single while waiting)\n", seq, s.c_str());
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
      String pkt; while(LoRa.available()) pkt+=(char)LoRa.read();
      String s,d,t; long seq=-1,idx=-1,tot=-1; uint64_t peerBytes=0, peerPkts=0;

      if(parseACK(pkt,s,d,seq,peerBytes,peerPkts)){
        if(d==myId && seq==expectSeq){
          unsigned long elapsed=millis()-sessionStartMs;
          bpsOut = elapsed? (peerBytes*8.0*1000.0/elapsed) : 0.0;
          pdrOut = (txDataPktsTotal>0)? (100.0*(double)peerPkts/(double)txDataPktsTotal) : 0.0;
          int rssi=LoRa.packetRssi(); float snr=LoRa.packetSnr();
          Serial.printf("[ACK OK] #%ld from %s | peerRxBytes=%llu | peerRxPkts=%llu | PDR=%.1f%% | %s | RSSI %d | SNR %.1f\n",
                        seq, s.c_str(), (unsigned long long)peerBytes, (unsigned long long)peerPkts,
                        pdrOut, speedToHuman(bpsOut).c_str(), rssi, snr);
          oled3("ACK OK ("+String(seq)+")", "PDR "+String(pdrOut,1)+"%  "+bytesToHuman(peerBytes), speedToHuman(bpsOut));
          return true;
        }
      } else {
        // process other traffic while we wait
        String s2,d2; long seq2=-1,idx2=-1,tot2=-1; uint64_t b2=0,k2=0;
        if(parseMSG(pkt,s2,d2,seq2,t)){
          size_t pktBytes=pkt.length(), textBytes=t.length();
          rxDataPktsTotal++; rxBytesTotal+=textBytes;
          Serial.printf("[RX] #%ld from %s (single while waiting ACK)\n", seq2, s2.c_str());
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
          // ignore here; fragment sender's loop is waiting for its own ACKF
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

      Serial.printf("  [TX] single %u bytes (%s, %s) | txt %uB\n",
                    (unsigned)pktBytes, bitsToHuman((uint64_t)pktBytes*8).c_str(), bytesToHuman(pktBytes).c_str(),
                    (unsigned)textBytes);
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
  // Optional robustness knobs (uncomment if needed):
  // LoRa.setCodingRate4(8);          // 4/8 (max redundancy)
  // LoRa.setSignalBandwidth(62.5E3); // narrower BW => better sensitivity

  sessionStartMs = millis();
  resetReasm();

  oled3("LoRa Chat Ready","ID: "+myId,"433 MHz, SF="+String(LORA_SF));
  Serial.println("=== LoRa Chat (Reliable + Exact Tries) ===");
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
      Serial.printf("[ACK stray] #%ld from %s | rxBytes=%llu rxPkts=%llu\n",
                    seq, s.c_str(), (unsigned long long)b, (unsigned long long)k);
      return;
    }
    if(parseACKF(pkt,s,d,seq,idx)){
      // stray ACKF (we aren't waiting here) → ignore
      return;
    }
    if(parseMSG(pkt,s,d,seq,t)){
      size_t pktBytes=pkt.length(), textBytes=t.length();
      rxDataPktsTotal++; rxBytesTotal+=textBytes;
      Serial.printf("[RX] #%ld from %s (single)\n", seq, s.c_str());
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

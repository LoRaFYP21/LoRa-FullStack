/*
  LoRa Serial Chat (Reliable Fragments) + Auto-ACK + PDR + Goodput
  ESP32 T-Display + SSD1306 + SX127x (Sandeep Mistry LoRa lib)

  Messages:
    - MSG, <src>,<dst>,<seq>,<text>                 (<= FRAG_CHUNK)
    - MSGF,<src>,<dst>,<seq>,<fragIdx>,<fragTot>,<chunk>
    - ACKF,<src>,<dst>,<seq>,<fragIdx>              (per-fragment ack)
    - ACK, <src>,<dst>,<seq>,<rxBytesTotal>,<rxPktsTotal>  (final ack)

  Sender logic:
    If text <= FRAG_CHUNK: send MSG, wait ACK.
    Else: for each fragment i:
            send MSGF(...,i,...), wait for ACKF(...,i) with timeout+retries.
          After last fragment is ACKF’d, wait for final ACK (with totals).

  Receiver logic:
    - On MSG: print text, update totals, send final ACK.
    - On MSGF: store chunk (ignore duplicates), send ACKF for that frag,
               when all received: reassemble, print full, send final ACK.

  Also prints per-packet sizes and keeps independent tx/rx counters.
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SPI.h>
#include <LoRa.h>

// ---------- Radio config ----------
#define FREQ_HZ   433E6
#define LORA_SYNC 0xA5
#define LORA_SF   8
// You can try SF9..12 to improve reliability on weak links.

// Wiring (your working map)
#define SCK   5
#define MISO  19
#define MOSI  27
#define SS    18
#define RST   14
#define DIO0  26

// Optional: force an easy ID if you want
// #define MY_NODE_ID "A"   // put "B" on the other board

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
uint64_t txDataPktsTotal=0, rxDataPktsTotal=0;     // counts MSG + MSGF fragments (attempts)
uint64_t txBytesTotal=0,   rxBytesTotal=0;         // app TEXT bytes totals

// ---------- Timing / ARQ ----------
uint32_t txSeq=0; bool waitingFinalAck=false; uint32_t waitingSeq=0;
unsigned long finalAckDeadline=0, sessionStartMs=0;

const size_t  FRAG_CHUNK = 200;     // text bytes per fragment
const int     FRAG_MAX_RETRIES = 3;
const unsigned long FRAG_ACK_TIMEOUT_MS = 900;  // wait for ACKF per fragment
const unsigned long BASE_FINAL_ACK_TIMEOUT_MS = 1500; // wait for final ACK (single or after fragments)
const unsigned long FRAG_SPACING_MS = 10;       // small guard between TX tries

// ---------- Utils ----------
static String bytesToHuman(uint64_t B){
  if(B>=1000000ULL) return String((double)B/1e6,3)+" MB";
  if(B>=1000ULL)    return String((double)B/1e3,3)+" kB";
  return String((uint64_t)B)+" B";
}
static String bitsToHuman(uint64_t b){
  if(b>=1000000ULL) return String((double)b/1e6,3)+" Mb";
  if(b>=1000ULL)    return String((double)b/1e3,3)+" kb";
  return String((uint64_t)b)+" b";
}
static String speedToHuman(double bps){
  if(bps>=1e6) return String(bps/1e6,3)+" Mb/s";
  if(bps>=1e3) return String(bps/1e3,3)+" kb/s";
  return String(bps,0)+" b/s";
}
static void macTo12Hex(String& out){
  uint64_t mac=ESP.getEfuseMac();
  char buf[13]; sprintf(buf,"%04X%08X",(uint16_t)(mac>>32),(uint32_t)mac);
  out=String(buf);
}
static void sanitizeText(String& s){
  for(uint16_t i=0;i<s.length();++i){ char c=s[i]; if(c==','||c=='\r'||c=='\n') s.setCharAt(i,' '); }
}
static void sendLoRa(const String& payload){
  LoRa.beginPacket(); LoRa.print(payload); LoRa.endPacket();
}
static long toLong(const String& s){
  long v=0; bool seen=false; for(uint16_t i=0;i<s.length();++i){ char c=s[i];
    if(c>='0'&&c<='9'){ v=v*10+(c-'0'); seen=true; } else if(seen) break; }
  return seen? v:-1;
}

// ---------- Parsers ----------
// MSG:  MSG,<src>,<dst>,<seq>,<text>
static bool parseMSG(const String& in, String& src,String& dst,long& seq,String& text){
  if(!in.startsWith("MSG,")) return false;
  int p1=in.indexOf(',',4); if(p1<0) return false;
  int p2=in.indexOf(',',p1+1); if(p2<0) return false;
  int p3=in.indexOf(',',p2+1); if(p3<0) return false;
  src=in.substring(4,p1); dst=in.substring(p1+1,p2);
  String seqStr=in.substring(p2+1,p3); text=in.substring(p3+1); seq=toLong(seqStr);
  return true;
}
// MSGF: MSGF,<src>,<dst>,<seq>,<fragIdx>,<fragTot>,<chunk>
static bool parseMSGF(const String& in, String& src,String& dst,long& seq,long& idx,long& tot,String& chunk){
  if(!in.startsWith("MSGF,")) return false;
  int p1=in.indexOf(',',5); if(p1<0) return false;
  int p2=in.indexOf(',',p1+1); if(p2<0) return false;
  int p3=in.indexOf(',',p2+1); if(p3<0) return false;
  int p4=in.indexOf(',',p3+1); if(p4<0) return false;
  int p5=in.indexOf(',',p4+1); if(p5<0) return false;
  src=in.substring(5,p1); dst=in.substring(p1+1,p2);
  String seqStr=in.substring(p2+1,p3); String idxStr=in.substring(p3+1,p4);
  String totStr=in.substring(p4+1,p5); chunk=in.substring(p5+1);
  seq=toLong(seqStr); idx=toLong(idxStr); tot=toLong(totStr);
  return true;
}
// ACKF: ACKF,<src>,<dst>,<seq>,<fragIdx>
static bool parseACKF(const String& in, String& src,String& dst,long& seq,long& idx){
  if(!in.startsWith("ACKF,")) return false;
  int p1=in.indexOf(',',5); if(p1<0) return false;
  int p2=in.indexOf(',',p1+1); if(p2<0) return false;
  int p3=in.indexOf(',',p2+1); if(p3<0) return false;
  src=in.substring(5,p1); dst=in.substring(p1+1,p2);
  String seqStr=in.substring(p2+1,p3); String idxStr=in.substring(p3+1);
  seq=toLong(seqStr); idx=toLong(idxStr);
  return true;
}
// ACK:  ACK,<src>,<dst>,<seq>,<rxBytesTotal>,<rxPktsTotal>
static bool parseACK(const String& in, String& src,String& dst,long& seq,uint64_t& rxTotBytes,uint64_t& rxTotPkts){
  if(!in.startsWith("ACK,")) return false;
  int p1=in.indexOf(',',4); if(p1<0) return false;
  int p2=in.indexOf(',',p1+1); if(p2<0) return false;
  int p3=in.indexOf(',',p2+1); if(p3<0) return false;
  int p4=in.indexOf(',',p3+1); if(p4<0) return false;
  src=in.substring(4,p1); dst=in.substring(p1+1,p2);
  String seqStr=in.substring(p2+1,p3); String bStr=in.substring(p3+1,p4); String kStr=in.substring(p4+1);
  seq=toLong(seqStr); rxTotBytes=(uint64_t)bStr.toDouble(); rxTotPkts=(uint64_t)kStr.toDouble();
  return true;
}

// ---------- RX reassembly (single in-flight per peer) ----------
String   reSrc=""; long reSeq=-1; long reTot=0; long reGot=0;
String*  reChunks=nullptr; bool* reHave=nullptr;
void resetReasm(){
  if(reChunks){ delete[] reChunks; reChunks=nullptr; }
  if(reHave){ delete[] reHave; reHave=nullptr; }
  reSrc=""; reSeq=-1; reTot=0; reGot=0;
}
void startReasm(const String& src, long seq, long tot){
  resetReasm();
  reSrc=src; reSeq=seq; reTot=tot; reGot=0;
  reChunks=new String[tot]; reHave=new bool[tot];
  for(long i=0;i<tot;i++) reHave[i]=false;
}
bool addFrag(long idx, const String& chunk){
  if(idx<0 || idx>=reTot) return false;
  if(!reHave[idx]){ reHave[idx]=true; reChunks[idx]=chunk; reGot++; return true; }
  return false; // duplicate
}
String joinReasm(){
  String out; out.reserve(reTot*FRAG_CHUNK);
  for(long i=0;i<reTot;i++) out += reChunks[i];
  return out;
}

// ---------- Wait for a specific ACKF (with timeout) ----------
bool waitForAckF(const String& expectDst, long expectSeq, long expectIdx, unsigned long timeoutMs){
  unsigned long deadline = millis()+timeoutMs;
  while((long)(millis()-deadline) < 0){
    int psz=LoRa.parsePacket();
    if(psz){
      String pkt; while(LoRa.available()) pkt += (char)LoRa.read();
      // Accept and ignore unrelated packets (but handle prints minimally)
      String s,d; long seq=-1,idx=-1; uint64_t b=0,k=0; String t; long i2=-1, tot2=-1;
      if(parseACKF(pkt,s,d,seq,idx)){
        if(d==myId && seq==expectSeq && idx==expectIdx){
          // got the one we need
          return true;
        }
        // some other ACKF; just log lightly
        // Serial.printf("[ACKF stray] %s->%s seq=%ld idx=%ld\n", s.c_str(), d.c_str(), seq, idx);
      } else if(parseACK(pkt,s,d,seq,b,k)){
        // final ACK that belongs to someone else or earlier — ignore
      } else if(parseMSG(pkt,s,d,seq,t)){
        // we received someone's MSG while waiting → process normal (show + final ACK)
        size_t pktBytes=pkt.length(), textBytes=t.length();
        rxDataPktsTotal++; rxBytesTotal+=textBytes;
        Serial.printf("[RX] #%ld from %s (single while waiting)\n", seq, s.c_str());
        Serial.printf("     Packet: %u bytes (%s, %s)\n",
                      (unsigned)pktBytes, bitsToHuman((uint64_t)pktBytes*8).c_str(), bytesToHuman(pktBytes).c_str());
        Serial.printf("     Text:   %u bytes (%s) | rxTotal=%llu | rxPkts=%llu\n",
                      (unsigned)textBytes, bytesToHuman(textBytes).c_str(),
                      (unsigned long long)rxBytesTotal, (unsigned long long)rxDataPktsTotal);
        serialPrintLnChunked(t);
        oled3("RX <- ("+String(seq)+")", t.substring(0,16), "txt "+String(textBytes)+"B");
        String ack="ACK,"+myId+","+s+","+String(seq)+","+String((unsigned long long)rxBytesTotal)+","+String((unsigned long long)rxDataPktsTotal);
        sendLoRa(ack);
        // continue waiting for our ACKF
      } else if(parseMSGF(pkt,s,d,seq,i2,tot2,t)){
        // fragment arrived while waiting → store + ACKF immediately
        if(s!=reSrc || seq!=reSeq) startReasm(s, seq, tot2);
        addFrag(i2, t);
        String ackf="ACKF,"+myId+","+s+","+String(seq)+","+String(i2);
        sendLoRa(ackf);
        // complete?
        if(reGot==reTot){
          String full=joinReasm();
          size_t textBytes=full.length();
          rxDataPktsTotal++;  // count the final logical message as well
          rxBytesTotal += textBytes;
          Serial.printf("[RX FULL] #%ld from %s | total text %uB\n", seq, s.c_str(), (unsigned)textBytes);
          serialPrintLnChunked(full);
          oled3("RX <- ("+String(seq)+")","full msg", bytesToHuman(textBytes));
          String ack="ACK,"+myId+","+s+","+String(seq)+","+String((unsigned long long)rxBytesTotal)+","+String((unsigned long long)rxDataPktsTotal);
          sendLoRa(ack);
          resetReasm();
        }
      } else {
        // unknown; ignore
      }
    }
    delay(1);
  }
  return false; // timeout
}

void setup(){
  Serial.begin(115200); Serial.setTimeout(10);
  Wire.begin();
  if(!display.begin(SSD1306_SWITCHCAPVCC,0x3C)){ Serial.println("SSD1306 fail"); for(;;); }

#ifdef MY_NODE_ID
  myId = String(MY_NODE_ID);
#else
  macTo12Hex(myId);   // full 48-bit MAC -> 12 hex
#endif

  SPI.begin(SCK,MISO,MOSI,SS); LoRa.setPins(SS,RST,DIO0);
  if(!LoRa.begin(FREQ_HZ)){ oled3("LoRa init FAILED","Check wiring/freq"); for(;;); }
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSyncWord(LORA_SYNC);
  LoRa.enableCrc();
  LoRa.setTxPower(17, PA_OUTPUT_PA_BOOST_PIN);

  sessionStartMs = millis();
  resetReasm();

  oled3("LoRa Chat Ready","ID: "+myId,"433 MHz, SF=8");
  Serial.println("=== LoRa Chat (Reliable Fragments) ===");
  Serial.println("115200, Newline. Type and Enter.");
  Serial.print("Node ID: "); Serial.println(myId);
}

void loop(){
  // -------- Serial -> LoRa (send) --------
  if(Serial.available()){
    String line=Serial.readStringUntil('\n'); line.trim();
    if(line.length()){
      sanitizeText(line);
      waitingSeq = txSeq;

      if(line.length() <= FRAG_CHUNK){
        // Single packet
        String payload="MSG,"+myId+","+dstAny+","+String(txSeq)+","+line;
        sendLoRa(payload);
        size_t pktBytes=payload.length(), textBytes=line.length();
        txDataPktsTotal++; txBytesTotal+=textBytes;
        Serial.printf("[TX] #%lu (single)\n",(unsigned long)txSeq);
        Serial.printf("     Packet: %u bytes (%s, %s)\n",(unsigned)pktBytes, bitsToHuman((uint64_t)pktBytes*8).c_str(), bytesToHuman(pktBytes).c_str());
        Serial.printf("     Text:   %u bytes (%s)\n",(unsigned)textBytes, bytesToHuman(textBytes).c_str());
        oled3("TX -> ("+String(txSeq)+")", line.substring(0,16), "txt "+String(textBytes)+"B");

        // wait for final ACK
        waitingFinalAck = true;
        finalAckDeadline = millis() + BASE_FINAL_ACK_TIMEOUT_MS;
      } else {
        // Fragmented with per-fragment ARQ
        size_t L=line.length();
        size_t total=(L+FRAG_CHUNK-1)/FRAG_CHUNK;
        Serial.printf("[TX] #%lu fragments: %u x %uB (total %uB)\n",(unsigned long)txSeq,(unsigned)total,(unsigned)FRAG_CHUNK,(unsigned)L);

        for(size_t i=0;i<total;i++){
          size_t off=i*FRAG_CHUNK;
          String chunk=line.substring(off, min(L, off+FRAG_CHUNK));
          String payload="MSGF,"+myId+","+dstAny+","+String(txSeq)+","+String(i)+","+String(total)+","+chunk;

          int tries=0; bool ok=false;
          while(tries<=FRAG_MAX_RETRIES && !ok){
            sendLoRa(payload);
            size_t pktBytes=payload.length();
            txDataPktsTotal++; txBytesTotal+=chunk.length();
            Serial.printf("  [TX FRAG %u/%u try %d] %u bytes (%s, %s) | txt %uB\n",
                          (unsigned)(i+1),(unsigned)total,tries+1,(unsigned)pktBytes,
                          bitsToHuman((uint64_t)pktBytes*8).c_str(), bytesToHuman(pktBytes).c_str(),
                          (unsigned)chunk.length());
            ok = waitForAckF(myId, (long)txSeq, (long)i, FRAG_ACK_TIMEOUT_MS);
            if(!ok){ tries++; if(tries<=FRAG_MAX_RETRIES) Serial.println("   -> no ACKF, retrying..."); }
            delay(FRAG_SPACING_MS);
          }
          if(!ok){
            Serial.println("[ABORT] fragment failed after retries");
            waitingFinalAck=false; // don’t wait final ack
            break;
          }
        }
        // After last frag ACKF, wait for final ACK (receiver sends after join)
        waitingFinalAck = true;
        // scale final ack timeout a bit with fragment count
        finalAckDeadline = millis() + BASE_FINAL_ACK_TIMEOUT_MS + total*500;
      }
      txSeq++;
    }
  }

  // -------- LoRa RX (normal) --------
  int psz=LoRa.parsePacket();
  if(psz){
    String pkt; while(LoRa.available()) pkt+=(char)LoRa.read();
    int rssi=LoRa.packetRssi(); float snr=LoRa.packetSnr();

    // Per-fragment ACK?
    String s,d; long seq=-1,idx=-1; uint64_t b=0,k=0; String t; long idx2=-1, tot2=-1;
    if(parseACKF(pkt,s,d,seq,idx)){
      // We don't need to print ACKFs here; the sender's waiting loop consumes them.
      return;
    }
    // Final ACK?
    if(parseACK(pkt,s,d,seq,b,k)){
      if(d==myId && waitingFinalAck && seq==(long)waitingSeq){
        waitingFinalAck=false;

        unsigned long elapsed=millis()-sessionStartMs;
        double bps = elapsed? (b*8.0*1000.0/elapsed) : 0.0;
        double pdr = (txDataPktsTotal>0)? (100.0*(double)k/(double)txDataPktsTotal) : 0.0;

        Serial.printf("[ACK OK] #%ld from %s | peerRxBytes=%llu | peerRxPkts=%llu | PDR=%.1f%% | %s | RSSI %d | SNR %.1f\n",
                      seq, s.c_str(), (unsigned long long)b, (unsigned long long)k,
                      pdr, speedToHuman(bps).c_str(), rssi, snr);
        oled3("ACK OK ("+String(seq)+")",
              "PDR "+String(pdr,1)+"%  "+bytesToHuman(b),
              speedToHuman(bps));
      }
      return;
    }
    // Single MSG?
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
      // final ACK
      String ack="ACK,"+myId+","+s+","+String(seq)+","+String((unsigned long long)rxBytesTotal)+","+String((unsigned long long)rxDataPktsTotal);
      sendLoRa(ack);
      resetReasm();
      return;
    }
    // Fragment MSGF?
    if(parseMSGF(pkt,s,d,seq,idx2,tot2,t)){
      if(s!=reSrc || seq!=reSeq) startReasm(s, seq, tot2);
      bool fresh = addFrag(idx2, t);
      // ACKF immediately (even if duplicate)
      String ackf="ACKF,"+myId+","+s+","+String(seq)+","+String(idx2);
      sendLoRa(ackf);

      size_t pktBytes=pkt.length(), textBytes=t.length();
      rxDataPktsTotal++; if(fresh) rxBytesTotal+=textBytes; // count text only once
      Serial.printf("[RX FRAG] #%ld from %s [%ld/%ld]%s\n", seq, s.c_str(), idx2+1, tot2, fresh?"":" (dup)");
      Serial.printf("          Packet: %u bytes (%s, %s) | chunk %uB | rxTotal=%llu | rxPkts=%llu\n",
                    (unsigned)pktBytes, bitsToHuman((uint64_t)pktBytes*8).c_str(), bytesToHuman(pktBytes).c_str(),
                    (unsigned)textBytes, (unsigned long long)rxBytesTotal, (unsigned long long)rxDataPktsTotal);
      oled3("RX frag "+String(idx2+1)+"/"+String(tot2),
            t.substring(0,16), "acc "+bytesToHuman(rxBytesTotal));

      // Completed?
      bool all=true; for(long i=0;i<reTot;i++) if(!reHave[i]){ all=false; break; }
      if(all){
        String full=joinReasm(); size_t textBytes2=full.length();
        Serial.printf("[RX FULL] #%ld from %s | total text %uB\n", seq, s.c_str(), (unsigned)textBytes2);
        serialPrintLnChunked(full);
        oled3("RX <- ("+String(seq)+")","full msg", bytesToHuman(textBytes2));
        // final ACK with totals
        String ack="ACK,"+myId+","+s+","+String(seq)+","+
                   String((unsigned long long)rxBytesTotal)+","+String((unsigned long long)rxDataPktsTotal);
        sendLoRa(ack);
        resetReasm();
      }
      return;
    }
    // Unknown
    // Serial.printf("[RX RAW] %s\n", pkt.c_str());
  }

  // Final ACK timeout (after sending)
  if(waitingFinalAck && (long)(millis()-finalAckDeadline) >= 0){
    waitingFinalAck=false;
    Serial.printf("[ACK TIMEOUT] seq #%lu (no final ack)\n", (unsigned long)waitingSeq);
    oled3("ACK TIMEOUT", "seq "+String(waitingSeq), "");
  }

  delay(1);
}

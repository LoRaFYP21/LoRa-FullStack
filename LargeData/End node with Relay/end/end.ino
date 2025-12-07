/*
  LoRa Serial Chat (One-Way: Transmit and Receive with Fragmentation)
  ESP32 T-Display + SSD1306 + SX127x (Sandeep Mistry LoRa lib)
  === AS923 variant (923 MHz) with header-aware fragment sizing (<= 255 B) ===

  Features:
    - Fragmentation for large messages
    - No ACKs - just send and receive
    - RSSI-based distance estimate
    - Hop header: H<hops>:<payload>
      * Endpoints originate with hop=0
      * Relay increments hop for MSG/MSGF: 0 -> 1 -> 2 ...
      * Logs show DIRECT (hop=0) vs VIA RELAY (hop>=1)
    - Self-echo protection (src == myId)
*/

// ===================== NODE CONFIG =============================

// For Node A:
//   #define MY_NODE_ID   "A"
//   #define PEER_NODE_ID "C"

// For Node C:
//   #define MY_NODE_ID   "C"
//   #define PEER_NODE_ID "A"

#define MY_NODE_ID   "C"   // <<< SET THIS FOR EACH ENDPOINT
#define PEER_NODE_ID "A"   // <<< SET THIS FOR EACH ENDPOINT

// ===============================================================

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SPI.h>
#include <LoRa.h>
#include <math.h>

// ---------- Radio config (AS923) ----------
#define FREQ_HZ   923E6
#define LORA_SYNC 0xA5
#define LORA_SF   8          // raise if link is weak (9..12)
const size_t LORA_MAX_PAYLOAD = 255;  // SX127x FIFO/payload byte limit

// Reserve some bytes for hop header "H<hop>:"
const size_t HOP_HEADER_MAX = 6;                   // worst case: "H123:"
const size_t LORA_MAX_PAYLOAD_EFFECTIVE = LORA_MAX_PAYLOAD - HOP_HEADER_MAX;

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
static void serialPrintLnChunked(const String& s, size_t ch=128){
  for(size_t i=0;i<s.length();i+=ch){
    size_t n=min(ch, s.length()-i);
    Serial.write((const uint8_t*)s.c_str()+i, n);
  }
  Serial.write('\n');
}

// ---------- Distance estimation (RSSI-based) ----------
const float RSSI_REF_1M   = -45.0f;   // avg RSSI at ~1 m (measure for your hardware)
const float PATH_LOSS_N   = 2.7f;     // ~2.0 LOS outdoor, 2.7–3.5 urban, 3–5 indoor

float rssiEma = NAN;
const float RSSI_ALPHA = 0.20f;       // 0..1 (higher = less smoothing)

float estimateDistanceMetersFromRSSI(int rssi) {
  float r = (float)rssi;
  if (isnan(rssiEma)) rssiEma = r;
  rssiEma = RSSI_ALPHA * r + (1.0f - RSSI_ALPHA) * rssiEma;
  float exponent = (RSSI_REF_1M - rssiEma) / (10.0f * PATH_LOSS_N);
  float d = powf(10.0f, exponent);
  return d;
}

// ---------- IDs & counters ----------
String myId="????????????", dstAny="FF";           // peer dst
uint64_t txDataPktsTotal=0, rxDataPktsTotal=0;     // counts MSG + unique MSGF frags
uint64_t txBytesTotal=0,   rxBytesTotal=0;         // app TEXT bytes totals

// ---------- Timing knobs ----------
size_t  FRAG_CHUNK = 230;                          // desired text bytes/frag (auto-shrinks)
const unsigned long FRAG_SPACING_MS = 500;          // delay between fragment transmissions

uint32_t txSeq=0;
unsigned long sessionStartMs=0;

// ---------- Helpers ----------
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
static void sanitizeText(String& s){
  for(uint16_t i=0;i<s.length();++i){
    char c=s[i];
    if(c==','||c=='\r'||c=='\n') s.setCharAt(i,' ');
  }
}
static long toLong(const String& s){
  long v=0; bool seen=false;
  for(uint16_t i=0;i<s.length();++i){
    char c=s[i];
    if(c>='0'&&c<='9'){ v=v*10+(c-'0'); seen=true; }
    else if(seen) break;
  }
  return seen? v:-1;
}
static int digitsU(unsigned long v){
  int d=1;
  while(v>=10){ v/=10; d++; }
  return d;
}

// ---------- Hop header helpers ----------
// Format on air: "H<hops>:<payload>"
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

static void sendLoRaWithHop(const String& payload, int hop){
  if(hop < 0) hop = 0;
  String wrap = "H" + String(hop) + ":" + payload;
  LoRa.beginPacket();
  LoRa.print(wrap);
  LoRa.endPacket();
}

// Endpoints always originate with hop=0
static void sendLoRa(const String& payload){
  sendLoRaWithHop(payload, 0);
}

// ---------- Parsers ----------
static bool parseMSG (const String& in, String& src,String& dst,long& seq,String& text){
  if(!in.startsWith("MSG,")) return false;
  int p1=in.indexOf(',',4); if(p1<0) return false;
  int p2=in.indexOf(',',p1+1); if(p2<0) return false;
  int p3=in.indexOf(',',p2+1); if(p3<0) return false;
  src=in.substring(4,p1);
  dst=in.substring(p1+1,p2);
  String seqStr=in.substring(p2+1,p3);
  text=in.substring(p3+1);
  seq=toLong(seqStr);
  return true;
}
static bool parseMSGF(const String& in, String& src,String& dst,long& seq,long& idx,long& tot,String& chunk){
  if(!in.startsWith("MSGF,")) return false;
  int p1=in.indexOf(',',5); if(p1<0) return false;
  int p2=in.indexOf(',',p1+1); if(p2<0) return false;
  int p3=in.indexOf(',',p2+1); if(p3<0) return false;
  int p4=in.indexOf(',',p3+1); if(p4<0) return false;
  int p5=in.indexOf(',',p4+1); if(p5<0) return false;
  src=in.substring(5,p1);
  dst=in.substring(p1+1,p2);
  String seqStr=in.substring(p2+1,p3);
  String idxStr=in.substring(p3+1,p4);
  String totStr=in.substring(p4+1,p5);
  chunk=in.substring(p5+1);
  seq=toLong(seqStr);
  idx=toLong(idxStr);
  tot=toLong(totStr);
  return true;
}

// ---------- RX reassembly (one in-flight per peer) ----------
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
  reChunks=new String[tot];
  reHave=new bool[tot];
  for(long i=0;i<tot;i++) reHave[i]=false;
}
bool addFrag(long idx, const String& chunk){
  if(idx<0||idx>=reTot) return false;
  if(!reHave[idx]){
    reHave[idx]=true;
    reChunks[idx]=chunk;
    reGot++;
    return true;
  }
  return false;
}
String joinReasm(){
  String out;
  out.reserve(reTot*FRAG_CHUNK);
  for(long i=0;i<reTot;i++) out += reChunks[i];
  return out;
}

// ---------- Send one message (no ACK, just transmit) ----------
void sendMessage(const String& lineIn){
  String line=lineIn; sanitizeText(line);
  const size_t L=line.length();
  const bool single = (L <= FRAG_CHUNK);
  const size_t total = single ? 1 : (L + FRAG_CHUNK - 1)/FRAG_CHUNK;

  uint32_t seq = txSeq; txSeq++;

  Serial.printf("[TX START] seq #%lu | total length: %u bytes | fragments: %u\n", 
                (unsigned long)seq, (unsigned)L, (unsigned)total);

  if(single){
    // Ensure single MSG stays <= LORA_MAX_PAYLOAD_EFFECTIVE
    size_t hdrLen = 4 + myId.length() + 1 + dstAny.length() + 1 + digitsU(seq) + 1;
    size_t maxText = (hdrLen < LORA_MAX_PAYLOAD_EFFECTIVE) ? (LORA_MAX_PAYLOAD_EFFECTIVE - hdrLen) : 0;
    String text = (L <= maxText) ? line : line.substring(0, maxText);

    String payload="MSG,"+myId+","+dstAny+","+String(seq)+","+text;
    sendLoRa(payload);

    size_t pktBytes=payload.length(), textBytes=text.length();
    txDataPktsTotal++; txBytesTotal+=textBytes;

    Serial.printf("  [TX] single %u bytes (%s, %s) | txt %uB\n",
                  (unsigned)pktBytes,
                  bitsToHuman((uint64_t)pktBytes*8).c_str(),
                  bytesToHuman(pktBytes).c_str(),
                  (unsigned)textBytes);
    oled3("TX -> ("+String(seq)+")", text.substring(0,16), "txt "+String(textBytes)+"B");
  } else {
    // Send all fragments
    for(size_t i=0;i<total;i++){
      size_t off=i*FRAG_CHUNK;
      String chunk=line.substring(off, min(L, off+FRAG_CHUNK));

      size_t hdrLen = 5                           // "MSGF,"
                      + myId.length() + 1        // src + ','
                      + dstAny.length() + 1      // dst + ','
                      + digitsU(seq) + 1         // seq + ','
                      + digitsU(i) + 1           // idx + ','
                      + digitsU(total) + 1;      // tot + ','
      size_t maxChunk = (hdrLen < LORA_MAX_PAYLOAD_EFFECTIVE)
                         ? (LORA_MAX_PAYLOAD_EFFECTIVE - hdrLen) : 0;
      if(chunk.length() > maxChunk) chunk.remove(maxChunk); // auto-shrink

      String payload="MSGF,"+myId+","+dstAny+","+String(seq)+","+String(i)+","+String(total)+","+chunk;

      sendLoRa(payload);
      size_t pktBytes=payload.length();
      txDataPktsTotal++; txBytesTotal+=chunk.length();

      Serial.printf("  [TX FRAG %u/%u] %u bytes (%s, %s) | txt %uB\n",
                    (unsigned)(i+1), (unsigned)total, (unsigned)pktBytes,
                    bitsToHuman((uint64_t)pktBytes*8).c_str(),
                    bytesToHuman(pktBytes).c_str(),
                    (unsigned)chunk.length());

      oled3("TX -> ("+String(seq)+")", 
            "Frag "+String(i+1)+"/"+String(total),
            bytesToHuman(chunk.length()));

      // Small delay between fragments to avoid overwhelming receiver
      if(i < total-1) delay(FRAG_SPACING_MS);
    }
  }

  Serial.printf("[TX COMPLETE] seq #%lu | txTotal=%llu bytes | txPkts=%llu\n",
                (unsigned long)seq,
                (unsigned long long)txBytesTotal,
                (unsigned long long)txDataPktsTotal);
}

// ---------- Setup / Loop ----------
void setup(){
  Serial.begin(115200); Serial.setTimeout(10);
  Wire.begin();
  if(!display.begin(SSD1306_SWITCHCAPVCC,0x3C)){
    Serial.println("SSD1306 fail"); for(;;);
  }

  myId  = String(MY_NODE_ID);
  dstAny = String(PEER_NODE_ID);

  SPI.begin(SCK,MISO,MOSI,SS);
  LoRa.setPins(SS,RST,DIO0);
  if(!LoRa.begin(FREQ_HZ)){
    oled3("LoRa init FAILED","Check wiring/freq");
    for(;;);
  }
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSyncWord(LORA_SYNC);
  LoRa.enableCrc();
  LoRa.setTxPower(17, PA_OUTPUT_PA_BOOST_PIN);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);

  sessionStartMs = millis();
  resetReasm();

  oled3("LoRa Chat Ready",
        "ID:"+myId+" -> "+dstAny,
        "923 MHz, SF="+String(LORA_SF));
  Serial.println("=== LoRa Chat (One-Way + Distance + Hops) — AS923 (923 MHz) ===");
  Serial.println("115200, Newline. Type and Enter.");
  Serial.print("Node ID: "); Serial.println(myId);
  Serial.print("Peer ID: "); Serial.println(dstAny);
}

void loop(){
  // 1) Send when user types
  if(Serial.available()){
    String line=Serial.readStringUntil('\n'); line.trim();
    if(line.length()) sendMessage(line);
  }

  // 2) Otherwise receive and serve peers
  int psz=LoRa.parsePacket();
  if(psz){
    String rawPkt; while(LoRa.available()) rawPkt+=(char)LoRa.read();
    String pkt; int hopCount=0;
    stripHopHeader(rawPkt, pkt, hopCount);

    String s,d,t; long seq=-1,idx=-1,tot=-1;

    if(parseMSG(pkt,s,d,seq,t)){
      if(d!=myId && d!="FF") return;

      // ignore self-echo
      if(s == myId){
        Serial.printf("[SELF-ECHO MSG] seq=%ld hops=%d (loop RX) -> ignored\n",
                      seq, hopCount);
        return;
      }

      // Log human-readable info
      int rssi = LoRa.packetRssi(); float snr = LoRa.packetSnr();
      float d_m = estimateDistanceMetersFromRSSI(rssi);
      size_t pktBytes=pkt.length(), textBytes=t.length();
      rxDataPktsTotal++; rxBytesTotal+=textBytes;
      Serial.printf("[RX] #%ld from %s (single, hops=%d) | %uB | RSSI %d | SNR %.1f\n",
                    seq, s.c_str(), hopCount, (unsigned)textBytes, rssi, snr);
      // Also emit a machine-parseable line for the host to reconstruct
      // Format: RECV_MSG,<src>,<seq>,<text>  (commas in text are sanitized earlier)
      Serial.print("RECV_MSG,"); Serial.print(s); Serial.print(',');
      Serial.print(seq); Serial.print(','); Serial.println(t);
      return;
    }
    if(parseMSGF(pkt,s,d,seq,idx,tot,t)){
      if(d!=myId && d!="FF") return;

      // ignore self-echo
      if(s == myId){
        Serial.printf("[SELF-ECHO MSGF] seq=%ld idx=%ld hops=%d (loop RX) -> ignored\n",
                      seq, idx, hopCount);
        return;
      }

      int rssi = LoRa.packetRssi(); float snr = LoRa.packetSnr();
      float d_m = estimateDistanceMetersFromRSSI(rssi);

      // Log human-readable info
      Serial.printf("[RX FRAG %ld/%ld] seq=%ld from %s | RSSI %d | SNR %.1f | hops=%d\n",
            idx+1, tot, seq, s.c_str(), rssi, snr, hopCount);

      // Emit machine-parseable fragment line for host reconstruction
      // Format: RECV_FRAG,<src>,<seq>,<idx>,<tot>,<payload>
      Serial.print("RECV_FRAG,"); Serial.print(s); Serial.print(',');
      Serial.print(seq); Serial.print(','); Serial.print(idx); Serial.print(',');
      Serial.print(tot); Serial.print(','); Serial.println(t);

      // Also perform on-device reassembly so Arduino Serial Monitor shows the
      // reconstructed message automatically (useful when you don't run a host
      // script). We still emit RECV_FRAG for external parsers.
      if(s!=reSrc || seq!=reSeq) startReasm(s, seq, tot);
      bool fresh = addFrag(idx, t);
      if(fresh){ rxDataPktsTotal++; rxBytesTotal += t.length(); }

      bool all=true; for(long i=0;i<reTot;i++) if(!reHave[i]){ all=false; break; }
      if(all){
        String full = joinReasm();
        Serial.printf("\n[RECONSTRUCTED ON-DEVICE] seq #%ld from %s | total text %uB\n",
              seq, s.c_str(), (unsigned)full.length());
        Serial.println("Message Content:");
        serialPrintLnChunked(full);
        Serial.println();
        oled3("RX COMPLETE ("+String(seq)+")",
          (hopCount==0?"D ":"R ")+String(full.length())+"B",
          "d~"+String(d_m,1)+"m | RSSI "+String(rssi));
        resetReasm();
      }
      return;
    }
    // else: unknown payload, ignore
  }


  delay(1);
}

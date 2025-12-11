/*
  LoRa Serial Chat (Reliable Fragments + ARQ Modes) + PDR + Goodput + Distance
  === PC-Reassembly Version: MCU only forwards fragments/messages to Serial ===

  RX side:
    - Does NOT reassemble large messages in RAM.
    - For MSGF: prints "FRAG,src,seq,idx,tot,rssi,d_m,chunk" to Serial.
    - For MSG:  prints "MSG,src,seq,rssi,d_m,text" to Serial.
    - Still sends ACK / ACKF to keep the ARQ logic working.

  TX side:
    - Fragmentation is done at MCU for long lines from PC.
    - ARQ scheme for fragments is selectable:
        * Stop-and-Wait
        * Go-Back-N
        * Selective Repeat
    - For small single-packet messages: classic stop-and-wait with ACK.

  PC side:
    - TX PC sends lines like:
        FILECHUNK:<fname>:<idx>:<tot>:<base64_chunk>
      as ordinary text lines.
    - MCU just sees them as lines and sends them reliably over LoRa.
    - RX PC reassembles both LoRa fragments and FILECHUNKs to reconstruct files.
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SPI.h>
#include <LoRa.h>

// ---------- ARQ mode selection ----------
enum ArqMode
{
    ARQ_STOP_AND_WAIT = 0,
    ARQ_GO_BACK_N = 1,
    ARQ_SELECTIVE_REPEAT = 2
};

// Choose which ARQ scheme to use for *fragmented* messages:
ArqMode gArqMode = ARQ_STOP_AND_WAIT; // change here: S&W / GBN / SR

// Window size for Go-Back-N / Selective Repeat
const size_t ARQ_WINDOW_SIZE = 20; // small window (RELIABLE)

// Maximum number of fragments we support per message
const size_t MAX_FRAGMENTS = 512;

// ---------- Radio config (AS923) ----------
#define FREQ_HZ 923E6
#define LORA_SYNC 0xA5
#define LORA_SF 7                    // raise if link is weak (9..12)
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
size_t FRAG_CHUNK = 220;
const int FRAG_MAX_TRIES = 3;
const unsigned long FRAG_ACK_TIMEOUT_MS = 5000; // 5 s (enough for LoRa hop)
const unsigned long FRAG_SPACING_MS = 20;
const unsigned long LISTEN_AFTER_TX_MS = 40; // 0.8 s of RX-only per TX

const int RX_ACK_DELAY_MS = 20;

uint32_t txSeq = 0;
unsigned long sessionStartMs = 0;

// ---------- ARQ state arrays ----------
bool fragAcked[MAX_FRAGMENTS];
unsigned long fragLastTx[MAX_FRAGMENTS];
uint8_t fragRetries[MAX_FRAGMENTS];

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

// ---------- Dummy reassembly reset ----------
void resetReasm() {}

// ---------- Blocking waits for single-packet messages ----------
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
            {
                // ignore
            }
            else if (parseMSG(pkt, s, d, seq, t))
            {
                delay(RX_ACK_DELAY_MS);
                String ack = "ACK," + myId + "," + s + "," + String(seq) + "," +
                             String((unsigned long long)(rxBytesTotal + t.length())) + "," +
                             String((unsigned long long)(rxDataPktsTotal + 1));
                sendLoRa(ack);
                Serial.println("[RX Interrupted] MSG from " + s);
            }
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
        }
        delay(1);
    }
    return false;
}

// ---------- RX while transmitting (for GBN/SR) ----------
static void processIncomingWhileTx(uint32_t expectSeq, size_t total)
{
    int psz = LoRa.parsePacket();
    if (!psz)
        return;

    String pkt;
    while (LoRa.available())
        pkt += (char)LoRa.read();

    String s, d, t;
    long seq = -1, idx = -1, tot = -1;
    uint64_t b = 0, k = 0;

    if (parseACKF(pkt, s, d, seq, idx))
    {
        if (d == myId && seq == (long)expectSeq)
        {
            if (idx >= 0 && (size_t)idx < total)
            {
                fragAcked[idx] = true;
                Serial.printf("  [RX ACKF] seq=%ld idx=%ld from=%s\n", seq, idx, s.c_str());
            }
        }
    }
    else if (parseACK(pkt, s, d, seq, b, k))
    {
        // ignore here
    }
    else if (parseMSG(pkt, s, d, seq, t))
    {
        delay(RX_ACK_DELAY_MS);
        String ack = "ACK," + myId + "," + s + "," + String(seq) + "," +
                     String((unsigned long long)(rxBytesTotal + t.length())) + "," +
                     String((unsigned long long)(rxDataPktsTotal + 1));
        sendLoRa(ack);
        Serial.println("[RX Interrupt] MSG from " + s);
    }
}

static void listenForAckWindow(uint32_t expectSeq, size_t total, unsigned long durationMs)
{
    unsigned long until = millis() + durationMs;
    while ((long)(millis() - until) < 0)
    {
        processIncomingWhileTx(expectSeq, total);
        delay(2);
    }
}

// ---------- Fragment send helpers ----------

// Stop-and-Wait per fragment
bool sendFragmentsStopAndWait(const String &line, size_t L, size_t total, uint32_t seq)
{
    for (size_t i = 0; i < total; i++)
    {
        size_t off = i * FRAG_CHUNK;
        String chunk = line.substring(off, min(L, off + FRAG_CHUNK));

        String payload = "MSGF," + myId + "," + dstAny + "," + String(seq) + "," +
                         String(i) + "," + String(total) + "," + chunk;

        bool fragOk = false;
        for (int ftry = 1; ftry <= FRAG_MAX_TRIES; ++ftry)
        {
            sendLoRa(payload);
            txDataPktsTotal++;
            txBytesTotal += chunk.length();

            Serial.printf("  [S&W FRAG %u/%u] Try %d/%d\n",
                          (unsigned)(i + 1), (unsigned)total, ftry, FRAG_MAX_TRIES);

            if (waitForAckF((long)seq, (long)i, FRAG_ACK_TIMEOUT_MS))
            {
                fragOk = true;
                break;
            }
            delay(FRAG_SPACING_MS);
        }

        if (!fragOk)
        {
            Serial.println("[ABORT] S&W: Frag failed after max retries. Dropping message.");
            oled3("TX FAILED", "Dropped Frag " + String(i + 1), "");
            return false;
        }
    }

    Serial.printf("[TX DONE] #%lu mode=S&W all fragments ACKed.\n", (unsigned long)seq);
    return true;
}

// Go-Back-N
bool sendFragmentsGoBackN(const String &line, size_t L, size_t total, uint32_t seq)
{
    if (total > MAX_FRAGMENTS)
    {
        Serial.println("[ERROR] GBN: total fragments > MAX_FRAGMENTS");
        return false;
    }

    for (size_t i = 0; i < total; ++i)
    {
        fragAcked[i] = false;
        fragLastTx[i] = 0;
        fragRetries[i] = 0;
    }

    size_t base = 0;
    size_t nextToSend = 0;

    Serial.printf("[GBN] seq=%lu totalFrags=%u window=%u\n",
                  (unsigned long)seq, (unsigned)total, (unsigned)ARQ_WINDOW_SIZE);

    while (base < total)
    {
        unsigned long now = millis();

        // Send new fragments up to window
        while (nextToSend < total && nextToSend < base + ARQ_WINDOW_SIZE)
        {
            size_t off = nextToSend * FRAG_CHUNK;
            String chunk = line.substring(off, min(L, off + FRAG_CHUNK));

            String payload = "MSGF," + myId + "," + dstAny + "," + String(seq) + "," +
                             String(nextToSend) + "," + String(total) + "," + chunk;

            sendLoRa(payload);
            txDataPktsTotal++;
            txBytesTotal += chunk.length();

            fragLastTx[nextToSend] = now;
            fragRetries[nextToSend]++;

            Serial.printf("  [GBN TX FRAG %u/%u] (retry %u)\n",
                          (unsigned)(nextToSend + 1), (unsigned)total,
                          (unsigned)fragRetries[nextToSend]);

            listenForAckWindow(seq, total, LISTEN_AFTER_TX_MS);

            nextToSend++;
        }

        // Wait for ACKFs / timeout for base
        while (true)
        {
            processIncomingWhileTx(seq, total);

            while (base < total && fragAcked[base])
            {
                base++;
            }

            if (base >= total)
            {
                Serial.printf("[TX DONE] #%lu mode=GBN all fragments ACKed.\n", (unsigned long)seq);
                return true;
            }

            if (nextToSend < total && nextToSend < base + ARQ_WINDOW_SIZE)
            {
                break;
            }

            unsigned long now2 = millis();

            if (!fragAcked[base] &&
                (long)(now2 - fragLastTx[base]) > (long)FRAG_ACK_TIMEOUT_MS)
            {
                Serial.printf("  [GBN TIMEOUT] base=%u -> retransmit [%u..%u)\n",
                              (unsigned)base, (unsigned)base, (unsigned)nextToSend);

                if (fragRetries[base] >= FRAG_MAX_TRIES)
                {
                    Serial.println("[ABORT] GBN: base fragment exceeded max retries, dropping message.");
                    oled3("TX FAILED", "GBN base exceeded", "");
                    return false;
                }

                for (size_t j = base; j < nextToSend; ++j)
                {
                    size_t off = j * FRAG_CHUNK;
                    String chunk = line.substring(off, min(L, off + FRAG_CHUNK));
                    String payload = "MSGF," + myId + "," + dstAny + "," + String(seq) + "," +
                                     String(j) + "," + String(total) + "," + chunk;

                    sendLoRa(payload);
                    txDataPktsTotal++;
                    txBytesTotal += chunk.length();

                    fragLastTx[j] = millis();
                    fragRetries[j]++;

                    Serial.printf("    [GBN RE-TX FRAG %u/%u] (retry %u)\n",
                                  (unsigned)(j + 1), (unsigned)total,
                                  (unsigned)fragRetries[j]);

                    listenForAckWindow(seq, total, LISTEN_AFTER_TX_MS);

                    if (fragRetries[j] > FRAG_MAX_TRIES)
                    {
                        Serial.println("[ABORT] GBN: fragment exceeded max retries.");
                        oled3("TX FAILED", "GBN retries", "");
                        return false;
                    }
                }
            }

            delay(2);
        }
    }

    Serial.printf("[TX DONE] #%lu mode=GBN all fragments ACKed.\n", (unsigned long)seq);
    return true;
}

// Selective Repeat
bool sendFragmentsSelectiveRepeat(const String &line, size_t L, size_t total, uint32_t seq)
{
    if (total > MAX_FRAGMENTS)
    {
        Serial.println("[ERROR] SR: total fragments > MAX_FRAGMENTS");
        return false;
    }

    for (size_t i = 0; i < total; ++i)
    {
        fragAcked[i] = false;
        fragLastTx[i] = 0;
        fragRetries[i] = 0;
    }

    size_t base = 0;
    size_t nextToSend = 0;

    Serial.printf("[SR] seq=%lu totalFrags=%u window=%u\n",
                  (unsigned long)seq, (unsigned)total, (unsigned)ARQ_WINDOW_SIZE);

    while (base < total)
    {
        unsigned long now = millis();

        // Send new fragments within window
        while (nextToSend < total && nextToSend < base + ARQ_WINDOW_SIZE)
        {
            size_t off = nextToSend * FRAG_CHUNK;
            String chunk = line.substring(off, min(L, off + FRAG_CHUNK));

            String payload = "MSGF," + myId + "," + dstAny + "," + String(seq) + "," +
                             String(nextToSend) + "," + String(total) + "," + chunk;

            sendLoRa(payload);
            txDataPktsTotal++;
            txBytesTotal += chunk.length();

            fragLastTx[nextToSend] = now;
            fragRetries[nextToSend]++;

            Serial.printf("  [SR TX FRAG %u/%u] (retry %u)\n",
                          (unsigned)(nextToSend + 1), (unsigned)total,
                          (unsigned)fragRetries[nextToSend]);

            listenForAckWindow(seq, total, LISTEN_AFTER_TX_MS);

            nextToSend++;
        }

        unsigned long loopStart = millis();

        while (true)
        {
            processIncomingWhileTx(seq, total);

            while (base < total && fragAcked[base])
            {
                base++;
            }

            if (base >= total)
            {
                Serial.printf("[TX DONE] #%lu mode=SR all fragments ACKed.\n", (unsigned long)seq);
                return true;
            }

            if (nextToSend < total && nextToSend < base + ARQ_WINDOW_SIZE)
            {
                break;
            }

            unsigned long now2 = millis();

            for (size_t j = base; j < nextToSend; ++j)
            {
                if (fragAcked[j])
                    continue;

                if ((long)(now2 - fragLastTx[j]) > (long)FRAG_ACK_TIMEOUT_MS)
                {
                    if (fragRetries[j] >= FRAG_MAX_TRIES)
                    {
                        Serial.println("[ABORT] SR: fragment exceeded max retries.");
                        oled3("TX FAILED", "SR retries", "");
                        return false;
                    }

                    size_t off = j * FRAG_CHUNK;
                    String chunk = line.substring(off, min(L, off + FRAG_CHUNK));
                    String payload = "MSGF," + myId + "," + dstAny + "," + String(seq) + "," +
                                     String(j) + "," + String(total) + "," + chunk;

                    sendLoRa(payload);
                    txDataPktsTotal++;
                    txBytesTotal += chunk.length();

                    fragLastTx[j] = now2;
                    fragRetries[j]++;

                    Serial.printf("    [SR RE-TX FRAG %u/%u] (retry %u)\n",
                                  (unsigned)(j + 1), (unsigned)total,
                                  (unsigned)fragRetries[j]);

                    listenForAckWindow(seq, total, LISTEN_AFTER_TX_MS);
                }
            }

            if ((long)(now2 - loopStart) > (long)(FRAG_ACK_TIMEOUT_MS / 2))
            {
                break;
            }

            delay(2);
        }
    }

    Serial.printf("[TX DONE] #%lu mode=SR all fragments ACKed.\n", (unsigned long)seq);
    return true;
}

// ---------- Send one message reliably ----------
bool sendMessageReliable(const String &lineIn)
{
    String line = lineIn;
    sanitizeText(line);
    const size_t L = line.length();
    const bool single = (L <= FRAG_CHUNK);
    const size_t total = single ? 1 : (L + FRAG_CHUNK - 1) / FRAG_CHUNK;

    uint32_t seq = txSeq;
    txSeq++;

    Serial.printf("[CHUNK START] seq=%lu L=%u totalFrags=%u\n",
                  (unsigned long)seq, (unsigned)L, (unsigned)total);

    Serial.printf("[TX START] #%lu len=%u chunks=%u\n",
                  (unsigned long)seq, (unsigned)L, (unsigned)total);

    if (single)
    {
        // Single-packet logic
        size_t hdrLen = 4 + myId.length() + 1 + dstAny.length() + 1 + digitsU(seq) + 1;
        size_t maxText = (hdrLen < LORA_MAX_PAYLOAD) ? (LORA_MAX_PAYLOAD - hdrLen) : 0;
        String text = (L <= maxText) ? line : line.substring(0, maxText);
        String payload = "MSG," + myId + "," + dstAny + "," + String(seq) + "," + text;

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

        Serial.printf("[TX DONE] #%lu mode=SINGLE\n", (unsigned long)seq);
    }
    else
    {
        bool ok = false;
        switch (gArqMode)
        {
        case ARQ_STOP_AND_WAIT:
            Serial.println("[ARQ] Mode = Stop-and-Wait");
            ok = sendFragmentsStopAndWait(line, L, total, seq);
            break;
        case ARQ_GO_BACK_N:
            Serial.println("[ARQ] Mode = Go-Back-N");
            ok = sendFragmentsGoBackN(line, L, total, seq);
            break;
        case ARQ_SELECTIVE_REPEAT:
            Serial.println("[ARQ] Mode = Selective Repeat");
            ok = sendFragmentsSelectiveRepeat(line, L, total, seq);
            break;
        }

        if (!ok)
            return false;
    }

    return true;
}

// ---------- Setup / Loop ----------
void setup()
{
    Serial.begin(115200);
    Serial.setTimeout(5000); // Allow large FILECHUNK lines
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
    LoRa.setSignalBandwidth(5000E3);

    sessionStartMs = millis();
    resetReasm();

    String modeStr = "S&W";
    if (gArqMode == ARQ_GO_BACK_N)
        modeStr = "GBN";
    else if (gArqMode == ARQ_SELECTIVE_REPEAT)
        modeStr = "SR";

    oled3("LoRa Chat Ready", "ID: " + myId, "ARQ: " + modeStr);
    Serial.println("=== LoRa Chat (PC Reassembly Mode) ===");
    Serial.println("Type a line and press Enter to send.");
    Serial.print("ARQ Mode: ");
    Serial.println(modeStr);
}

void loop()
{
    // 1) Send (from Serial to LoRa)
    if (Serial.available())
    {
        String line = Serial.readStringUntil('\n');
        line.trim();
        if (line.length())
            sendMessageReliable(line);
    }

    // 2) Receive (from LoRa to Serial + ACK/ACKF)
    int psz = LoRa.parsePacket();
    if (psz)
    {
        String pkt;
        while (LoRa.available())
            pkt += (char)LoRa.read();
        String s, d, t;
        long seq = -1, idx = -1, tot = -1;
        uint64_t b = 0, k = 0;

        if (parseACK(pkt, s, d, seq, b, k) || parseACKF(pkt, s, d, seq, idx))
            return;

        if (parseMSG(pkt, s, d, seq, t))
        {
            int rssi = LoRa.packetRssi();
            float d_m = estimateDistanceMetersFromRSSI(rssi);
            rxDataPktsTotal++;
            rxBytesTotal += t.length();

            String out = "MSG," + s +
                         "," + String(seq) +
                         "," + String(rssi) +
                         "," + String(d_m, 2) +
                         "," + t;
            serialPrintLnChunked(out);

            oled3("RX MSG (" + String(seq) + ")", t.substring(0, 16), "d~" + String(d_m, 1) + "m");

            delay(RX_ACK_DELAY_MS);
            String ack = "ACK," + myId + "," + s + "," + String(seq) + "," +
                         String((unsigned long long)rxBytesTotal) + "," +
                         String((unsigned long long)rxDataPktsTotal);
            sendLoRa(ack);
            return;
        }

        if (parseMSGF(pkt, s, d, seq, idx, tot, t))
        {
            int rssi = LoRa.packetRssi();
            float d_m = estimateDistanceMetersFromRSSI(rssi);

            rxDataPktsTotal++;
            rxBytesTotal += t.length();

            String out = "FRAG," + s +
                         "," + String(seq) +
                         "," + String(idx) +
                         "," + String(tot) +
                         "," + String(rssi) +
                         "," + String(d_m, 2) +
                         "," + t;
            serialPrintLnChunked(out);

            oled3("RX FRAG (" + String(seq) + ")",
                  "i=" + String(idx) + "/" + String(tot),
                  "d~" + String(d_m, 1) + "m");

            delay(RX_ACK_DELAY_MS);
            String ackf = "ACKF," + myId + "," + s + "," + String(seq) + "," + String(idx);
            sendLoRa(ackf);
            Serial.println("[TX ACKF] " + ackf);
            return;
        }
    }
}

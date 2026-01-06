/*
  LoRa SF/BW Sweep + TX/RX Power (electrical) logging to Serial for PC CSV capture
  - SAME sketch for TX and RX devices (role set via Serial, saved in NVS)
  - TX trigger from PC: sends TRIG over LoRa
  - RX waits for TRIG, then both follow same SF/BW schedule
  - Every 10 seconds: change config to next (SF,BW) combo
  - TX sends: "HELLO,sf,bw,seq,Hello SF=.. and BW=.."
  - RX replies ACK: "ACK,seq"
  - Serial outputs lines starting with: LOG,...

  Power logging:
    - txp_dbm: configured TX output power setting (e.g., 17 dBm)
    - ptx_mw / prx_mw: estimated electrical power consumption (mW)
    - ptx_dbm_eq / prx_dbm_eq: 10*log10(mW) (math conversion, not RF)
    - RSSI / ACK_RSSI: RF received signal strength in dBm

  Region: AS923 @ 923 MHz
*/

#include <SPI.h>
#include <LoRa.h>
#include <Preferences.h>
#include <math.h>

// ===================== RADIO / BOARD PINS =====================
#define FREQ_HZ 923E6
#define LORA_SYNC 0xA5

#define SCK 5
#define MISO 19
#define MOSI 27
#define SS 18
#define RST 14
#define DIO0 26

// ===================== TEST SWEEP CONFIG =====================
static const uint8_t SF_VALUES[] = {7, 8, 9, 10, 11, 12};
static const uint8_t NUM_SF = sizeof(SF_VALUES) / sizeof(SF_VALUES[0]);

static const long BW_VALUES_HZ[] = {125000, 250000, 500000};
static const uint8_t NUM_BW = sizeof(BW_VALUES_HZ) / sizeof(BW_VALUES_HZ[0]);

static const uint32_t SLOT_MS = 10000;       // 10s per config
static const uint32_t START_DELAY_MS = 2500; // after trigger (align both)

static const bool CRC_ON = true;
static const bool IH_EXPLICIT = true; // explicit header
static const uint8_t CR_DENOM = 5;    // CR 4/5
static const uint16_t PREAMBLE_SYMS = 8;

// TX Power setting (radio output power setting)
static const int TX_POWER_DBM = 17;
static const uint8_t PA_PIN = PA_OUTPUT_PA_BOOST_PIN;

// retry behavior inside slot (prevents “RX missed the packet”)
static const uint32_t ACK_TIMEOUT_MS = 1200; // wait ACK
static const uint32_t RETRY_GAP_MS = 1000;   // resend within slot if no ACK

// ===================== ROLE / STATE =====================
enum Role : uint8_t
{
    ROLE_RX = 0,
    ROLE_TX = 1
};
enum State : uint8_t
{
    ST_IDLE = 0,
    ST_ARMED = 1,
    ST_RUNNING = 2,
    ST_DONE = 3
};

Preferences prefs;
Role myRole = ROLE_RX;
State st = ST_IDLE;

String myId = "????????????";

// schedule timing
uint32_t testId = 0;
uint32_t t0_ms = 0; // schedule start time in millis()
int lastComboIdx = -1;

// TX counters / per-slot send control
uint32_t txSeq = 0;
uint32_t lastAttemptMs = 0;
bool slotAcked = false;

// ===================== HELPERS =====================
static void macTo12Hex(String &out)
{
    uint64_t mac = ESP.getEfuseMac();
    char buf[13];
    sprintf(buf, "%04X%08X", (uint16_t)(mac >> 32), (uint32_t)mac);
    out = String(buf);
}

static void setRadio(uint8_t sf, long bw_hz)
{
    LoRa.setSpreadingFactor(sf);
    LoRa.setSignalBandwidth(bw_hz);
    LoRa.setCodingRate4(CR_DENOM);
    LoRa.setPreambleLength(PREAMBLE_SYMS);
    if (CRC_ON)
        LoRa.enableCrc();
    else
        LoRa.disableCrc();
    LoRa.setSyncWord(LORA_SYNC);
    LoRa.setTxPower(TX_POWER_DBM, PA_PIN);
}

static double loraToA_ms(uint8_t sf, long bw_hz, int payload_len)
{
    const double bw = (double)bw_hz;
    const double sf_d = (double)sf;
    const double Tsym = (pow(2.0, sf_d) / bw); // seconds
    const double Tpreamble = (PREAMBLE_SYMS + 4.25) * Tsym;

    const int IH = IH_EXPLICIT ? 0 : 1;
    const int CRC = CRC_ON ? 1 : 0;
    const int DE = ((sf >= 11) && (bw_hz == 125000)) ? 1 : 0;

    int CR = 1; // for 4/5

    double tmp = (8.0 * payload_len - 4.0 * sf + 28.0 + 16.0 * CRC - 20.0 * IH);
    double denom = 4.0 * (sf - 2.0 * DE);
    double nPayload = 8.0 + (tmp > 0 ? ceil(tmp / denom) * (CR + 4) : 0.0);

    double Tpayload = nPayload * Tsym;
    double Tpacket = Tpreamble + Tpayload;
    return Tpacket * 1000.0;
}

// ===== Electrical power estimate (mW) =====
// These are “board-level rough typicals” (good for comparisons across SF/BW)
static double txCurrent_mA(int dbm)
{
    if (dbm >= 17)
        return 120.0;
    if (dbm >= 14)
        return 80.0;
    if (dbm >= 10)
        return 45.0;
    return 35.0;
}
static double rxCurrent_mA()
{
    return 12.0;
}
static double power_mW(double current_mA, double v = 3.3)
{
    return v * (current_mA / 1000.0) * 1000.0; // V*A => W ; *1000 => mW
}
static double dbm_equiv_from_mW(double mw)
{
    if (mw <= 0.000001)
        return -120.0;
    return 10.0 * log10(mw);
}
static double energy_mJ(double current_mA, double toa_ms, double v = 3.3)
{
    return v * (current_mA / 1000.0) * (toa_ms / 1000.0) * 1000.0;
}

static void logLine(const String &line)
{
    Serial.println(line);
}

static void saveRole(Role r)
{
    prefs.begin("sweep", false);
    prefs.putUChar("role", (uint8_t)r);
    prefs.end();
}
static Role loadRole()
{
    prefs.begin("sweep", true);
    uint8_t r = prefs.getUChar("role", 0);
    prefs.end();
    return (r == 1) ? ROLE_TX : ROLE_RX;
}

static void sendPkt(const String &s)
{
    LoRa.beginPacket();
    LoRa.print(s);
    LoRa.endPacket();
}

// ===================== PARSERS =====================
static bool parseTrig(const String &pkt, uint32_t &outTestId)
{
    if (!pkt.startsWith("TRIG,"))
        return false;
    outTestId = (uint32_t)pkt.substring(5).toInt();
    return true;
}
static bool parseHello(const String &pkt, uint8_t &sf, long &bw, uint32_t &seq)
{
    if (!pkt.startsWith("HELLO,"))
        return false;
    int p1 = pkt.indexOf(',', 6);
    if (p1 < 0)
        return false;
    int p2 = pkt.indexOf(',', p1 + 1);
    if (p2 < 0)
        return false;
    int p3 = pkt.indexOf(',', p2 + 1);
    if (p3 < 0)
        return false;
    sf = (uint8_t)pkt.substring(6, p1).toInt();
    bw = pkt.substring(p1 + 1, p2).toInt();
    seq = (uint32_t)pkt.substring(p2 + 1, p3).toInt();
    return true;
}
static bool parseAck(const String &pkt, uint32_t &seq)
{
    if (!pkt.startsWith("ACK,"))
        return false;
    seq = (uint32_t)pkt.substring(4).toInt();
    return true;
}

// ===================== ACK WAIT (TX) =====================
static bool waitAck(uint32_t expectSeq, uint32_t timeoutMs, int &outRssi)
{
    uint32_t deadline = millis() + timeoutMs;
    while ((int32_t)(millis() - deadline) < 0)
    {
        int psz = LoRa.parsePacket();
        if (psz)
        {
            String pkt;
            while (LoRa.available())
                pkt += (char)LoRa.read();
            uint32_t s = 0;
            if (parseAck(pkt, s) && s == expectSeq)
            {
                outRssi = LoRa.packetRssi();
                return true;
            }
        }
        delay(1);
    }
    return false;
}

// ===================== SCHEDULE =====================
static int totalCombos() { return (int)(NUM_SF * NUM_BW); }

static void comboFromIdx(int idx, uint8_t &sf, long &bw)
{
    int sf_i = idx / NUM_BW;
    int bw_i = idx % NUM_BW;
    sf = SF_VALUES[sf_i];
    bw = BW_VALUES_HZ[bw_i];
}

static void startRunning(uint32_t newTestId)
{
    testId = newTestId;
    t0_ms = millis() + START_DELAY_MS;
    st = ST_RUNNING;
    lastComboIdx = -1;

    txSeq = 0;
    lastAttemptMs = 0;
    slotAcked = false;

    logLine("LOG,EVENT,TEST_START,testId," + String(testId) + ",role," + String((myRole == ROLE_TX) ? "TX" : "RX"));
}

static void handleRunning()
{
    if ((int32_t)(millis() - t0_ms) < 0)
        return;

    uint32_t elapsed = millis() - t0_ms;
    int idx = (int)(elapsed / SLOT_MS);

    if (idx >= totalCombos())
    {
        st = ST_DONE;
        logLine("LOG,EVENT,TEST_DONE,testId," + String(testId) + ",role," + String((myRole == ROLE_TX) ? "TX" : "RX"));
        return;
    }

    // slot change => apply config and reset slot flags
    if (idx != lastComboIdx)
    {
        lastComboIdx = idx;
        slotAcked = false;
        lastAttemptMs = 0;

        uint8_t sf;
        long bw;
        comboFromIdx(idx, sf, bw);
        setRadio(sf, bw);

        logLine("LOG,CFG,testId," + String(testId) +
                ",slot," + String(idx) +
                ",sf," + String(sf) +
                ",bw," + String(bw));
    }

    // TX: retry within slot until ACK or slot ends
    if (myRole == ROLE_TX && !slotAcked)
    {
        uint32_t now = millis();
        if (lastAttemptMs == 0 || (now - lastAttemptMs) >= RETRY_GAP_MS)
        {
            lastAttemptMs = now;

            uint8_t sf;
            long bw;
            comboFromIdx(lastComboIdx, sf, bw);

            String msg = "Hello SF=" + String(sf) + " and BW=" + String(bw);
            String pkt = "HELLO," + String(sf) + "," + String(bw) + "," + String(txSeq) + "," + msg;

            int payloadLen = pkt.length();
            double toa = loraToA_ms(sf, bw, payloadLen);

            double itx = txCurrent_mA(TX_POWER_DBM);
            double ptx_mw = power_mW(itx);
            double ptx_dbm_eq = dbm_equiv_from_mW(ptx_mw);
            double etx = energy_mJ(itx, toa);

            sendPkt(pkt);

            int ackRssi = -200;
            bool got = waitAck(txSeq, ACK_TIMEOUT_MS, ackRssi);
            if (got)
                slotAcked = true;

            // LOG,TX,... + power fields
            logLine("LOG,TX,testId," + String(testId) +
                    ",slot," + String(lastComboIdx) +
                    ",sf," + String(sf) +
                    ",bw," + String(bw) +
                    ",seq," + String(txSeq) +
                    ",len," + String(payloadLen) +
                    ",toa_ms," + String(toa, 2) +
                    ",txp_dbm," + String(TX_POWER_DBM) +
                    ",ptx_mw," + String(ptx_mw, 3) +
                    ",ptx_dbm_eq," + String(ptx_dbm_eq, 2) +
                    ",etx_mJ," + String(etx, 4) +
                    ",ack," + String(got ? 1 : 0) +
                    ",ack_rssi," + String(ackRssi));
            // only increment seq when ACKed (so RX sees same seq for retries)
            if (got)
                txSeq++;
        }
    }
}

// ===================== SERIAL COMMANDS =====================
static void handleSerialLine(String line)
{
    line.trim();
    if (!line.length())
        return;

    String u = line;
    u.toUpperCase();

    if (u.startsWith("ROLE "))
    {
        if (u.endsWith("TX"))
        {
            myRole = ROLE_TX;
            saveRole(myRole);
        }
        else
        {
            myRole = ROLE_RX;
            saveRole(myRole);
        }
        logLine("LOG,EVENT,ROLE_SET,role," + String((myRole == ROLE_TX) ? "TX" : "RX"));
        return;
    }
    if (u == "ARM")
    {
        st = ST_ARMED;
        logLine("LOG,EVENT,ARMED");
        return;
    }
    if (u == "GO")
    {
        if (myRole != ROLE_TX)
        {
            logLine("LOG,ERROR,NOT_TX_ROLE");
            return;
        }
        uint32_t newTest = (uint32_t)millis();
        sendPkt("TRIG," + String(newTest));
        logLine("LOG,EVENT,TRIG_SENT,testId," + String(newTest));
        startRunning(newTest);
        return;
    }
    if (u == "STATUS")
    {
        logLine("LOG,EVENT,STATUS,role," + String((myRole == ROLE_TX) ? "TX" : "RX") + ",state," + String((int)st));
        return;
    }

    logLine("LOG,WARN,UNKNOWN_CMD,cmd," + line);
}

// ===================== SETUP/LOOP =====================
void setup()
{
    Serial.begin(115200);
    Serial.setTimeout(20);

    macTo12Hex(myId);
    myRole = loadRole();
    st = ST_IDLE;

    SPI.begin(SCK, MISO, MOSI, SS);
    LoRa.setPins(SS, RST, DIO0);
    if (!LoRa.begin(FREQ_HZ))
    {
        Serial.println("LOG,ERROR,LORA_BEGIN_FAIL");
        while (true)
            delay(1000);
    }

    // default listen config (RX must start somewhere)
    setRadio(8, 125000);

    logLine("LOG,EVENT,BOOT,id," + myId + ",role," + String((myRole == ROLE_TX) ? "TX" : "RX"));
    logLine("LOG,EVENT,HELP,cmds,ROLE TX|ROLE RX|ARM|GO|STATUS");
}

void loop()
{
    if (Serial.available())
    {
        String line = Serial.readStringUntil('\n');
        handleSerialLine(line);
    }

    // LoRa RX
    int psz = LoRa.parsePacket();
    if (psz)
    {
        String pkt;
        while (LoRa.available())
            pkt += (char)LoRa.read();
        int rssi = LoRa.packetRssi();

        // TRIG handling
        uint32_t trigId = 0;
        if (parseTrig(pkt, trigId))
        {
            if (myRole == ROLE_RX && (st == ST_ARMED || st == ST_IDLE))
            {
                logLine("LOG,EVENT,TRIG_RX,testId," + String(trigId) + ",rssi," + String(rssi));
                startRunning(trigId);
            }
            return;
        }

        // HELLO handling => ACK + RX power fields
        uint8_t sf;
        long bw;
        uint32_t seq;
        if (parseHello(pkt, sf, bw, seq))
        {
            int payloadLen = pkt.length();
            double toa = loraToA_ms(sf, bw, payloadLen);

            double irx = rxCurrent_mA();
            double prx_mw = power_mW(irx);
            double prx_dbm_eq = dbm_equiv_from_mW(prx_mw);
            double erx = energy_mJ(irx, toa);

            sendPkt("ACK," + String(seq));

            logLine("LOG,RX,testId," + String(testId) +
                    ",sf," + String(sf) +
                    ",bw," + String(bw) +
                    ",seq," + String(seq) +
                    ",len," + String(payloadLen) +
                    ",rssi," + String(rssi) +
                    ",toa_ms," + String(toa, 2) +
                    ",prx_mw," + String(prx_mw, 3) +
                    ",prx_dbm_eq," + String(prx_dbm_eq, 2) +
                    ",erx_mJ," + String(erx, 4));
            return;
        }
    }

    if (st == ST_RUNNING)
        handleRunning();
}

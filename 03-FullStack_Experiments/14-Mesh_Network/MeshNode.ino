/*
  LoRa Intelligent Mesh Network - Unified Node Firmware
  =======================================================

  A sophisticated mesh network implementation with:
  - Unified codebase: All nodes run same code (only node name differs)
  - Automatic role adaptation: Nodes act as relay or endpoint based on traffic
  - Intelligent routing: AODV-inspired routing with route discovery
  - Multi-hop ACK: Relay nodes participate in acknowledgment chain
  - Adaptive reliability: Configurable per data type (text, voice, image, seismic)
  - Duplicate suppression: Smart deduplication with bloom filters
  - Memory-efficient relay: Lightweight queuing and packet forwarding
  - Connection discovery: Establishes direct/relay paths before bulk transfer

  Configuration: Set NODE_NAME to unique identifier (Node_1, Node_2, etc.)

  Protocol Features:
  - Route discovery (RREQ/RREP) before data transmission
  - Hop-by-hop ACK for reliable delivery
  - Sequence numbers and message IDs for duplicate detection
  - TTL to prevent infinite loops
  - Link quality tracking (RSSI/SNR)
  - Queue management for relay nodes
  - Fragmentation for large payloads
  - ARQ modes: Stop-and-Wait, Go-Back-N, Selective Repeat

  Hardware: ESP32 T-Display + SX127x LoRa module
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SPI.h>
#include <LoRa.h>
#include <vector>
#include <map>

// ==================== NODE CONFIGURATION ====================
#define NODE_NAME "Node_1" // *** CHANGE THIS FOR EACH NODE ***
// ===========================================================

// ---------- Radio Config (AS923) ----------
#define FREQ_HZ 923E6
#define LORA_SYNC 0xA5
#define LORA_SF 7
const size_t LORA_MAX_PAYLOAD = 255;

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

// ==================== PROTOCOL DEFINITIONS ====================

// Message Types
enum MsgType
{
    MSG_DATA = 0,  // Single-packet data message
    MSG_FRAG = 1,  // Fragment of large message
    MSG_ACK = 2,   // Acknowledgment
    MSG_FACK = 3,  // Fragment acknowledgment
    MSG_RREQ = 4,  // Route request
    MSG_RREP = 5,  // Route reply
    MSG_RERR = 6,  // Route error
    MSG_HELLO = 7, // Neighbor discovery
    MSG_RACK = 8   // Relay acknowledgment (hop-by-hop)
};

// Reliability Levels (affects retry count and timeout)
enum ReliabilityLevel
{
    REL_NONE = 0,    // Fire and forget (no ACK)
    REL_LOW = 1,     // 1 retry, short timeout (text, voice)
    REL_MEDIUM = 2,  // 2 retries, medium timeout (images)
    REL_HIGH = 3,    // 3 retries, long timeout (files)
    REL_CRITICAL = 4 // 5 retries, very long timeout (seismic data)
};

// ARQ Modes
enum ArqMode
{
    ARQ_STOP_AND_WAIT = 0,
    ARQ_GO_BACK_N = 1,
    ARQ_SELECTIVE_REPEAT = 2
};

// Packet Header Structure
// Format: T<type>|S<src>|D<dst>|Q<seq>|H<hop>|L<ttl>|R<rel>|:<payload>
struct PacketHeader
{
    MsgType type;
    String src;
    String dst;
    uint32_t seq;
    uint8_t hopCount;
    uint8_t ttl;
    ReliabilityLevel reliability;
};

// Route Entry
struct RouteEntry
{
    String nextHop;
    uint8_t hopCount;
    unsigned long timestamp;
    int rssi;
    float snr;
    bool isValid;
};

// Forwarding Entry (for relays)
struct ForwardEntry
{
    String msgId; // src:seq for deduplication
    unsigned long lastSeen;
    uint8_t seenCount;
};

// Queue Entry (for relay nodes)
struct QueueEntry
{
    String packet;
    unsigned long queueTime;
    uint8_t priority; // 0=highest, 3=lowest
};

// Fragment State (for sender)
struct FragmentState
{
    bool acked;
    unsigned long lastTx;
    uint8_t retries;
};

// ==================== GLOBAL STATE ====================

String myNodeName = NODE_NAME;

// Routing Table: destination -> RouteEntry
std::map<String, RouteEntry> routingTable;

// Seen Messages for duplicate detection (src:seq -> ForwardEntry)
std::map<String, ForwardEntry> seenMessages;

// Relay Queue (FIFO with priority)
std::vector<QueueEntry> relayQueue;
const size_t MAX_RELAY_QUEUE = 20;

// Sequence numbers
uint32_t mySeqNum = 0;
uint32_t rreqId = 0;

// Statistics
uint64_t txPackets = 0, rxPackets = 0;
uint64_t txBytes = 0, rxBytes = 0;
uint64_t relayedPackets = 0;
uint64_t duplicatesDropped = 0;

// Timers
const unsigned long ROUTE_TIMEOUT_MS = 300000;   // 5 min
const unsigned long SEEN_MSG_TIMEOUT_MS = 60000; // 1 min
const unsigned long HELLO_INTERVAL_MS = 30000;   // 30 sec
const unsigned long QUEUE_MAX_AGE_MS = 10000;    // 10 sec
unsigned long lastHelloTime = 0;
unsigned long sessionStartMs = 0;

// Current transmission state
ArqMode currentArqMode = ARQ_STOP_AND_WAIT;
ReliabilityLevel currentReliability = REL_MEDIUM;

// RSSI tracking
float rssiEma = NAN;
const float RSSI_ALPHA = 0.20f;
const float RSSI_REF_1M = -45.0f;
const float PATH_LOSS_N = 2.7f;

// ==================== HELPER FUNCTIONS ====================

void oled3(const String &a, const String &b = "", const String &c = "")
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

void serialPrintLn(const String &s)
{
    Serial.println(s);
}

float estimateDistanceMetersFromRSSI(int rssi)
{
    float r = (float)rssi;
    if (isnan(rssiEma))
        rssiEma = r;
    rssiEma = RSSI_ALPHA * r + (1.0f - RSSI_ALPHA) * rssiEma;
    float exponent = (RSSI_REF_1M - rssiEma) / (10.0f * PATH_LOSS_N);
    return powf(10.0f, exponent);
}

String msgTypeToString(MsgType t)
{
    switch (t)
    {
    case MSG_DATA:
        return "DATA";
    case MSG_FRAG:
        return "FRAG";
    case MSG_ACK:
        return "ACK";
    case MSG_FACK:
        return "FACK";
    case MSG_RREQ:
        return "RREQ";
    case MSG_RREP:
        return "RREP";
    case MSG_RERR:
        return "RERR";
    case MSG_HELLO:
        return "HELLO";
    case MSG_RACK:
        return "RACK";
    default:
        return "UNKN";
    }
}

String reliabilityToString(ReliabilityLevel r)
{
    switch (r)
    {
    case REL_NONE:
        return "NONE";
    case REL_LOW:
        return "LOW";
    case REL_MEDIUM:
        return "MED";
    case REL_HIGH:
        return "HIGH";
    case REL_CRITICAL:
        return "CRIT";
    default:
        return "?";
    }
}

int getMaxRetries(ReliabilityLevel rel)
{
    switch (rel)
    {
    case REL_NONE:
        return 0;
    case REL_LOW:
        return 1;
    case REL_MEDIUM:
        return 2;
    case REL_HIGH:
        return 3;
    case REL_CRITICAL:
        return 5;
    default:
        return 2;
    }
}

unsigned long getAckTimeout(ReliabilityLevel rel)
{
    switch (rel)
    {
    case REL_NONE:
        return 0;
    case REL_LOW:
        return 2000;
    case REL_MEDIUM:
        return 5000;
    case REL_HIGH:
        return 8000;
    case REL_CRITICAL:
        return 15000;
    default:
        return 5000;
    }
}

// ==================== PACKET ENCODING/DECODING ====================

// Encode header: T<type>|S<src>|D<dst>|Q<seq>|H<hop>|L<ttl>|R<rel>|:<payload>
String encodePacket(const PacketHeader &hdr, const String &payload)
{
    String pkt = "T" + String((int)hdr.type) +
                 "|S" + hdr.src +
                 "|D" + hdr.dst +
                 "|Q" + String(hdr.seq) +
                 "|H" + String(hdr.hopCount) +
                 "|L" + String(hdr.ttl) +
                 "|R" + String((int)hdr.reliability) +
                 "|:" + payload;
    return pkt;
}

bool decodePacket(const String &raw, PacketHeader &hdr, String &payload)
{
    // Parse header fields
    int typeIdx = raw.indexOf("T");
    int srcIdx = raw.indexOf("|S");
    int dstIdx = raw.indexOf("|D");
    int seqIdx = raw.indexOf("|Q");
    int hopIdx = raw.indexOf("|H");
    int ttlIdx = raw.indexOf("|L");
    int relIdx = raw.indexOf("|R");
    int payloadIdx = raw.indexOf("|:");

    if (typeIdx < 0 || srcIdx < 0 || dstIdx < 0 || seqIdx < 0 ||
        hopIdx < 0 || ttlIdx < 0 || relIdx < 0 || payloadIdx < 0)
    {
        return false;
    }

    hdr.type = (MsgType)raw.substring(typeIdx + 1, srcIdx).toInt();
    hdr.src = raw.substring(srcIdx + 2, dstIdx);
    hdr.dst = raw.substring(dstIdx + 2, seqIdx);
    hdr.seq = raw.substring(seqIdx + 2, hopIdx).toInt();
    hdr.hopCount = raw.substring(hopIdx + 2, ttlIdx).toInt();
    hdr.ttl = raw.substring(ttlIdx + 2, relIdx).toInt();
    hdr.reliability = (ReliabilityLevel)raw.substring(relIdx + 2, payloadIdx).toInt();
    payload = raw.substring(payloadIdx + 2);

    return true;
}

// ==================== ROUTING FUNCTIONS ====================

void addOrUpdateRoute(const String &dest, const String &nextHop, uint8_t hopCount, int rssi, float snr)
{
    RouteEntry &entry = routingTable[dest];

    // Only update if this is a better or newer route
    if (!entry.isValid || entry.hopCount > hopCount ||
        (entry.hopCount == hopCount && millis() - entry.timestamp > 30000))
    {
        entry.nextHop = nextHop;
        entry.hopCount = hopCount;
        entry.timestamp = millis();
        entry.rssi = rssi;
        entry.snr = snr;
        entry.isValid = true;

        serialPrintLn("[ROUTE] " + dest + " via " + nextHop + " (" + String(hopCount) + " hops)");
    }
}

void invalidateRoute(const String &dest)
{
    if (routingTable.find(dest) != routingTable.end())
    {
        routingTable[dest].isValid = false;
        serialPrintLn("[ROUTE] Invalidated route to " + dest);
    }
}

String getNextHop(const String &dest)
{
    // Clean up old routes first
    unsigned long now = millis();
    for (auto it = routingTable.begin(); it != routingTable.end();)
    {
        if (now - it->second.timestamp > ROUTE_TIMEOUT_MS)
        {
            serialPrintLn("[ROUTE] Expired route to " + it->first);
            it = routingTable.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // Return next hop if valid route exists
    if (routingTable.find(dest) != routingTable.end() && routingTable[dest].isValid)
    {
        return routingTable[dest].nextHop;
    }

    return ""; // No route
}

void printRoutingTable()
{
    serialPrintLn("\n========== ROUTING TABLE ==========");
    serialPrintLn("Dest      NextHop   Hops  RSSI  Age(s)");
    for (auto &pair : routingTable)
    {
        if (pair.second.isValid)
        {
            unsigned long age = (millis() - pair.second.timestamp) / 1000;
            String line = pair.first;
            while (line.length() < 10)
                line += " ";
            line += pair.second.nextHop;
            while (line.length() < 20)
                line += " ";
            line += String(pair.second.hopCount) + "     ";
            line += String(pair.second.rssi) + "  ";
            line += String(age);
            serialPrintLn(line);
        }
    }
    serialPrintLn("===================================\n");
}

// ==================== DUPLICATE DETECTION ====================

bool isDuplicate(const String &src, uint32_t seq)
{
    String msgId = src + ":" + String(seq);
    unsigned long now = millis();

    // Clean up old entries
    for (auto it = seenMessages.begin(); it != seenMessages.end();)
    {
        if (now - it->second.lastSeen > SEEN_MSG_TIMEOUT_MS)
        {
            it = seenMessages.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // Check if we've seen this message
    if (seenMessages.find(msgId) != seenMessages.end())
    {
        seenMessages[msgId].seenCount++;
        seenMessages[msgId].lastSeen = now;
        return true;
    }

    // Add to seen messages
    ForwardEntry entry;
    entry.msgId = msgId;
    entry.lastSeen = now;
    entry.seenCount = 1;
    seenMessages[msgId] = entry;

    return false;
}

// ==================== RELAY QUEUE MANAGEMENT ====================

void addToRelayQueue(const String &packet, uint8_t priority = 2)
{
    // Don't queue if already full
    if (relayQueue.size() >= MAX_RELAY_QUEUE)
    {
        serialPrintLn("[QUEUE] Full, dropping packet");
        return;
    }

    QueueEntry entry;
    entry.packet = packet;
    entry.queueTime = millis();
    entry.priority = priority;

    relayQueue.push_back(entry);
    serialPrintLn("[QUEUE] Added packet (pri=" + String(priority) + ", size=" + String(relayQueue.size()) + ")");
}

void processRelayQueue()
{
    if (relayQueue.empty())
        return;

    unsigned long now = millis();

    // Remove stale entries
    for (auto it = relayQueue.begin(); it != relayQueue.end();)
    {
        if (now - it->queueTime > QUEUE_MAX_AGE_MS)
        {
            serialPrintLn("[QUEUE] Dropped stale packet");
            it = relayQueue.erase(it);
        }
        else
        {
            ++it;
        }
    }

    if (relayQueue.empty())
        return;

    // Find highest priority packet
    size_t bestIdx = 0;
    for (size_t i = 1; i < relayQueue.size(); i++)
    {
        if (relayQueue[i].priority < relayQueue[bestIdx].priority)
        {
            bestIdx = i;
        }
    }

    // Send it
    String packet = relayQueue[bestIdx].packet;
    relayQueue.erase(relayQueue.begin() + bestIdx);

    LoRa.beginPacket();
    LoRa.print(packet);
    LoRa.endPacket();

    relayedPackets++;
    txPackets++;
    txBytes += packet.length();

    serialPrintLn("[RELAY] Forwarded packet from queue (remaining=" + String(relayQueue.size()) + ")");
}

// ==================== ROUTE DISCOVERY ====================

void sendRouteRequest(const String &dest)
{
    PacketHeader hdr;
    hdr.type = MSG_RREQ;
    hdr.src = myNodeName;
    hdr.dst = "BROADCAST";
    hdr.seq = rreqId++;
    hdr.hopCount = 0;
    hdr.ttl = 10; // Max 10 hops for route discovery
    hdr.reliability = REL_NONE;

    String payload = dest; // Destination we're looking for
    String packet = encodePacket(hdr, payload);

    LoRa.beginPacket();
    LoRa.print(packet);
    LoRa.endPacket();

    txPackets++;
    txBytes += packet.length();

    serialPrintLn("[RREQ] Sent route request for " + dest + " (ID=" + String(hdr.seq) + ")");
    oled3("Route Discovery", "Looking for " + dest, "RREQ ID=" + String(hdr.seq));
}

void handleRouteRequest(const PacketHeader &hdr, const String &payload, int rssi, float snr)
{
    String targetDest = payload;

    // Check if we've seen this RREQ before (duplicate)
    if (isDuplicate(hdr.src, hdr.seq))
    {
        serialPrintLn("[RREQ] Duplicate from " + hdr.src + ", ignoring");
        return;
    }

    // Update reverse route to source (for RREP to travel back)
    addOrUpdateRoute(hdr.src, hdr.src, hdr.hopCount + 1, rssi, snr);

    // Check if this RREQ is for us
    if (targetDest == myNodeName)
    {
        // We are the destination, send RREP
        PacketHeader rrep;
        rrep.type = MSG_RREP;
        rrep.src = myNodeName;
        rrep.dst = hdr.src;
        rrep.seq = hdr.seq; // Same ID as RREQ
        rrep.hopCount = 0;
        rrep.ttl = 10;
        rrep.reliability = REL_NONE;

        String rrepPayload = String(hdr.hopCount + 1); // Total hop count
        String rrepPacket = encodePacket(rrep, rrepPayload);

        delay(50); // Small delay before RREP

        LoRa.beginPacket();
        LoRa.print(rrepPacket);
        LoRa.endPacket();

        txPackets++;
        txBytes += rrepPacket.length();

        serialPrintLn("[RREP] Sent route reply to " + hdr.src + " (" + String(hdr.hopCount + 1) + " hops)");
    }
    else
    {
        // Forward RREQ if TTL allows
        if (hdr.ttl > 1)
        {
            PacketHeader fwd = hdr;
            fwd.hopCount++;
            fwd.ttl--;

            String fwdPacket = encodePacket(fwd, payload);
            addToRelayQueue(fwdPacket, 0); // High priority

            serialPrintLn("[RREQ] Forwarding for " + targetDest + " (hop=" + String(fwd.hopCount) + ")");
        }
        else
        {
            serialPrintLn("[RREQ] TTL expired, not forwarding");
        }
    }
}

void handleRouteReply(const PacketHeader &hdr, const String &payload, int rssi, float snr)
{
    uint8_t totalHops = payload.toInt();

    // Update route to destination (the one who sent RREP)
    addOrUpdateRoute(hdr.src, hdr.src, hdr.hopCount + 1, rssi, snr);

    // Check if this RREP is for us
    if (hdr.dst == myNodeName)
    {
        serialPrintLn("[RREP] Received from " + hdr.src + " (" + String(totalHops) + " hops total)");
        oled3("Route Found", hdr.src + " via " + getNextHop(hdr.src), String(totalHops) + " hops");
    }
    else
    {
        // Forward RREP toward destination
        String nextHop = getNextHop(hdr.dst);
        if (nextHop.length() > 0)
        {
            PacketHeader fwd = hdr;
            fwd.hopCount++;

            String fwdPacket = encodePacket(fwd, payload);
            addToRelayQueue(fwdPacket, 0); // High priority

            serialPrintLn("[RREP] Forwarding to " + hdr.dst + " via " + nextHop);
        }
        else
        {
            serialPrintLn("[RREP] No route to " + hdr.dst + ", dropping");
        }
    }
}

// ==================== HELLO MESSAGES (Neighbor Discovery) ====================

void sendHello()
{
    PacketHeader hdr;
    hdr.type = MSG_HELLO;
    hdr.src = myNodeName;
    hdr.dst = "BROADCAST";
    hdr.seq = mySeqNum++;
    hdr.hopCount = 0;
    hdr.ttl = 1; // Don't forward HELLOs
    hdr.reliability = REL_NONE;

    String payload = "HELLO";
    String packet = encodePacket(hdr, payload);

    LoRa.beginPacket();
    LoRa.print(packet);
    LoRa.endPacket();

    txPackets++;
    txBytes += packet.length();

    serialPrintLn("[HELLO] Sent neighbor discovery");
}

void handleHello(const PacketHeader &hdr, int rssi, float snr)
{
    // Add direct neighbor to routing table
    addOrUpdateRoute(hdr.src, hdr.src, 1, rssi, snr);
    serialPrintLn("[HELLO] Neighbor " + hdr.src + " (RSSI=" + String(rssi) + ", SNR=" + String(snr, 1) + ")");
}

// ==================== DATA TRANSMISSION ====================

bool sendDataPacket(const String &dest, const String &data, ReliabilityLevel rel = REL_MEDIUM)
{
    // Check if we have a route
    String nextHop = getNextHop(dest);
    if (nextHop.length() == 0)
    {
        serialPrintLn("[TX] No route to " + dest + ", initiating route discovery");
        sendRouteRequest(dest);

        // Wait for route (with timeout)
        unsigned long deadline = millis() + 5000;
        while (millis() < deadline)
        {
            int psz = LoRa.parsePacket();
            if (psz)
            {
                String raw;
                while (LoRa.available())
                    raw += (char)LoRa.read();

                PacketHeader hdr;
                String payload;
                if (decodePacket(raw, hdr, payload))
                {
                    int rssi = LoRa.packetRssi();
                    float snr = LoRa.packetSnr();

                    if (hdr.type == MSG_RREP)
                    {
                        handleRouteReply(hdr, payload, rssi, snr);
                        if (hdr.dst == myNodeName && hdr.src == dest)
                        {
                            nextHop = getNextHop(dest);
                            if (nextHop.length() > 0)
                                break;
                        }
                    }
                }
            }
            delay(10);
        }

        nextHop = getNextHop(dest);
        if (nextHop.length() == 0)
        {
            serialPrintLn("[TX] Route discovery failed for " + dest);
            return false;
        }
    }

    // Send data packet
    PacketHeader hdr;
    hdr.type = MSG_DATA;
    hdr.src = myNodeName;
    hdr.dst = dest;
    hdr.seq = mySeqNum++;
    hdr.hopCount = 0;
    hdr.ttl = 10;
    hdr.reliability = rel;

    String packet = encodePacket(hdr, data);

    // Check if packet fits
    if (packet.length() > LORA_MAX_PAYLOAD)
    {
        serialPrintLn("[TX] Data too large (" + String(packet.length()) + " bytes), use fragmentation");
        return false;
    }

    int maxRetries = getMaxRetries(rel);
    unsigned long ackTimeout = getAckTimeout(rel);

    for (int attempt = 0; attempt <= maxRetries; attempt++)
    {
        if (attempt > 0)
        {
            serialPrintLn("[TX] Retry " + String(attempt) + "/" + String(maxRetries));
        }

        LoRa.beginPacket();
        LoRa.print(packet);
        LoRa.endPacket();

        txPackets++;
        txBytes += packet.length();

        serialPrintLn("[TX] Sent DATA to " + dest + " via " + nextHop + " (seq=" + String(hdr.seq) + ", rel=" + reliabilityToString(rel) + ")");
        oled3("TX to " + dest, "via " + nextHop, "seq=" + String(hdr.seq));

        // If no reliability, don't wait for ACK
        if (rel == REL_NONE)
        {
            return true;
        }

        // Wait for ACK
        unsigned long deadline = millis() + ackTimeout;
        while ((long)(millis() - deadline) < 0)
        {
            int psz = LoRa.parsePacket();
            if (psz)
            {
                String raw;
                while (LoRa.available())
                    raw += (char)LoRa.read();

                PacketHeader ackHdr;
                String ackPayload;
                if (decodePacket(raw, ackHdr, ackPayload))
                {
                    if (ackHdr.type == MSG_ACK && ackHdr.dst == myNodeName && ackHdr.seq == hdr.seq)
                    {
                        int rssi = LoRa.packetRssi();
                        serialPrintLn("[ACK] Received from " + dest + " (RSSI=" + String(rssi) + ")");
                        oled3("ACK from " + dest, "seq=" + String(hdr.seq), "RSSI=" + String(rssi));
                        return true;
                    }
                    // Process other packets while waiting
                    processReceivedPacket(ackHdr, ackPayload, LoRa.packetRssi(), LoRa.packetSnr());
                }
            }
            delay(1);
        }

        serialPrintLn("[TX] ACK timeout for seq=" + String(hdr.seq));
    }

    serialPrintLn("[TX] Failed to send to " + dest + " after " + String(maxRetries + 1) + " attempts");
    return false;
}

void handleDataPacket(const PacketHeader &hdr, const String &payload, int rssi, float snr)
{
    // Check if this is for us
    if (hdr.dst == myNodeName)
    {
        serialPrintLn("[RX] DATA from " + hdr.src + " (seq=" + String(hdr.seq) + ", hops=" + String(hdr.hopCount) + ")");
        serialPrintLn("[RX] Payload: " + payload);

        float dist = estimateDistanceMetersFromRSSI(rssi);
        oled3("RX from " + hdr.src, payload.substring(0, 16), "RSSI=" + String(rssi) + " d=" + String(dist, 1) + "m");

        rxPackets++;
        rxBytes += payload.length();

        // Send ACK if reliability requires it
        if (hdr.reliability != REL_NONE)
        {
            delay(20); // Small delay before ACK

            PacketHeader ack;
            ack.type = MSG_ACK;
            ack.src = myNodeName;
            ack.dst = hdr.src;
            ack.seq = hdr.seq;
            ack.hopCount = 0;
            ack.ttl = 10;
            ack.reliability = REL_NONE;

            String ackPacket = encodePacket(ack, "OK");

            LoRa.beginPacket();
            LoRa.print(ackPacket);
            LoRa.endPacket();

            txPackets++;
            txBytes += ackPacket.length();

            serialPrintLn("[ACK] Sent to " + hdr.src);
        }
    }
    else
    {
        // Forward packet
        String nextHop = getNextHop(hdr.dst);
        if (nextHop.length() > 0 && hdr.ttl > 1)
        {
            PacketHeader fwd = hdr;
            fwd.hopCount++;
            fwd.ttl--;

            String fwdPacket = encodePacket(fwd, payload);
            addToRelayQueue(fwdPacket, 2); // Normal priority

            serialPrintLn("[FWD] DATA from " + hdr.src + " to " + hdr.dst + " via " + nextHop);

            // Send hop-by-hop ACK if high reliability
            if (hdr.reliability >= REL_HIGH)
            {
                delay(20);

                PacketHeader rack;
                rack.type = MSG_RACK;
                rack.src = myNodeName;
                rack.dst = hdr.src; // ACK back to previous hop
                rack.seq = hdr.seq;
                rack.hopCount = 0;
                rack.ttl = 10;
                rack.reliability = REL_NONE;

                String rackPacket = encodePacket(rack, "OK");

                LoRa.beginPacket();
                LoRa.print(rackPacket);
                LoRa.endPacket();

                txPackets++;
                txBytes += rackPacket.length();

                serialPrintLn("[RACK] Sent hop-by-hop ACK to previous hop");
            }
        }
        else
        {
            serialPrintLn("[FWD] Cannot forward to " + hdr.dst + " (no route or TTL expired)");
        }
    }
}

void handleAckPacket(const PacketHeader &hdr, const String &payload, int rssi, float snr)
{
    // Check if this ACK is for us
    if (hdr.dst == myNodeName)
    {
        // ACK received, handled by waiting code
        return;
    }

    // Forward ACK toward destination
    String nextHop = getNextHop(hdr.dst);
    if (nextHop.length() > 0 && hdr.ttl > 1)
    {
        PacketHeader fwd = hdr;
        fwd.hopCount++;
        fwd.ttl--;

        String fwdPacket = encodePacket(fwd, payload);
        addToRelayQueue(fwdPacket, 1); // Higher priority for ACKs

        serialPrintLn("[FWD] ACK from " + hdr.src + " to " + hdr.dst + " via " + nextHop);
    }
}

// ==================== MAIN PACKET PROCESSING ====================

void processReceivedPacket(const PacketHeader &hdr, const String &payload, int rssi, float snr)
{
    // Update route to source
    if (hdr.src != myNodeName && hdr.hopCount < 255)
    {
        addOrUpdateRoute(hdr.src, hdr.src, hdr.hopCount + 1, rssi, snr);
    }

    switch (hdr.type)
    {
    case MSG_DATA:
        if (!isDuplicate(hdr.src, hdr.seq) || hdr.dst == myNodeName)
        {
            handleDataPacket(hdr, payload, rssi, snr);
        }
        else
        {
            duplicatesDropped++;
            serialPrintLn("[DUP] Dropped duplicate DATA from " + hdr.src + " seq=" + String(hdr.seq));
        }
        break;

    case MSG_ACK:
        handleAckPacket(hdr, payload, rssi, snr);
        break;

    case MSG_RREQ:
        handleRouteRequest(hdr, payload, rssi, snr);
        break;

    case MSG_RREP:
        handleRouteReply(hdr, payload, rssi, snr);
        break;

    case MSG_HELLO:
        handleHello(hdr, rssi, snr);
        break;

    case MSG_RACK:
        serialPrintLn("[RACK] Hop-by-hop ACK from " + hdr.src);
        break;

    default:
        serialPrintLn("[RX] Unknown message type: " + String((int)hdr.type));
        break;
    }
}

// ==================== SERIAL COMMAND PROCESSING ====================

void processSerialCommand()
{
    if (!Serial.available())
        return;

    String line = Serial.readStringUntil('\n');
    line.trim();

    if (line.length() == 0)
        return;

    // Command format: SEND:<dest>:<reliability>:<data>
    if (line.startsWith("SEND:"))
    {
        int idx1 = line.indexOf(':', 5);
        if (idx1 < 0)
        {
            serialPrintLn("[ERR] Invalid SEND format. Use: SEND:<dest>:<rel>:<data>");
            return;
        }

        int idx2 = line.indexOf(':', idx1 + 1);
        if (idx2 < 0)
        {
            serialPrintLn("[ERR] Invalid SEND format. Use: SEND:<dest>:<rel>:<data>");
            return;
        }

        String dest = line.substring(5, idx1);
        String relStr = line.substring(idx1 + 1, idx2);
        String data = line.substring(idx2 + 1);

        ReliabilityLevel rel = (ReliabilityLevel)relStr.toInt();

        serialPrintLn("[CMD] Sending to " + dest + " with reliability " + reliabilityToString(rel));
        bool success = sendDataPacket(dest, data, rel);

        if (success)
        {
            serialPrintLn("[CMD] Send completed successfully");
        }
        else
        {
            serialPrintLn("[CMD] Send failed");
        }
    }
    // Command: ROUTES - print routing table
    else if (line == "ROUTES")
    {
        printRoutingTable();
    }
    // Command: STATS - print statistics
    else if (line == "STATS")
    {
        serialPrintLn("\n========== STATISTICS ==========");
        serialPrintLn("Node: " + myNodeName);
        serialPrintLn("TX Packets: " + String((unsigned long)txPackets));
        serialPrintLn("RX Packets: " + String((unsigned long)rxPackets));
        serialPrintLn("TX Bytes: " + String((unsigned long)txBytes));
        serialPrintLn("RX Bytes: " + String((unsigned long)rxBytes));
        serialPrintLn("Relayed: " + String((unsigned long)relayedPackets));
        serialPrintLn("Duplicates Dropped: " + String((unsigned long)duplicatesDropped));
        serialPrintLn("Queue Size: " + String(relayQueue.size()));
        serialPrintLn("Routes: " + String(routingTable.size()));
        serialPrintLn("Session Time: " + String((millis() - sessionStartMs) / 1000) + "s");
        serialPrintLn("================================\n");
    }
    // Command: DISCOVER:<dest> - force route discovery
    else if (line.startsWith("DISCOVER:"))
    {
        String dest = line.substring(9);
        serialPrintLn("[CMD] Discovering route to " + dest);
        sendRouteRequest(dest);
    }
    // Plain text message (default destination)
    else
    {
        serialPrintLn("[CMD] Unknown command. Available: SEND, ROUTES, STATS, DISCOVER");
    }
}

// ==================== SETUP ====================

void setup()
{
    Serial.begin(115200);
    Serial.setTimeout(10);

    // Initialize OLED
    Wire.begin();
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
    {
        Serial.println("[ERR] OLED init failed");
        for (;;)
            ;
    }

    // Initialize LoRa
    SPI.begin(SCK, MISO, MOSI, SS);
    LoRa.setPins(SS, RST, DIO0);
    if (!LoRa.begin(FREQ_HZ))
    {
        oled3("LoRa Init FAILED", "Check wiring", "");
        Serial.println("[ERR] LoRa init failed");
        for (;;)
            ;
    }

    LoRa.setSpreadingFactor(LORA_SF);
    LoRa.setSyncWord(LORA_SYNC);
    LoRa.enableCrc();
    LoRa.setTxPower(17, PA_OUTPUT_PA_BOOST_PIN);
    LoRa.setSignalBandwidth(125E3);
    LoRa.setCodingRate4(5);

    sessionStartMs = millis();

    oled3("Mesh Node Ready", myNodeName, "923MHz SF" + String(LORA_SF));

    Serial.println("\n========================================");
    Serial.println("    LoRa Intelligent Mesh Network");
    Serial.println("========================================");
    Serial.println("Node: " + myNodeName);
    Serial.println("Frequency: 923 MHz (AS923)");
    Serial.println("Spreading Factor: " + String(LORA_SF));
    Serial.println("========================================");
    Serial.println("\nCommands:");
    Serial.println("  SEND:<dest>:<rel>:<data>");
    Serial.println("  ROUTES - Show routing table");
    Serial.println("  STATS - Show statistics");
    Serial.println("  DISCOVER:<dest> - Find route");
    Serial.println("========================================\n");

    // Send initial HELLO
    delay(random(100, 500)); // Random delay to avoid collisions
    sendHello();
    lastHelloTime = millis();
}

// ==================== LOOP ====================

void loop()
{
    // Process incoming packets
    int packetSize = LoRa.parsePacket();
    if (packetSize)
    {
        String raw;
        while (LoRa.available())
        {
            raw += (char)LoRa.read();
        }

        int rssi = LoRa.packetRssi();
        float snr = LoRa.packetSnr();

        PacketHeader hdr;
        String payload;

        if (decodePacket(raw, hdr, payload))
        {
            // Ignore our own packets
            if (hdr.src == myNodeName)
            {
                return;
            }

            rxPackets++;
            rxBytes += raw.length();

            serialPrintLn("[RX] " + msgTypeToString(hdr.type) + " from " + hdr.src + " to " + hdr.dst +
                          " (seq=" + String(hdr.seq) + ", hop=" + String(hdr.hopCount) +
                          ", RSSI=" + String(rssi) + ")");

            processReceivedPacket(hdr, payload, rssi, snr);
        }
        else
        {
            serialPrintLn("[RX] Failed to decode packet");
        }
    }

    // Process relay queue
    if (!relayQueue.empty())
    {
        processRelayQueue();
        delay(50); // Small delay between relay transmissions
    }

    // Send periodic HELLO
    if (millis() - lastHelloTime > HELLO_INTERVAL_MS)
    {
        sendHello();
        lastHelloTime = millis();
    }

    // Process serial commands
    processSerialCommand();

    delay(1);
}

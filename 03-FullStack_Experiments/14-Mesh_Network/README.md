# LoRa Intelligent Mesh Network

A sophisticated mesh networking system for LoRa devices with intelligent routing, adaptive reliability, and multimedia support.

## 🌟 Features

### Core Network Capabilities

- **Unified Codebase**: All nodes run identical firmware (only node name differs)
- **Automatic Role Adaptation**: Nodes act as endpoints or relays based on traffic patterns
- **Intelligent Routing**: AODV-inspired routing with automatic route discovery
- **Multi-hop Communication**: Messages automatically relay through intermediate nodes
- **Duplicate Suppression**: Smart deduplication prevents message loops
- **Memory-Efficient Relay**: Lightweight queuing system for relay nodes

### Reliability & Performance

- **Adaptive Reliability Levels**:

  - `NONE` (0): Fire-and-forget, no ACK
  - `LOW` (1): Text, voice (1 retry, 2s timeout)
  - `MEDIUM` (2): Images (2 retries, 5s timeout)
  - `HIGH` (3): Files (3 retries, 8s timeout)
  - `CRITICAL` (4): MiniSEED seismic data (5 retries, 15s timeout)

- **Multi-hop ACK System**: Relay nodes participate in acknowledgment chain
- **Connection Discovery**: Routes established before bulk data transfer
- **ARQ Support**: Stop-and-Wait, Go-Back-N, Selective Repeat

### Data Types Supported

- ✅ Text messages
- ✅ Voice/Audio files (WAV, MP3, OGG)
- ✅ Images (JPEG, PNG, GIF, BMP)
- ✅ MiniSEED seismic data
- ✅ Generic files (PDF, DOC, ZIP, etc.)

## 🔧 Hardware Requirements

- **Microcontroller**: ESP32 T-Display (or compatible)
- **Radio Module**: SX127x LoRa transceiver
- **Display**: SSD1306 OLED (128x64)
- **Frequency**: 923 MHz (AS923 band)

### Wiring (LilyGo T-Display → SX127x)

```
SCK   → GPIO 5
MISO  → GPIO 19
MOSI  → GPIO 27
SS    → GPIO 18
RST   → GPIO 14
DIO0  → GPIO 26
```

## 📦 Installation

### Arduino Firmware

1. **Install Required Libraries** (via Arduino Library Manager):

   ```
   - Adafruit GFX Library
   - Adafruit SSD1306
   - LoRa by Sandeep Mistry
   ```

2. **Configure Node Name**:
   Open `MeshNode.ino` and change the node name at the top:

   ```cpp
   #define NODE_NAME "Node_1"  // Change this for each node
   ```

3. **Upload** to your ESP32 device

4. **Repeat** for each node in your mesh network (Node_1, Node_2, Node_3, etc.)

### Python Interface

1. **Install Dependencies**:

   ```bash
   pip install pyserial
   ```

2. **Make executable** (Linux/Mac):
   ```bash
   chmod +x mesh_network_interface.py
   ```

## 🚀 Usage

### Basic Text Messaging

**Send a text message:**

```bash
python mesh_network_interface.py COM9 --send-text "Hello Node_2!" --dest Node_2
```

**With custom reliability:**

```bash
python mesh_network_interface.py COM9 --send-text "Important message" --dest Node_3 --rel 3
```

### File Transfer

**Send an image:**

```bash
python mesh_network_interface.py COM9 --send-file photo.jpg --dest Node_2
```

**Send MiniSEED data (critical reliability):**

```bash
python mesh_network_interface.py COM9 --send-file seismic_data.mseed --dest Node_4 --rel 4
```

**Send voice recording:**

```bash
python mesh_network_interface.py COM9 --send-file voice.wav --dest Node_3
```

### Network Monitoring

**Monitor all network activity:**

```bash
python mesh_network_interface.py COM9 --monitor
```

**Show routing table:**

```bash
python mesh_network_interface.py COM9 --routes
```

**Show node statistics:**

```bash
python mesh_network_interface.py COM9 --stats
```

**Discover route to node:**

```bash
python mesh_network_interface.py COM9 --discover Node_5
```

### Serial Commands (Direct to Node)

Connect via serial terminal (115200 baud) and use these commands:

```
SEND:<dest>:<rel>:<data>     - Send message
ROUTES                       - Show routing table
STATS                        - Show statistics
DISCOVER:<dest>              - Find route to destination
```

**Examples:**

```
SEND:Node_2:2:Hello there!
ROUTES
STATS
DISCOVER:Node_5
```

## 📡 Protocol Details

### Packet Format

```
T<type>|S<src>|D<dst>|Q<seq>|H<hop>|L<ttl>|R<rel>|:<payload>
```

**Fields:**

- `T`: Message type (DATA, ACK, RREQ, RREP, HELLO, etc.)
- `S`: Source node name
- `D`: Destination node name
- `Q`: Sequence number
- `H`: Hop count
- `L`: Time-to-live (TTL)
- `R`: Reliability level
- `payload`: Message content

### Message Types

| Type  | Code | Description                |
| ----- | ---- | -------------------------- |
| DATA  | 0    | Single-packet data message |
| FRAG  | 1    | Fragment of large message  |
| ACK   | 2    | End-to-end acknowledgment  |
| FACK  | 3    | Fragment acknowledgment    |
| RREQ  | 4    | Route request (discovery)  |
| RREP  | 5    | Route reply                |
| RERR  | 6    | Route error                |
| HELLO | 7    | Neighbor discovery         |
| RACK  | 8    | Relay ACK (hop-by-hop)     |

### Routing Protocol

The system uses an **AODV-inspired** (Ad-hoc On-Demand Distance Vector) routing protocol:

1. **Route Discovery**: When a node needs to send data to an unknown destination:

   - Broadcasts `RREQ` (Route Request)
   - Intermediate nodes forward RREQ and establish reverse routes
   - Destination responds with `RREP` (Route Reply)
   - RREP travels back, establishing forward route

2. **Route Maintenance**:

   - Routes expire after 5 minutes of inactivity
   - `HELLO` messages maintain neighbor awareness (every 30s)
   - `RERR` messages handle broken links

3. **Duplicate Prevention**:
   - Each message has unique `(source, sequence)` identifier
   - Seen messages tracked for 60 seconds
   - Duplicates dropped automatically

### Reliability Mechanism

#### Single-Hop ACK (Low-Medium Reliability)

```
Node_1 → Node_2: DATA
Node_2 → Node_1: ACK
```

#### Multi-Hop ACK (High-Critical Reliability)

```
Node_1 → Relay_A: DATA + wait for RACK
Relay_A → Node_1: RACK (hop-by-hop ACK)
Relay_A → Node_2: DATA + wait for RACK/ACK
Node_2 → Node_1: ACK (end-to-end)
```

### Relay Queue Management

Relay nodes maintain a priority queue:

- **Priority 0**: Route control (RREQ, RREP) - Highest
- **Priority 1**: ACK messages
- **Priority 2**: Data messages - Normal
- **Priority 3**: HELLO messages - Lowest

Queue characteristics:

- Max size: 20 packets
- Max age: 10 seconds
- Oldest/stale packets dropped first

## 🏗️ Network Topology Examples

### Linear Topology

```
Node_1 ←→ Node_2 ←→ Node_3 ←→ Node_4
```

Message from Node_1 to Node_4 routes through Node_2 and Node_3.

### Star Topology

```
         Node_2
            ↕
Node_1 ←→ Hub ←→ Node_3
            ↕
         Node_4
```

Hub node relays all traffic.

### Mesh Topology

```
Node_1 ←→ Node_2
  ↕         ↕
Node_3 ←→ Node_4
```

Multiple routes available, automatic failover.

## 📊 Performance Considerations

### Typical Performance

- **Direct link**: ~1-2 kbps effective throughput
- **One relay**: ~0.5-1 kbps
- **Two relays**: ~0.3-0.7 kbps
- **Range**: 500m - 2km (depending on environment)

### Optimization Tips

1. **Reliability Selection**:

   - Use `REL_LOW` for text/voice (faster, less overhead)
   - Use `REL_CRITICAL` only for MiniSEED/important files
   - Consider trade-off: reliability vs. speed

2. **Fragmentation**:

   - Current chunk size: 150 chars (~200 bytes with headers)
   - Smaller chunks = more reliable but slower
   - Larger chunks = faster but more retries

3. **Network Load**:

   - Limit simultaneous transmissions
   - Stagger HELLO intervals between nodes
   - Monitor relay queue sizes

4. **Radio Settings**:
   - SF7 = faster, shorter range
   - SF12 = slower, longer range
   - Adjust in firmware for your environment

## 🔍 Troubleshooting

### No Route Found

**Problem**: Route discovery fails

```
[TX] No route to Node_X, initiating route discovery
[RREQ] Sent route request for Node_X
[TX] Route discovery failed
```

**Solutions**:

- Check if destination node is powered on
- Verify destination is within range (direct or via relay)
- Increase TTL in route discovery (edit firmware)
- Check for interference or obstacles

### High Packet Loss

**Problem**: Many retries, timeouts

```
[TX] ACK timeout for seq=123
[TX] Retry 2/3
```

**Solutions**:

- Increase reliability level
- Check RSSI values (should be > -120 dBm)
- Reduce transmission power to avoid saturation
- Check for nearby LoRa devices causing collisions

### Relay Queue Full

**Problem**: Relay node dropping packets

```
[QUEUE] Full, dropping packet
```

**Solutions**:

- Reduce network load
- Increase `MAX_RELAY_QUEUE` in firmware
- Add more relay nodes to distribute load
- Optimize route discovery to find shorter paths

### Duplicate Messages

**Problem**: Same message received multiple times

```
[DUP] Dropped duplicate DATA from Node_X seq=456
```

**Note**: This is normal! The duplicate suppression is working correctly. If you see excessive duplicates (>50%), check for:

- Routing loops (shouldn't happen with TTL)
- Multiple relay paths creating echoes
- Very long SEEN_MSG_TIMEOUT

## 📈 Future Enhancements

Potential improvements for advanced users:

1. **Dynamic SF Adjustment**: Automatically adjust spreading factor based on link quality
2. **Multi-path Routing**: Send fragments via different routes for redundancy
3. **Energy Optimization**: Sleep modes, duty cycling for battery nodes
4. **Encryption**: Add AES encryption for secure communications
5. **GPS Integration**: Location-based routing, geofencing
6. **Web Interface**: Real-time network visualization
7. **OTA Updates**: Firmware updates over the mesh network

## 📝 Technical Comparison

### vs. 10-Relay_Experiment

- ✅ Unified codebase (was: separate end node / relay node)
- ✅ Intelligent routing (was: transparent relay)
- ✅ Multi-hop ACK (was: end-to-end only)
- ✅ Duplicate suppression (was: limited)
- ✅ Route discovery (was: manual topology)

### vs. 11-Multimedia_Tunnel

- ✅ Multi-hop support (was: point-to-point)
- ✅ Automatic relaying (was: direct link only)
- ✅ Network-wide operation (was: PC-to-PC tunnel)
- ✅ Adaptive reliability (was: fixed ARQ mode)
- ✅ Route management (was: N/A)

## 🤝 Contributing

This is a research/educational project. Improvements welcome:

- Optimize routing algorithms
- Add new reliability mechanisms
- Improve fragmentation efficiency
- Better queue management
- Enhanced monitoring/debugging tools

## 📄 License

This project is for educational and research purposes. Based on:

- Sandeep Mistry's LoRa library
- Adafruit graphics libraries
- AODV routing protocol concepts

## 🔗 Related Files

- `MeshNode.ino` - Unified node firmware
- `mesh_network_interface.py` - Python interface for multimedia
- Previous experiments:
  - `03-FullStack_Experiments/10-Relay_Experiment` - Basic relay
  - `03-FullStack_Experiments/11-Multimedia_Tunnel` - Point-to-point multimedia
  - `03-FullStack_Experiments/08-Seismic_Stream_v8` - MiniSEED transmission

## 📞 Support

For issues or questions:

1. Check serial monitor for error messages
2. Use `--monitor` to see network activity
3. Use `STATS` and `ROUTES` commands for debugging
4. Review RSSI/SNR values for link quality

---

**Built for reliability, optimized for real-world mesh networking! 🚀**

# Quick Start Guide - LoRa Intelligent Mesh Network

## 🎯 Getting Started in 5 Minutes

### Step 1: Hardware Setup

1. **Connect your LoRa modules** to ESP32 boards following the wiring diagram in README.md
2. **Connect OLED displays** (optional but recommended for status)
3. **Power up** each device via USB

### Step 2: Flash the Firmware

**For EACH node:**

1. Open `MeshNode.ino` in Arduino IDE
2. Change the node name (line ~35):
   ```cpp
   #define NODE_NAME "Node_1"  // Change to Node_2, Node_3, etc.
   ```
3. Select **Board**: "ESP32 Dev Module"
4. Select **Port**: Your COM port
5. Click **Upload** ✓

**Repeat for all nodes with different names!**

### Step 3: Verify Network Operation

**Check the OLED display** on each node - should show:

```
Mesh Node Ready
Node_1
923MHz SF7
```

**Check serial monitor** (115200 baud) - should show:

```
========================================
    LoRa Intelligent Mesh Network
========================================
Node: Node_1
Frequency: 923 MHz (AS923)
Spreading Factor: 7
========================================
```

### Step 4: Test Communication

**Simple test - No Python required:**

1. **On Node_1**, open serial monitor and type:

   ```
   SEND:Node_2:2:Hello Node 2!
   ```

2. **On Node_2**, you should see:
   ```
   [RX] DATA from Node_1 (seq=1, hops=0)
   [RX] Payload: Hello Node 2!
   ```

**Success!** ✓ Your mesh network is working!

---

## 📡 Typical Network Setups

### Setup 1: Simple 2-Node Chat

```
Node_1 ←→ Node_2
```

**Use case**: Direct point-to-point communication

**Setup**:

- Place nodes within ~1 km (line of sight)
- Flash with names Node_1 and Node_2

**Test**:

```bash
# From Node_1's Python interface
python mesh_network_interface.py COM9 --send-text "Hello!" --dest Node_2
```

---

### Setup 2: 3-Node with Relay

```
Node_1 ←→ Node_Relay ←→ Node_2
```

**Use case**: Extended range communication

**Setup**:

- Node_1 and Node_2 are **out of direct range**
- Node_Relay is placed in between

**How it works**:

1. Node_1 sends route request (RREQ)
2. Node_Relay forwards RREQ to Node_2
3. Node_2 replies with RREP through Node_Relay
4. Route established: Node_1 → Node_Relay → Node_2
5. All data automatically relays through Node_Relay

**Test**:

```bash
# Node_1 discovers route automatically
python mesh_network_interface.py COM9 --send-text "Testing relay" --dest Node_2
```

---

### Setup 3: 4-Node Mesh

```
Node_1 ←→ Node_2
  ↕         ↕
Node_3 ←→ Node_4
```

**Use case**: Redundant paths, automatic failover

**Setup**:

- All nodes within range of at least 2 neighbors
- Creates multiple routing options

**Advantages**:

- If one node fails, traffic reroutes automatically
- Load balancing across multiple paths
- Better reliability

---

### Setup 4: Star Network with Central Hub

```
         Node_2
            ↕
Node_1 ←→ Hub ←→ Node_3
            ↕
         Node_4
```

**Use case**: Centralized architecture

**Setup**:

- Hub node has best antenna/position
- All nodes communicate via Hub

**Good for**:

- Gateway to internet/server
- Central data collection
- Monitoring network health

---

## 🚀 Common Tasks

### Task 1: Send a Text Message

**Command line:**

```bash
python mesh_network_interface.py COM9 --send-text "Your message here" --dest Node_2
```

**Serial monitor:**

```
SEND:Node_2:1:Your message here
```

---

### Task 2: Send an Image

**Prepare your image:**

- JPEG or PNG format
- Recommended size: < 500 KB for faster transmission
- Larger files work but take longer

**Send:**

```bash
python mesh_network_interface.py COM9 --send-file photo.jpg --dest Node_3
```

**On receiving node, run:**

```bash
python mesh_receiver.py COM8 --out-dir received_files
```

**Result:**

- File automatically saved in `received_files/photo.jpg`

---

### Task 3: Send MiniSEED Data (Critical Reliability)

**For seismic/scientific data requiring 100% reliability:**

```bash
python mesh_network_interface.py COM9 --send-file seismic.mseed --dest Node_4 --rel 4
```

**What happens:**

- Reliability level 4 = CRITICAL
- 5 retry attempts per chunk
- 15 second timeout per chunk
- Hop-by-hop ACKs through relays
- Will take longer but ensures delivery

---

### Task 4: Monitor Network Activity

**See all network traffic in real-time:**

```bash
python mesh_network_interface.py COM9 --monitor
```

**Output example:**

```
→ [TX] Sent DATA to Node_2 via Node_2 (seq=1, rel=MED)
← [RX] DATA from Node_2 (seq=5, hop=0, RSSI=-45)
↔ [FWD] DATA from Node_1 to Node_3 via Node_3
☆ [ROUTE] Node_2 via Node_2 (1 hops)
✓ [ACK] Received from Node_2 (RSSI=-47)
```

**Symbols:**

- `→` Transmit
- `←` Receive
- `↔` Relay/Forward
- `☆` Route update
- `✓` Acknowledgment
- `✗` Error

---

### Task 5: Check Network Topology

**See all known routes:**

```bash
python mesh_network_interface.py COM9 --routes
```

**Output:**

```
========== ROUTING TABLE ==========
Dest      NextHop   Hops  RSSI  Age(s)
Node_2    Node_2    1     -45   12
Node_3    Node_2    2     -58   45
Node_4    Node_4    1     -52   8
===================================
```

**Interpretation:**

- Node_2: Direct neighbor (1 hop)
- Node_3: Reached via Node_2 (2 hops)
- Node_4: Direct neighbor (1 hop)

---

## ⚙️ Reliability Settings Explained

Choose reliability based on your data type:

### REL_NONE (0) - Fire and Forget

```bash
--rel 0
```

- **No acknowledgments**
- **No retries**
- **Fastest** transmission
- **Use for**: Non-critical beacons, telemetry where some loss is acceptable

### REL_LOW (1) - Text/Voice

```bash
--rel 1
```

- 1 retry attempt
- 2 second timeout
- **Use for**: Chat messages, voice snippets

### REL_MEDIUM (2) - Images (Default)

```bash
--rel 2  # or omit, this is default
```

- 2 retry attempts
- 5 second timeout
- **Use for**: Photos, graphics, non-critical files

### REL_HIGH (3) - Important Files

```bash
--rel 3
```

- 3 retry attempts
- 8 second timeout
- **Use for**: Documents, logs, configuration files

### REL_CRITICAL (4) - MiniSEED/Scientific Data

```bash
--rel 4
```

- 5 retry attempts
- 15 second timeout
- Hop-by-hop ACKs enabled
- **Use for**: Seismic data, research data, mission-critical transfers

---

## 🔧 Troubleshooting Quick Fixes

### "No route to destination"

**Fix 1:** Manually discover route

```bash
python mesh_network_interface.py COM9 --discover Node_X
```

**Fix 2:** Check if destination is powered on and within range

**Fix 3:** Add a relay node between sender and receiver

---

### "High packet loss / many retries"

**Fix 1:** Increase reliability level

```bash
--rel 3  # or --rel 4
```

**Fix 2:** Check RSSI values (in serial monitor)

```
Good:   RSSI > -90 dBm
Fair:   RSSI -90 to -110 dBm
Poor:   RSSI < -110 dBm
```

**Fix 3:** Reduce distance between nodes or add relay

---

### "Relay queue full"

**Fix 1:** Reduce transmission rate / send smaller chunks

**Fix 2:** Add more relay nodes to distribute load

**Fix 3:** Increase queue size in firmware:

```cpp
const size_t MAX_RELAY_QUEUE = 30;  // Default is 20
```

---

### "File transfer stalls"

**Fix 1:** Use receiver script on destination:

```bash
python mesh_receiver.py COM8
```

**Fix 2:** Wait longer - large files take time over LoRa

**Fix 3:** Check for interference (other LoRa devices nearby)

---

## 📊 Performance Expectations

### Direct Link (1 hop)

- **Throughput**: ~1-2 kbps effective
- **Latency**: ~1-2 seconds per packet
- **Range**: 500m - 2km (depending on environment)

**Example times:**

- 1 KB text: ~10 seconds
- 100 KB image: ~15 minutes
- 1 MB file: ~2 hours

### Through 1 Relay (2 hops)

- **Throughput**: ~0.5-1 kbps
- **Latency**: ~2-4 seconds per packet
- Times roughly **2x** slower than direct

### Through 2 Relays (3 hops)

- **Throughput**: ~0.3-0.7 kbps
- **Latency**: ~4-8 seconds per packet
- Times roughly **3x** slower than direct

**Optimization tip**: Position relay nodes to minimize hop count!

---

## 🎓 Next Steps

1. **Experiment with network topologies** - Try different node arrangements
2. **Test range limits** - How far apart can nodes be?
3. **Optimize reliability** - Find best settings for your use case
4. **Monitor network health** - Use `--monitor` to watch traffic patterns
5. **Scale up** - Add more nodes, create larger mesh

---

## 📞 Need Help?

**Debug checklist:**

- [ ] Correct NODE_NAME flashed to each device?
- [ ] All nodes powered on and OLED showing "Ready"?
- [ ] Correct COM port in Python commands?
- [ ] Destination node name spelled correctly?
- [ ] Nodes within range (check RSSI in serial monitor)?

**Still stuck?**

- Check serial monitor on both sender and receiver
- Use `ROUTES` command to verify topology
- Use `STATS` command to check for errors
- Try `--monitor` to see what's happening

---

**Happy meshing! 🎉**

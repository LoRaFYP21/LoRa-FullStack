# Configuration Examples & Advanced Usage

## 📝 Example Configurations

### Example 1: Text Messaging Network (Low Bandwidth)

**Scenario**: Simple chat between 3 nodes

**Network:**

```
Office ←→ Lab ←→ Field
```

**Node Names:**

- `Node_Office`
- `Node_Lab`
- `Node_Field`

**Configuration (MeshNode.ino):**

```cpp
// For office node
#define NODE_NAME "Node_Office"

// For lab node
#define NODE_NAME "Node_Lab"

// For field node
#define NODE_NAME "Node_Field"
```

**Usage:**

```bash
# Send quick message with low reliability
python mesh_network_interface.py COM9 --send-text "Lunch time!" --dest Node_Lab --rel 1

# Chat mode with monitoring
python mesh_receiver.py COM9
```

**Best settings:**

- Reliability: LOW (1)
- Short messages: < 150 chars (single packet)
- Expected speed: ~10-20 messages/minute

---

### Example 2: Image Transfer Network (Medium Bandwidth)

**Scenario**: Wildlife camera network sending photos to base station

**Network:**

```
Camera_1 ←→ Relay_A ←→ Base
Camera_2 ←→ Relay_B ←→ Base
```

**Node Names:**

- `Camera_1`, `Camera_2` (endpoint cameras)
- `Relay_A`, `Relay_B` (dedicated relays)
- `Base` (collection point)

**Best practices:**

- Compress images to < 500 KB before sending
- Use JPEG format (smaller than PNG)
- Send during low-activity periods

**Sending:**

```bash
# From Camera_1
python mesh_network_interface.py COM9 --send-file photo_001.jpg --dest Base --rel 2
```

**Receiving (at Base):**

```bash
python mesh_receiver.py COM8 --out-dir wildlife_images
```

**Expected performance:**

- 100 KB image: ~15-20 minutes
- 500 KB image: ~60-90 minutes
- Success rate: 95-99% with REL_MEDIUM

---

### Example 3: Seismic Monitoring Network (Critical Reliability)

**Scenario**: Remote seismic sensors sending MiniSEED data with 100% reliability

**Network:**

```
Sensor_1 ─┐
Sensor_2 ─┼─→ Gateway ←→ Base_Station
Sensor_3 ─┘
```

**Node Names:**

- `Sensor_1`, `Sensor_2`, `Sensor_3`
- `Gateway` (aggregation point)
- `Base_Station` (data processing)

**Configuration adjustments for critical data:**

**In MeshNode.ino (optional tuning):**

```cpp
// Increase timeout for critical reliability
unsigned long getAckTimeout(ReliabilityLevel rel) {
  switch(rel) {
    case REL_CRITICAL: return 20000;  // 20s instead of 15s
    // ... rest of code
  }
}

// Increase max retries
int getMaxRetries(ReliabilityLevel rel) {
  switch(rel) {
    case REL_CRITICAL: return 7;  // 7 instead of 5
    // ... rest of code
  }
}
```

**Sending MiniSEED:**

```bash
python mesh_network_interface.py COM9 --send-file seismic_24h.mseed --dest Base_Station --rel 4
```

**Expected performance:**

- 1 MB MiniSEED: ~3-5 hours
- Success rate: 99.9% with REL_CRITICAL
- Data integrity: CRC-checked at every hop

---

### Example 4: Mixed-Use Mesh (Text + Images + Data)

**Scenario**: Research station with multiple data types

**Network:**

```
Weather_Station ─┐
Soil_Sensor ─────┼─→ Hub ←→ Base_Lab
Camera_1 ────────┤
Camera_2 ────────┘
```

**Data types:**

- Weather telemetry: Every 5 minutes (REL_LOW)
- Soil data: Every hour (REL_MEDIUM)
- Camera images: On motion (REL_MEDIUM)
- Daily summary: End of day (REL_HIGH)

**Python automation script example:**

```python
#!/usr/bin/env python3
import time
from mesh_network_interface import MeshNetworkInterface

# Connect to node
interface = MeshNetworkInterface('COM9')
interface.connect()

while True:
    # Send weather data (quick, low reliability)
    weather = read_weather_sensors()  # Your function
    interface.send_text('Base_Lab',
                       f"WEATHER:{weather}",
                       reliability=1)

    time.sleep(300)  # Wait 5 minutes

    # Every hour, send soil data
    if time.localtime().tm_min == 0:
        soil = read_soil_sensors()
        interface.send_text('Base_Lab',
                           f"SOIL:{soil}",
                           reliability=2)

    # Every 6 hours, send image
    if time.localtime().tm_hour % 6 == 0:
        interface.send_file('Base_Lab',
                          'latest_image.jpg',
                          reliability=2)

interface.disconnect()
```

---

## 🔧 Advanced Firmware Tuning

### Tuning 1: Adjust Spreading Factor for Range vs. Speed

**Default: SF7 (faster, shorter range)**

**For longer range (slower):**

```cpp
#define LORA_SF   10  // Change from 7 to 10
```

**Effect:**

- SF7: ~2 km range, ~5 kbps airtime
- SF10: ~8 km range, ~1 kbps airtime
- SF12: ~15 km range, ~0.3 kbps airtime

**When to use high SF:**

- Rural/remote deployments
- Obstacles between nodes
- Lower antenna quality

---

### Tuning 2: Adjust TX Power

**Default: 17 dBm**

**For maximum range:**

```cpp
LoRa.setTxPower(20, PA_OUTPUT_PA_BOOST_PIN);  // Max power
```

**For saving battery:**

```cpp
LoRa.setTxPower(10, PA_OUTPUT_PA_BOOST_PIN);  // Low power
```

**Power levels:**

- 10 dBm: ~500m range, lowest power
- 17 dBm: ~2 km range, good balance
- 20 dBm: ~4 km range, maximum power

---

### Tuning 3: Adjust Fragment Size

**Default: 150 chars per chunk**

**For better reliability (slower):**

```python
# In mesh_network_interface.py
CHUNK_SIZE = 100  # Smaller chunks
```

**For faster transfer (more retries):**

```python
CHUNK_SIZE = 200  # Larger chunks
```

**Trade-off:**

- Smaller chunks: More reliable, more overhead, slower
- Larger chunks: Faster if link is good, more retries if link is poor

---

### Tuning 4: Relay Queue Size

**Default: 20 packets**

**For high-traffic relay nodes:**

```cpp
const size_t MAX_RELAY_QUEUE = 50;  // Increase queue
```

**Effect:**

- Larger queue: Can buffer more messages during bursts
- Cost: Uses more RAM on ESP32

---

### Tuning 5: Route Timeout

**Default: 5 minutes**

**For static networks:**

```cpp
const unsigned long ROUTE_TIMEOUT_MS = 600000;  // 10 minutes
```

**For mobile nodes:**

```cpp
const unsigned long ROUTE_TIMEOUT_MS = 120000;  // 2 minutes
```

**Effect:**

- Longer timeout: Less route discovery overhead
- Shorter timeout: Adapts faster to topology changes

---

## 🎯 Use Case Specific Configs

### Config A: Emergency Communication Network

**Requirements:**

- High reliability
- Low latency for alerts
- Mixed message priorities

**Settings:**

```cpp
// MeshNode.ino
#define LORA_SF   9  // Good range + acceptable speed

// Adjust priority queue
const size_t MAX_RELAY_QUEUE = 30;
```

**Python usage:**

```bash
# High priority alert
python mesh_network_interface.py COM9 --send-text "ALERT: Emergency!" --dest All_Nodes --rel 3

# Normal message
python mesh_network_interface.py COM9 --send-text "Status OK" --dest Base --rel 1
```

---

### Config B: Solar-Powered Remote Sensors

**Requirements:**

- Minimize power consumption
- Infrequent transmissions
- Maximum battery life

**Settings:**

```cpp
// MeshNode.ino
#define LORA_SF   12  // Longest range at lowest power

// Reduce TX power
LoRa.setTxPower(14, PA_OUTPUT_PA_BOOST_PIN);

// Reduce HELLO frequency
const unsigned long HELLO_INTERVAL_MS = 300000;  // 5 minutes

// Sleep between transmissions (add deep sleep code)
```

**Estimated battery life:**

- Without sleep: ~2-3 days on 2000mAh
- With deep sleep: ~2-3 weeks on 2000mAh

---

### Config C: High-Throughput Gateway

**Requirements:**

- Maximum speed
- Close range (< 500m)
- Low latency

**Settings:**

```cpp
// MeshNode.ino
#define LORA_SF   7  // Fastest speed

// Increase bandwidth
LoRa.setSignalBandwidth(250E3);  // 250 kHz instead of 125 kHz

// Reduce fragment spacing
const unsigned long FRAG_SPACING_MS = 10;  // 10ms instead of 20ms
```

**Expected improvement:**

- ~2x faster throughput
- Reduced range (~50%)

---

## 🧪 Testing & Validation

### Test 1: Range Test

**Objective:** Find maximum distance between nodes

**Method:**

1. Start with nodes close together
2. Gradually increase distance
3. Send test messages every 50m
4. Record RSSI at each distance

**Commands:**

```bash
# Sender
python mesh_network_interface.py COM9 --send-text "TEST 50m" --dest Node_2 --rel 2

# Monitor receiver
python mesh_network_interface.py COM8 --monitor
```

**Look for:**

- RSSI values in serial monitor
- Success rate of transmissions
- Maximum reliable distance

---

### Test 2: Throughput Test

**Objective:** Measure effective data rate

**Method:**

```bash
# Send known-size file and time it
time python mesh_network_interface.py COM9 --send-file test_1MB.dat --dest Node_2 --rel 2
```

**Calculate:**

```
Throughput (bps) = File size (bits) / Time (seconds)
```

---

### Test 3: Relay Performance Test

**Objective:** Evaluate relay node performance

**Setup:**

```
Node_A ←[far]→ Relay ←[far]→ Node_B
```

**Method:**

1. Send 10 messages from Node_A to Node_B
2. Monitor Relay serial output
3. Check queue statistics

**Commands:**

```bash
# At Relay node
STATS
```

**Look for:**

- Queue size: Should stay < 5 for good performance
- Relayed packets: Should match sent messages
- Duplicates dropped: Should be low

---

## 📊 Performance Optimization Matrix

| Use Case  | SF  | BW     | Power | Reliability | Expected Range | Speed     |
| --------- | --- | ------ | ----- | ----------- | -------------- | --------- |
| Chat      | 7   | 125kHz | 17dBm | LOW         | 1-2 km         | Fast      |
| Images    | 8   | 125kHz | 17dBm | MEDIUM      | 2-4 km         | Medium    |
| Files     | 9   | 125kHz | 17dBm | HIGH        | 4-6 km         | Slow      |
| Seismic   | 10  | 125kHz | 20dBm | CRITICAL    | 6-10 km        | Very Slow |
| Emergency | 8   | 125kHz | 20dBm | HIGH        | 3-5 km         | Medium    |

---

## 🔒 Security Considerations

### Current State

- **No encryption** in base implementation
- All messages transmitted in plain text
- Any node can join network

### To Add Encryption (Advanced)

**Option 1: AES encryption at application layer**

```cpp
// In Python interface, encrypt before sending
import cryptography
# ... encrypt data before base64 encoding
```

**Option 2: Add authentication**

```cpp
// In MeshNode.ino, add node authentication
bool authenticateNode(const String &nodeName, const String &token) {
    // Check if node is authorized
}
```

**Recommendation:**

- For sensitive data, implement encryption at Python layer
- Use strong keys (AES-256)
- Implement key rotation

---

## 💡 Tips & Tricks

### Tip 1: Pre-discover Routes

```bash
# Before sending large file, discover route first
python mesh_network_interface.py COM9 --discover Node_5

# Wait for route to establish (5-10 seconds)

# Then send file
python mesh_network_interface.py COM9 --send-file data.bin --dest Node_5
```

### Tip 2: Batch Small Messages

Instead of sending many tiny messages, batch them:

```python
# Bad: Many small sends (lots of overhead)
for i in range(10):
    send_text(f"Sensor_{i}: {value}")

# Good: One batched send
batch = "; ".join([f"S{i}:{val}" for i, val in enumerate(values)])
send_text(batch)
```

### Tip 3: Schedule Transmissions

Avoid network congestion:

```python
# Stagger transmissions from different nodes
node_1_schedule = [0, 10, 20, 30, 40, 50]  # Minutes
node_2_schedule = [5, 15, 25, 35, 45, 55]  # Minutes (offset)
```

### Tip 4: Monitor Link Quality

```bash
# Use HELLO messages to track neighbor quality
python mesh_network_interface.py COM9 --monitor

# Look for:
# [HELLO] Neighbor Node_2 (RSSI=-45, SNR=8.5)
# Good: RSSI > -90, SNR > 5
# Fair: RSSI -90 to -110, SNR 0 to 5
# Poor: RSSI < -110, SNR < 0
```

---

**That's it! You now have comprehensive configuration options for any deployment scenario! 🎉**

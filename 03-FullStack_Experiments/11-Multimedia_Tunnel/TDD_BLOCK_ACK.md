# TDD-Aware Block ACK ARQ Implementation

## Overview

This implementation adds a **TDD-Aware Block ACK** mechanism to the LoRa communication system, optimizing for Time Division Duplex (TDD) constraints by using burst transmission and aggregated acknowledgments.

## Key Concepts

### Traditional ARQ Limitations

- **Stop-and-Wait**: Sends one packet, waits for ACK → Very slow
- **Go-Back-N**: Pipeline but retransmit entire window on loss → Wasteful
- **Selective Repeat**: Better but still sends individual ACKs → ACK overhead

### TDD-Aware Block ACK Solution

- **Burst Transmission**: Sender transmits multiple packets continuously during downlink slot
- **Aggregated Feedback**: Receiver sends ONE Block ACK with bitmap during uplink slot
- **Selective Retransmission**: Only lost packets are retransmitted based on bitmap

---

## Architecture

### Three-Step Process

#### Step 1: Burst Transmission (Downlink Slot)

```
Sender → [Pkt#1][Pkt#2][Pkt#3]...[Pkt#64] → Receiver
         (Continuous transmission, no waiting)
```

- Sender transmits up to **64 packets** in rapid succession
- Minimal 10ms spacing between packets (just enough to avoid collisions)
- No individual ACK waits during burst
- Receiver buffers all incoming packets

#### Step 2: Buffering & Reordering (Receiver)

- Receiver maintains a buffer tracking which packets arrived:
  ```
  received[0] = true   // Packet #0 arrived
  received[1] = false  // Packet #1 lost
  received[2] = true   // Packet #2 arrived
  ...
  ```

#### Step 3: Aggregated Feedback (Uplink Slot)

```
Receiver → BACK,src,dst,seq,startIdx,bitmap → Sender
```

**Block ACK Format**: `BACK,src,dst,seq,startIdx,bitmap`

- **src**: Receiver ID
- **dst**: Sender ID
- **seq**: Sequence number of the message
- **startIdx**: Starting fragment index of this burst (e.g., 0, 64, 128)
- **bitmap**: Binary string showing received packets
  - `"1"` = Packet received successfully
  - `"0"` = Packet lost/not received
  - Example: `"1101111..."` means packets 0,2,3,4,5,6 received, packet 1 lost

---

## Implementation Details

### Configuration Parameters

```cpp
const size_t TDD_BURST_SIZE = 64;                    // Packets per burst
const unsigned long TDD_BLOCK_ACK_TIMEOUT_MS = 6000; // Wait time for Block ACK
```

### Sender Logic (TX Side)

1. **Initialize**: Set all fragments as not-ACKed
2. **Loop through bursts**:
   - Calculate burst range: `[base, base+64)`
   - **Transmit burst**: Send all packets in range continuously
   - **Wait for Block ACK**: Listen for bitmap response
   - **Process bitmap**: Mark ACKed packets based on '1's in bitmap
   - **Selective retransmit**: Resend only packets marked '0'
   - **Verify**: Check all packets ACKed, otherwise retry
   - **Advance**: Move to next burst window

3. **Retransmission Strategy**:
   - Only retransmit packets marked as '0' in bitmap
   - Maximum 3 retries per packet
   - Fail entire transmission if any packet exceeds retry limit

### Receiver Logic (RX Side)

1. **Buffer Management**:

   ```cpp
   struct BlockAckBuffer {
       uint32_t seq;              // Current sequence number
       String srcId;              // Sender ID
       size_t total;              // Total fragments expected
       bool received[512];        // Bitmap of received packets
       unsigned long lastUpdate;  // Last packet time
       size_t lastBurstStart;    // Last burst start index
       bool active;              // Buffer in use
   };
   ```

2. **Fragment Reception**:
   - When MSGF arrives, mark `received[idx] = true`
   - Detect burst completion by:
     - Timing gap (>100ms since last packet)
     - Burst size reached (64 packets)
     - Last packet received

3. **Block ACK Generation**:
   - Build bitmap string for current burst window
   - Send single `BACK` packet with complete bitmap
   - Format: `BACK,myId,senderId,seq,burstStart,bitmap`

---

## Timing Behavior

### Downlink Slot (Burst Transmission)

```
TX: [Pkt0] --10ms-- [Pkt1] --10ms-- [Pkt2] ... [Pkt63]
    └──────────────── Continuous stream ────────────────┘
                    (~1-2 seconds total)
```

### Uplink Slot (Block ACK)

```
RX: Waits 20ms → Generates bitmap → Sends BACK packet
                                    (Single transmission)
```

### Advantages Over Individual ACKs

| Metric                       | Individual ACK       | Block ACK         |
| ---------------------------- | -------------------- | ----------------- |
| ACK packets for 64 fragments | 64                   | 1                 |
| ACK overhead                 | ~64 × 50ms = 3.2s    | ~50ms             |
| Airtime efficiency           | Low                  | High              |
| Retransmission               | Per packet or window | Only lost packets |

---

## Example Scenario

### Sending 128 Fragments

#### Burst 1 (Fragments 0-63):

1. TX sends packets 0-63 continuously (~1.5s)
2. RX receives: 0,1,2,4,5,6,7,8... (packet 3 lost)
3. RX sends Block ACK: `BACK,RX_ID,TX_ID,42,0,1110111111...`
4. TX sees '0' at position 3 → retransmits only packet 3
5. RX confirms packet 3 → sends updated Block ACK: `BACK,RX_ID,TX_ID,42,0,1111111111...`

#### Burst 2 (Fragments 64-127):

1. TX sends packets 64-127 continuously
2. RX receives all → sends Block ACK: `BACK,RX_ID,TX_ID,42,64,1111111111...`
3. All ACKed → transmission complete

**Total**: 2 bursts, 2-4 Block ACKs vs 128 individual ACKs

---

## Code Structure

### Arduino (.ino) Changes

1. **New ARQ mode**: `ARQ_TDD_BLOCK_ACK = 3`
2. **New parser**: `parseBACK()` - Extracts bitmap from Block ACK
3. **New TX function**: `sendFragmentsTDDBlockAck()` - Burst + Block ACK handling
4. **New RX buffer**: `rxBlockBuffer` - Tracks received packets and generates bitmaps
5. **Updated RX logic**: Detects bursts and sends aggregated Block ACKs
6. **Updated processing**: `processIncomingWhileTx()` now handles BACK packets

### Python Changes (Not Required for Basic Operation)

The Python side (`lora_transceiver.py` and `gui_app.py`) doesn't need changes because:

- They only receive `MSG` and `FRAG` lines from MCU (unchanged)
- Block ACK (`BACK`) is handled internally between LoRa nodes
- Reassembly logic remains the same

However, you could add logging to see Block ACK messages if desired.

---

## Usage

### Enable TDD Block ACK Mode

In [11-Multimedia_Tunnel.ino](11-Multimedia_Tunnel.ino#L38), set:

```cpp
ArqMode gArqMode = ARQ_TDD_BLOCK_ACK;
```

### Tune Burst Size

Adjust based on your TDD slot duration and packet size:

```cpp
const size_t TDD_BURST_SIZE = 64;  // Increase for longer slots
```

### Monitor Operation

Serial output shows:

```
[TDD-BACK] seq=42 totalFrags=128 burstSize=64
  [TDD DOWNLINK] Burst transmission: frags 0-63 (64 packets)
    [TDD TX FRAG 1/128]
    [TDD TX FRAG 2/128]
    ...
  [TDD BURST COMPLETE] Waiting for Block ACK...
  [RX BACK] seq=42 startIdx=0 bitmap=110111... from=ABC123DEF456
  [TDD RETRANSMIT] Resending 2 lost packets
    [TDD RE-TX FRAG 3/128] (retry 2)
    [TDD RE-TX FRAG 10/128] (retry 2)
  [TDD BLOCK ACK OK] Burst 0-63 fully ACKed
  [TDD DOWNLINK] Burst transmission: frags 64-127 (64 packets)
  ...
[TX DONE] #42 mode=TDD-BACK all fragments ACKed.
```

---

## Performance Characteristics

### Strengths

✅ **Minimal ACK overhead**: 1 ACK per burst instead of per packet  
✅ **Fast burst transmission**: No waiting between packets  
✅ **Selective retransmission**: Only lost packets resent  
✅ **TDD-optimized**: Matches duplex timing constraints  
✅ **Scalable**: Handles large messages efficiently

### Considerations

⚠️ **Burst loss**: If Block ACK is lost, entire burst retransmits  
⚠️ **Buffer size**: Receiver needs RAM for `MAX_FRAGMENTS` bitmap  
⚠️ **Timing sensitivity**: Relies on burst boundary detection

### Best Use Cases

- Long file transfers (images, audio)
- High packet loss environments (saves retransmissions)
- TDD radio systems with defined uplink/downlink slots
- Scenarios where ACK overhead dominates airtime

---

## Comparison: All ARQ Modes

| Mode                 | Throughput | Reliability | Complexity | Use Case                  |
| -------------------- | ---------- | ----------- | ---------- | ------------------------- |
| **Stop-and-Wait**    | ⭐         | ⭐⭐⭐      | ⭐         | Small messages, testing   |
| **Go-Back-N**        | ⭐⭐       | ⭐⭐        | ⭐⭐       | Moderate loss, pipelining |
| **Selective Repeat** | ⭐⭐⭐     | ⭐⭐⭐      | ⭐⭐⭐     | High loss, efficiency     |
| **TDD Block ACK**    | ⭐⭐⭐⭐   | ⭐⭐⭐      | ⭐⭐⭐⭐   | Large files, TDD systems  |

---

## Testing Recommendations

1. **Start small**: Test with 10-20 fragments first
2. **Monitor bitmaps**: Check Serial output for bitmap patterns
3. **Simulate loss**: Temporarily reduce TX power to test retransmission
4. **Compare modes**: Benchmark against GBN and SR for your use case
5. **Tune burst size**: Adjust `TDD_BURST_SIZE` based on slot duration

---

## Future Enhancements

1. **Dynamic burst sizing**: Adjust based on channel quality
2. **Cumulative ACK**: Option for "ACK up to index X" instead of full bitmap
3. **NACK-only bitmap**: Send only indices of lost packets (smaller payload)
4. **Adaptive retry**: Reduce retries for consistently good links
5. **Burst priority**: Send critical bursts first

---

## References

- **TDD Principle**: Match ARQ to Time Division Duplex slot timing
- **Block ACK Concept**: Aggregate multiple ACKs into single response
- **Selective Retransmission**: Only retransmit packets marked as lost

**Implementation Date**: January 2026  
**Compatible with**: LoRa SX127x, ESP32, AS923 frequency

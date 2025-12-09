# Go-Back-N ARQ Technical Implementation Details

## Protocol Overview

### Message Structure (Unchanged)
```
MSG:    MSG,<src>,<dst>,<seq>,<text>
MSGF:   MSGF,<src>,<dst>,<seq>,<idx>,<tot>,<chunk>
ACK:    ACK,<src>,<dst>,<seq>,<rxBytes>,<rxPkts>
ACKF:   ACKF,<src>,<dst>,<seq>,<idx>
```

### Receiver Behavior (Unchanged)
1. Receives fragments out of order
2. Accumulates them in reassembly buffer
3. Sends ACKF with **highest consecutive fragment index** received
4. Reassembles and delivers complete message

**Key Point**: ACKF now carries **cumulative** semantics instead of just acknowledging the specific fragment.

---

## Transmitter State Machine

### Go-Back-N Transmission Flow

```
1. PREPARATION PHASE
   ├─ Receive user input (message)
   ├─ Break into fragments (if needed)
   └─ Store all in gbnWindow.fragments[]

2. WINDOW TRANSMISSION PHASE
   ├─ While (gbnWindow.base < gbnWindow.tot):
   │  ├─ FOR each position in window (0 to windowSize-1):
   │  │  ├─ Calculate actual fragment index: base + position
   │  │  ├─ IF not yet sent:
   │  │  │  ├─ Send fragment
   │  │  │  ├─ Mark sent[i] = true
   │  │  │  └─ Record sendTime[i] = millis()
   │  │  └─ Delay GBN_FRAG_SPACING_MS
   │  │
   │  └─ WAIT FOR ACKs
   │     ├─ IF (allSent()):
   │     │  ├─ Call waitForWindowAck()
   │     │  ├─ IF successful:
   │     │  │  └─ Break (all ACKed)
   │     │  └─ ELSE (timeout):
   │     │     ├─ Log TIMEOUT_WINDOW
   │     │     ├─ Reset sent[] flags
   │     │     └─ Retransmit from gbnWindow.base
   │     └─ Delay 10ms

3. FINALIZATION PHASE
   ├─ Wait for final message ACK
   ├─ Calculate PDR and throughput
   └─ Return success/failure
```

---

## ACK Processing (Cumulative ACK Handler)

### waitForWindowAck() Logic

```cpp
while (time < deadline) {
  // Timeout check
  foreach fragment in window:
    if (sent[i] && (now - sendTime[i]) > GBN_ACK_TIMEOUT_MS):
      return false;  // Timeout - trigger go-back-N
  
  // Receive processing
  if (packet received):
    if (ACKF packet):
      if (seq == expectSeq):
        // CUMULATIVE ACK PROCESSING
        last_acked = idx  // fragment index
        if (last_acked >= base && last_acked < tot):
          base = last_acked + 1  // Slide window
          if (base >= tot):
            return true;  // All ACKed
    else if (MSG/MSGF incoming):
      // Process inbound while waiting for ACKs
      (add to reassembly, send ACKF)
}
```

### Window Sliding Example

```
Initial State: base=0, tot=4, window=[0,1,2,3]
  Sent: 0✓ 1✓ 2✓ 3✓ | Waiting...

Receive ACKF for fragment 0:
  base = 0 + 1 = 1
  Window slides: [1,2,3,4] (but tot=4, so really [1,2,3])

Receive ACKF for fragment 2 (out of order):
  No action (2 >= base=1 but not contiguous advancement needed yet)

Receive ACKF for fragment 1:
  base = 1 + 1 = 2
  Window slides: [2,3]

Receive ACKF for fragment 3:
  base = 3 + 1 = 4
  base >= tot → All ACKed ✓
```

---

## Timeout and Retransmission

### Scenario: Packet Loss in Window

```
Timeline:
  t=0ms:    Send fragments 0,1,2,3
  t=2000ms: ACK timeout (none received)
  
  Action:
  1. Log: TIMEOUT_WINDOW, seq=X, base=0, tot=4
  2. Reset: sent[0]=false, sent[1]=false, sent[2]=false, sent[3]=false
  3. Continue outer loop: resend fragments 0,1,2,3
  4. Wait again for ACKs
```

### Multi-fragment Timeout Handling

```
If timeout after 3 window retries (GBN_ACK_TIMEOUT_MS * 3):
  → Mark message failed
  → Retry entire message (MSG_MAX_TRIES = 3)
  → Log RETRY_MSG
  → If MSG_MAX_TRIES exceeded:
     → Log ABORT
     → Display: "SEND FAILED after retries"
```

---

## Memory Layout

### GBN Window Structure (Stack allocation)

```
GBNWindow gbnWindow (global):
├─ seq: long (4 bytes)
├─ tot: long (4 bytes)
├─ base: long (4 bytes)
├─ sendTime[4]: unsigned long (16 bytes)    // 4 timestamps
├─ fragments[4]: String (80+ bytes)          // 4 fragment payloads
├─ sent[4]: bool (4 bytes)                   // 4 sent flags
└─ Methods: reset(), windowSize(), allSent(), windowEmpty()

Total: ~116+ bytes (fragments variable size)
```

**Note**: Fragment strings stored for retransmission. Memory freed when message completes via `gbnWindow.reset()`.

---

## Fragment Indexing

### Window Position to Fragment Index Mapping

```
gbnWindow.base = 0, windowSize = 4
  Position 0 → Fragment 0
  Position 1 → Fragment 1
  Position 2 → Fragment 2
  Position 3 → Fragment 3

gbnWindow.base = 2, windowSize = 4
  Position 0 → Fragment 2
  Position 1 → Fragment 3
  Position 2 → Fragment 4 (if tot >= 5)
  Position 3 → Fragment 5 (if tot >= 6)

Calculation: fragIdx = gbnWindow.base + i
```

---

## CSV Event Logging

### New Events Added

| Event | Role | Meaning |
|-------|------|---------|
| TIMEOUT_WINDOW | TX | ACK timeout on current window |
| WAIT_ACKF_START | TX | Began waiting for window ACKs (with base, tot) |
| WAIT_ACKF_OK | TX | Window fully ACKed |

### Enhanced Event Format
CSV columns: `nodeId,role,event,seq,idx,tot,bytes,rssi,snr,toa_ms,t_ms,dt_ms`

**Window-related events log:**
- `seq`: Message sequence number
- `idx`: Current window base (first unACKed fragment)
- `tot`: Total fragments in message
- `bytes`: 0 (not applicable for timing events)

Example:
```
TIM,ABCD1234EF56,TX,TIMEOUT_WINDOW,5,0,4,0,-,-,0,45230,120
TIM,ABCD1234EF56,TX,WAIT_ACKF_START,5,0,4,0,-,-,0,45250,20
TIM,ABCD1234EF56,RX,ACKF_RX,5,2,-1,19,19,-91,-0.5,45320,70
```

---

## Performance Improvements

### Comparison: Stop-and-Wait vs Go-Back-N

**Example: 4-fragment message, 923 MHz, SF8, 200B chunks**

#### Stop-and-Wait
```
Fragment ToA ≈ 100ms each
Delay between fragments ≈ 50ms (wait for ACK + processing)

Timeline:
t=0:    Send Frag0 (0-100ms)
t=150:  Recv ACK0, Send Frag1 (150-250ms)
t=300:  Recv ACK1, Send Frag2 (300-400ms)
t=450:  Recv ACK2, Send Frag3 (450-550ms)
t=600:  Recv ACK3
Total: ~600ms
```

#### Go-Back-N (window=4)
```
Fragment ToA ≈ 100ms each
Delay between fragments ≈ 20ms

Timeline:
t=0:    Send Frag0 (0-100ms)
t=20:   Send Frag1 (20-120ms)
t=40:   Send Frag2 (40-140ms)
t=60:   Send Frag3 (60-160ms)
t=180:  Recv ACKs...
Total: ~250ms (63% faster!)
```

**Benefits on longer messages:**
- 10 fragments: 3-4x faster on low-loss channels
- More robust to channel delay variations
- Better radio utilization

---

## Debugging Tips

### Monitoring Window Progress
```
From CSV timing_data.csv:
1. Look for WAIT_ACKF_START events → shows window base & tot
2. Look for ACKF_RX events → shows which fragments acknowledged
3. Watch base increment in WAIT_ACKF_START → sliding window
4. TIMEOUT_WINDOW → indicates loss, retransmit coming
```

### Testing Window Size
Current: `GBN_WINDOW_SIZE = 4`

To increase throughput on good channels:
```cpp
const int GBN_WINDOW_SIZE = 8;  // Allows 8 unACKed fragments
```

To reduce latency sensitivity:
```cpp
const int GBN_WINDOW_SIZE = 2;  // More conservative
```

### Verifying Cumulative ACKs
In `waitForWindowAck()`, check:
```
if (idx >= gbnWindow.base && idx < gbnWindow.tot) {
  gbnWindow.base = idx + 1;  // This is cumulative ACK
}
```
- ACK for fragment 2 moves base from 0 → 3 (all 0,1,2 assumed ACKed)
- Out-of-order ACKs handled correctly (ignored if idx < base)

---

## Packet Size Impact

### Fragment Size: 200 bytes
```
Overhead per MSGF packet:
  "MSGF," + src(12) + "," + dst(2) + "," + seq(up to 8) + "," 
  + idx(up to 3) + "," + tot(up to 3) + "," + chunk(200)
  
  ≈ 50 bytes overhead + 200 data = 250 bytes typical
  ≈ 80% efficiency per fragment
```

### Adjusting Fragment Size
Current: `FRAG_CHUNK = 200`

For shorter messages:
```cpp
const size_t FRAG_CHUNK = 100;  // More granular, more ACKs
```

For longer messages:
```cpp
const size_t FRAG_CHUNK = 400;  // Fewer fragments, fewer ACKs
```

---

## References

### ARQ Algorithms
- **Stop-and-Wait**: Simplest, RTT-limited
- **Go-Back-N**: This implementation, pipelined with cumulative ACKs
- **Selective Repeat**: Advanced, can ACK individual packets

### LoRa Timing
- SF8 @ 125kHz: ~100ms per 250-byte packet
- AS923 regional settings maintained
- ToA calculation based on LoRa spec

### Implementation Patterns
- Window management: Circular buffer simulation with base pointer
- Cumulative ACK: Only advancing base when highest consecutive received
- Timeout: Per-window rather than per-fragment

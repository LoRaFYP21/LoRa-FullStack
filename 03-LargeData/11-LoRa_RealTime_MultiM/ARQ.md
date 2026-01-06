# Common ground (applies to all 3)

### Hardware / radio facts

- LoRa is **half-duplex**

  - While `sendLoRa()` is transmitting → **RX is deaf**

- After TX finishes → radio returns to **RX**
- RX is **polled**, not interrupt-driven (`LoRa.parsePacket()`)

---

### Common timing constants (used everywhere)

| Constant              | Value   | Meaning                                |
| --------------------- | ------- | -------------------------------------- |
| `RX_ACK_DELAY_MS`     | 20 ms   | RX waits before sending ACK / ACKF     |
| `FRAG_ACK_TIMEOUT_MS` | 5000 ms | Max time to wait before retransmission |
| `FRAG_MAX_TRIES`      | 3       | Max retries per fragment               |
| `LISTEN_AFTER_TX_MS`  | 40 ms   | Short RX window after each TX (GBN/SR) |
| `FRAG_SPACING_MS`     | 20 ms   | Gap before retry (used mainly in S&W)  |

---

# 1️⃣ Stop-and-Wait (S&W)

### Algorithm (what happens)

- Send **one fragment**
- **Stop**
- Wait for **its ACK**
- Only then send the **next fragment**

No pipelining at all.

---

### Timing behavior (important)

For **each fragment**:

1. TX sends fragment
   → RX disabled during airtime

2. TX switches to RX and waits
   → waits **up to 5000 ms** (`FRAG_ACK_TIMEOUT_MS`)
   → polling `LoRa.parsePacket()`

3. RX side:

   - receives fragment
   - waits **20 ms**
   - sends `ACKF`

4. If ACKF arrives within 5 s
   → fragment accepted → next fragment

5. If not:

   - wait **20 ms**
   - retry same fragment
   - up to **3 times**

Worst case per fragment:

```
≈ 3 × 5 seconds
```

---

### Key property

- **Next fragment is never sent** until current fragment is ACKed
- Very reliable
- Very slow

---

# 2️⃣ Go-Back-N (GBN)

### Algorithm (what happens)

- Send fragments **continuously** up to window size
- Track the **oldest un-ACKed fragment** → `base`
- ACKs may arrive **out of order**
- If `base` ACK is missing too long → **retransmit everything from base onward**

---

### Timing behavior (this answers most confusion)

#### Sending phase

- TX sends fragments back-to-back (limited by airtime)
- After **each fragment TX**:

  - TX listens **40 ms** (`LISTEN_AFTER_TX_MS`)
  - catches fast ACKFs

#### ACK tracking

- ACKFs set `fragAcked[i] = true`
- `base` moves **only when ACK(base) arrives**
- ACKs for later fragments **do not move base**

---

#### Timeout behavior (VERY IMPORTANT)

- **Only the base fragment has a timeout**
- Timer starts when **base fragment was last sent**
- If:

  ```
  now − fragLastTx[base] > 5000 ms
  ```

  then:

➡️ **Retransmit ALL fragments**

```
base .. nextToSend−1
```

Even if:

- window is not full
- other fragments were already ACKed

This does **not** wait for the window to finish.

---

### Key property

- One lost ACK causes **many retransmissions**
- Faster than S&W when clean
- Wasteful when lossy

---

# 3️⃣ Selective Repeat (SR)

### Algorithm (what happens)

- Same windowed sending as GBN
- But **each fragment has its own timer**
- Only missing fragments are retransmitted

---

### Timing behavior

#### Sending phase

- TX sends fragments up to window size
- After **each TX**:

  - listens **40 ms**

#### ACK tracking

- ACKFs can arrive in any order
- `fragAcked[i] = true`
- `base` moves forward only when contiguous ACKs exist
  (same base rule as GBN)

---

#### Timeout behavior (key difference)

For **each fragment `j`**:

```
if now − fragLastTx[j] > 5000 ms
```

then:

- Retransmit **only fragment j**
- Wait `LISTEN_AFTER_TX_MS`
- Increment retry count

No window-wide retransmission.

If any fragment exceeds **3 retries** → abort message.

---

### Key property

- Loss affects **only that fragment**
- Best throughput + energy efficiency
- Best choice for LoRa multi-hop

---

# Side-by-side comparison (your code)

| Aspect         | Stop-and-Wait        | Go-Back-N         | Selective Repeat |
| -------------- | -------------------- | ----------------- | ---------------- |
| Sending        | 1 fragment at a time | Windowed          | Windowed         |
| ACK waited for | Current fragment     | Base fragment     | Each fragment    |
| Timeout scope  | Single fragment      | Whole window      | Single fragment  |
| Retransmission | Same fragment        | All from base     | Only missing     |
| Base movement  | Immediate            | ACK(base) only    | ACK(base) only   |
| Energy use     | High                 | Very high on loss | Lowest           |
| Best for LoRa  | ❌                   | ⚠️                | ✅               |

---

# One-sentence takeaway (memorize this)

```
S&W: wait → ACK → next
GBN: timeout(base) → resend window
SR: timeout(j) → resend j
```

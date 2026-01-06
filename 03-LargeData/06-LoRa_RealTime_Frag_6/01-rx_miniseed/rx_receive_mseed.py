#!/usr/bin/env python3
"""
RX-side reassembler for LoRa tunnel.

- Connects to RX MCU serial.
- Listens for:
    MSG,src,seq,rssi,d_m,text
    FRAG,src,seq,idx,tot,rssi,d_m,chunk
- Reassembles fragments by (src, seq).
- When complete, checks if payload starts with "FILE:".
  If so, parses:
      FILE:<filename>:<base64-data>
  and writes <filename> in the current directory.

Adjust SERIAL_PORT before running.
"""

import base64
import serial
import time
from pathlib import Path
from collections import defaultdict

# ==== CONFIG ====
SERIAL_PORT = "COM9"   # <-- CHANGE THIS to your RX MCU port
BAUD_RATE   = 115200
# ===============

class MessageReassembler:
    """Reassembles FRAG messages coming from the RX MCU."""

    def __init__(self):
        # (src, seq) -> {"tot": int, "chunks": {idx: str}}
        self.messages = {}

    def add_frag(self, src, seq, idx, tot, chunk):
        key = (src, seq)
        if key not in self.messages:
            self.messages[key] = {"tot": tot, "chunks": {}}
        msg = self.messages[key]
        msg["tot"] = tot  # in case first frag wasn't idx=0
        msg["chunks"][idx] = chunk

        # Check if we have all chunks
        if len(msg["chunks"]) == msg["tot"]:
            # Reassemble
            ordered = [msg["chunks"][i] for i in range(msg["tot"])]
            full_payload = "".join(ordered)
            # Remove from dict
            del self.messages[key]
            return full_payload
        return None

def handle_full_payload(payload):
    """
    Called when we have a fully reassembled payload that was originally
    one long line at the TX PC.

    If it starts with FILE:<filename>:<base64>, save the file.
    Otherwise, just print it.
    """
    if payload.startswith("FILE:"):
        # Split only on the first two ':' to avoid breaking base64
        # Format: FILE:<filename>:<base64>
        try:
            _, fname, b64 = payload.split(":", 2)
        except ValueError:
            print("[WARN] FILE payload format invalid, printing raw:")
            print(payload)
            return

        print(f"[INFO] Received FILE '{fname}' (base64 length {len(b64)})")

        try:
            raw = base64.b64decode(b64)
        except Exception as e:
            print(f"[ERROR] Base64 decode failed: {e}")
            return

        out_path = Path(fname)
        out_path.write_bytes(raw)
        print(f"[OK] Wrote {len(raw)} bytes to '{out_path.resolve()}'")
    else:
        # Not a FILE message, just show it
        print("[FULL PAYLOAD]", payload)

def main():
    reasm = MessageReassembler()

    print(f"Opening serial port {SERIAL_PORT} @ {BAUD_RATE}...")
    with serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1) as ser:
        # Give board time to reset
        time.sleep(2.0)
        print("Listening for FRAG/MSG lines from RX MCU...")

        while True:
            try:
                line = ser.readline().decode(errors="ignore").strip()
                if not line:
                    continue

                # Debug: see raw line
                # print(f"[RAW] {line}")

                if line.startswith("MSG,"):
                    # MSG,src,seq,rssi,d_m,text
                    parts = line.split(",", 5)
                    if len(parts) < 6:
                        print("[WARN] Bad MSG line:", line)
                        continue
                    _, src, seq, rssi, d_m, text = parts
                    print(f"[MSG] src={src} seq={seq} rssi={rssi} d~{d_m}m text='{text[:50]}'")

                    # If text is itself an entire FILE payload (small file),
                    # you can handle it here:
                    handle_full_payload(text)

                elif line.startswith("FRAG,"):
                    # FRAG,src,seq,idx,tot,rssi,d_m,chunk
                    parts = line.split(",", 7)
                    if len(parts) < 8:
                        print("[WARN] Bad FRAG line:", line)
                        continue
                    _, src, seq, idx, tot, rssi, d_m, chunk = parts
                    try:
                        seq_i = int(seq)
                        idx_i = int(idx)
                        tot_i = int(tot)
                    except ValueError:
                        print("[WARN] Non-integer seq/idx/tot in FRAG:", line)
                        continue

                    full = reasm.add_frag(src, seq_i, idx_i, tot_i, chunk)
                    if full is not None:
                        print(f"[INFO] Got full payload for src={src} seq={seq_i}, length={len(full)}")
                        handle_full_payload(full)

                else:
                    # Some other debug line from MCU
                    print(f"[MCU] {line}")

            except KeyboardInterrupt:
                print("\nExiting.")
                break
            except Exception as e:
                print(f"[ERROR] {e}")
                time.sleep(1)

if __name__ == "__main__":
    main()

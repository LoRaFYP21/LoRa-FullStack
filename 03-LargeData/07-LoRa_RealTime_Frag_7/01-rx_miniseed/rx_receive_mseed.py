#!/usr/bin/env python3
"""
RX-side reassembler for LoRa tunnel.

Stage 1 (LoRa-level):
- Connects to RX MCU serial.
- Listens for:
    MSG,src,seq,rssi,d_m,text
    FRAG,src,seq,idx,tot,rssi,d_m,chunk
- Reassembles LoRa fragments by (src, seq) into full text payloads.

Stage 2 (file-level):
- Each full text payload may be:
    a) FILECHUNK:<filename>:<idx>:<tot>:<base64-chunk>
    b) FILE:<filename>:<base64-data> (legacy small-file mode)
- For (a): reassembles <tot> chunks into one big base64 string per file.
- Once complete, decodes base64 and writes <filename> in current directory.

Adjust SERIAL_PORT before running.
"""

import base64
import serial
import time
from pathlib import Path
from collections import defaultdict

# ==== CONFIG ====
SERIAL_PORT = "COM12"   # <-- CHANGE THIS to your RX MCU port
BAUD_RATE   = 115200
# ===============

class MessageReassembler:
    """Reassembles FRAG messages coming from the RX MCU (LoRa-level)."""

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

class ChunkedFileReassembler:
    """
    Reassembles FILECHUNK:<fname>:<idx>:<tot>:<b64_chunk>
    into a single base64 string per file (file-level).
    """

    def __init__(self):
        # fname -> {"tot": int, "chunks": {idx: str}}
        self.files = {}

    def add_chunk(self, fname, idx, tot, b64_chunk):
        entry = self.files.setdefault(fname, {"tot": tot, "chunks": {}})
        entry["tot"] = tot
        entry["chunks"][idx] = b64_chunk

        if len(entry["chunks"]) == entry["tot"]:
            # Reassemble
            ordered = [entry["chunks"][i] for i in range(entry["tot"])]
            full_b64 = "".join(ordered)
            del self.files[fname]
            return full_b64
        return None

file_reasm = ChunkedFileReassembler()

def handle_full_payload(payload):
    """
    Called when we have a fully reassembled payload that was originally
    one long line at the TX MCU.

    This can be:
    - FILECHUNK:<fname>:<idx>:<tot>:<b64_chunk>
    - FILE:<fname>:<b64>
    - Or just any other text.
    """

    # ==============================
    # New big-file protocol: FILECHUNK
    # ==============================
    if payload.startswith("FILECHUNK:"):
        try:
            # Split into 5 parts max:
            #   FILECHUNK:<fname>:<idx>:<tot>:<b64_chunk>
            _, fname, idx_str, tot_str, b64_chunk = payload.split(":", 4)
            idx = int(idx_str)
            tot = int(tot_str)
        except ValueError:
            print("[WARN] FILECHUNK payload format invalid, printing raw:")
            print(payload)
            return

        full_b64 = file_reasm.add_chunk(fname, idx, tot, b64_chunk)
        print(f"[INFO] Got FILECHUNK {idx+1}/{tot} for '{fname}'")

        # If we now have all chunks for this file, decode & write
        if full_b64 is not None:
            print(f"[INFO] All {tot} chunks received for '{fname}'. Base64 len={len(full_b64)}")
            try:
                raw = base64.b64decode(full_b64)
            except Exception as e:
                print(f"[ERROR] Base64 decode failed: {e}")
                return

            out_path = Path(fname)
            out_path.write_bytes(raw)
            print(f"[OK] Wrote {len(raw)} bytes to '{out_path.resolve()}'")

        return

    # ==============================
    # Legacy small-file protocol: FILE
    # ==============================
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
        return

    # ==============================
    # Non-file payload
    # ==============================
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

                    # If text is itself an entire FILE/FILECHUNK payload
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

#!/usr/bin/env python3
"""
RX-side reassembler for LoRa tunnel.

- Connects to RX MCU serial.
- Listens for:
    MSG,src,seq,rssi,d_m,text
    FRAG,src,seq,idx,tot,rssi,d_m,chunk
- Reassembles LoRa fragments by (src, seq).
- When a full payload is available:
    If it starts with:
        FILECHUNK:<fname>:<idx>:<tot>:<base64_chunk>
    it will reassemble FILECHUNKs per file and finally write <fname>.
    If it starts with:
        FILE:<fname>:<base64-data>
    it writes that file directly (legacy mode).

Adjust SERIAL_PORT before running.
"""

import base64
import serial
import time
from pathlib import Path

# ==== CONFIG ====
SERIAL_PORT = "COM12"   # <-- CHANGE THIS for RX MCU
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
        msg["tot"] = tot
        msg["chunks"][idx] = chunk

        if len(msg["chunks"]) == msg["tot"]:
            ordered = [msg["chunks"][i] for i in range(msg["tot"])]
            full_payload = "".join(ordered)
            del self.messages[key]
            return full_payload
        return None


class FileChunkAssembler:
    """
    Assembles FILECHUNK:<fname>:<idx>:<tot>:<b64> messages into final files.
    """

    def __init__(self):
        # fname -> { "tot": int, "chunks": dict[idx] = str }
        self.files = {}

    def add_chunk(self, fname, idx, tot, b64_chunk):
        if fname not in self.files:
            self.files[fname] = {"tot": tot, "chunks": {}}
        entry = self.files[fname]

        entry["tot"] = tot
        entry["chunks"][idx] = b64_chunk

        print(f"[INFO] Got FILECHUNK {idx+1}/{tot} for '{fname}'")

        if len(entry["chunks"]) == entry["tot"]:
            ordered = [entry["chunks"][i] for i in range(entry["tot"])]
            full_b64 = "".join(ordered)
            del self.files[fname]

            try:
                raw = base64.b64decode(full_b64)
            except Exception as e:
                print(f"[ERROR] Base64 decode failed for '{fname}': {e}")
                return

            out_path = Path(fname)
            out_path.write_bytes(raw)
            print(f"[OK] Reassembled and wrote {len(raw)} bytes to '{out_path.resolve()}'")


def handle_full_payload(payload, file_asm: FileChunkAssembler):
    """
    Called when we have a fully reassembled payload from LoRa-level FRAGs.

    Supports:
      - FILECHUNK:<fname>:<idx>:<tot>:<base64_chunk>
      - FILE:<fname>:<base64>
    """
    if payload.startswith("FILECHUNK:"):
        # FILECHUNK:<fname>:<idx>:<tot>:<base64_chunk>
        try:
            _, fname, s_idx, s_tot, b64_chunk = payload.split(":", 4)
            idx = int(s_idx)
            tot = int(s_tot)
        except ValueError:
            print("[WARN] FILECHUNK payload format invalid, printing raw:")
            print(payload[:120] + "...")
            return
        file_asm.add_chunk(fname, idx, tot, b64_chunk)
        return

    if payload.startswith("FILE:"):
        # Legacy one-shot file:
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

    # Otherwise, just log it
    print("[FULL PAYLOAD]", payload[:120] + ("..." if len(payload) > 120 else ""))


def main():
    reasm = MessageReassembler()
    file_asm = FileChunkAssembler()

    print(f"Opening serial port {SERIAL_PORT} @ {BAUD_RATE}...")
    with serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1) as ser:
        time.sleep(2.0)
        print("Listening for FRAG/MSG lines from RX MCU...")

        while True:
            try:
                line = ser.readline().decode(errors="ignore").strip()
                if not line:
                    continue

                if line.startswith("MSG,"):
                    # MSG,src,seq,rssi,d_m,text
                    parts = line.split(",", 5)
                    if len(parts) < 6:
                        print("[WARN] Bad MSG line:", line)
                        continue
                    _, src, seq, rssi, d_m, text = parts
                    print(f"[MSG] src={src} seq={seq} rssi={rssi} d~{d_m}m text='{text[:50]}'")

                    # small messages might directly contain FILE or FILECHUNK
                    handle_full_payload(text, file_asm)

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
                        handle_full_payload(full, file_asm)

                else:
                    print(f"[MCU] {line}")

            except KeyboardInterrupt:
                print("\nExiting.")
                break
            except Exception as e:
                print(f"[ERROR] {e}")
                time.sleep(1)


if __name__ == "__main__":
    main()

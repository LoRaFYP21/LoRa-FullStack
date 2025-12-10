#!/usr/bin/env python3
"""
TX-side file sender for LoRa tunnel (chunked + handshake).

- Reads 'synthetic_15m.mseed' from current directory.
- Base64-encodes it.
- Splits the base64 text into moderately sized chunks.
- Sends EACH chunk as a separate line to the TX MCU:

    FILECHUNK:<filename>:<idx>:<tot>:<base64-chunk>\n

- AFTER sending each line, waits for the MCU to finish sending all LoRa
  fragments for that chunk. It detects this by watching for "[TX DONE]"
  (or a failure message) on the MCU serial output.

This prevents the ESP32 Serial buffer from overflowing while the MCU is
busy sending LoRa.

Adjust SERIAL_PORT and FILENAME before running.
"""

import base64
import serial
import time
from pathlib import Path
import math

# ==== CONFIG ====
SERIAL_PORT = "COM9"       # <-- CHANGE THIS (e.g. "COM5" on Windows, "/dev/ttyUSB0" on Linux)
BAUD_RATE   = 115200
FILENAME    = "synthetic_36s.mseed"
CHARS_PER_CHUNK = 40000    # base64 chars per logical "message" to MCU
# ===============

def wait_for_tx_done(ser: serial.Serial, idx: int, total: int):
    """
    After sending one FILECHUNK line, block here until the MCU
    finishes sending all its LoRa fragments for that chunk.

    We watch the MCU's debug output for:
      - "[TX DONE]"  (success)
      - "TX FAILED"  (single-packet fail)
      - "[ABORT]"    (fragment fail)

    Any lines from the MCU are printed as [MCU] ...
    """
    print(f"Waiting for MCU to finish chunk {idx+1}/{total}...")
    while True:
        line = ser.readline().decode(errors="ignore").strip()
        if not line:
            # No line yet; just loop
            continue
        print(f"[MCU] {line}")

        if "[TX DONE]" in line:
            print(f"[INFO] MCU reports TX DONE for chunk {idx+1}/{total}")
            break
        if "TX FAILED" in line or "[ABORT]" in line:
            print(f"[WARN] MCU reported a failure while sending chunk {idx+1}/{total}")
            # We still break; user can decide what to do next.
            break

def main():
    path = Path(FILENAME)
    if not path.is_file():
        print(f"Error: file '{FILENAME}' not found in current directory.")
        return

    # Read raw MiniSEED bytes
    raw = path.read_bytes()
    print(f"Read {len(raw)} bytes from {FILENAME}")

    # Base64 encode so it becomes ASCII-safe text
    b64 = base64.b64encode(raw).decode("ascii")
    print(f"Base64 length: {len(b64)} characters")

    # Compute how many FILECHUNK lines we will send
    total_chunks = math.ceil(len(b64) / CHARS_PER_CHUNK)
    print(f"Will send {total_chunks} FILECHUNK lines to MCU")

    # Open serial to TX MCU
    print(f"Opening serial port {SERIAL_PORT} @ {BAUD_RATE}...")
    with serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1) as ser:
        # Give the board a moment after opening the port (ESP32 often resets)
        time.sleep(2.0)

        # Clear any boot messages from MCU
        while ser.in_waiting:
            boot_line = ser.readline().decode(errors="ignore").strip()
            if boot_line:
                print(f"[MCU-BOOT] {boot_line}")

        # Send each chunk as its own FILECHUNK line, with handshake
        for idx in range(total_chunks):
            start = idx * CHARS_PER_CHUNK
            end   = min(len(b64), start + CHARS_PER_CHUNK)
            chunk = b64[start:end]

            # Application-level payload:
            # FILECHUNK:<filename>:<idx>:<total>:<base64-chunk>
            line = f"FILECHUNK:{FILENAME}:{idx}:{total_chunks}:{chunk}\n"

            print(f"\n=== Sending FILECHUNK {idx+1}/{total_chunks} (len={len(chunk)}) ===")
            ser.write(line.encode("utf-8"))
            ser.flush()

            # Now wait until MCU finishes this chunk (all its LoRa fragments)
            wait_for_tx_done(ser, idx, total_chunks)

        print("\nAll FILECHUNK lines sent and acknowledged by MCU (TX side).")

if __name__ == "__main__":
    main()

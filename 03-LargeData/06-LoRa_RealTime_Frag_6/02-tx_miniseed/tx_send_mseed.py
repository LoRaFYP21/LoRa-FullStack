#!/usr/bin/env python3
"""
TX-side file sender for LoRa tunnel.

- Reads 'synthetic_1h.mseed' from current directory.
- Base64-encodes it.
- Sends ONE long line over Serial:
    FILE:<filename>:<base64-data>\n

Your TX MCU sketch (PC reassembly mode) sees that as a big text line,
fragments it, and sends it over LoRa.

Adjust SERIAL_PORT before running.
"""

import base64
import serial
import time
from pathlib import Path

# ==== CONFIG ====
SERIAL_PORT = "COM12"       # <-- CHANGE THIS (e.g. "COM5" on Windows, "/dev/ttyUSB0" on Linux)
BAUD_RATE   = 115200
FILENAME    = "synthetic_1h.mseed"
# ===============

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

    # Build the application-level payload
    # Format: FILE:<filename>:<base64>
    payload = f"FILE:{FILENAME}:{b64}\n"

    # Open serial to TX MCU
    print(f"Opening serial port {SERIAL_PORT} @ {BAUD_RATE}...")
    with serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1) as ser:
        # Give the board a moment after opening the port (ESP32 often resets)
        time.sleep(2.0)

        print("Sending FILE line to MCU...")
        ser.write(payload.encode("utf-8"))
        ser.flush()

        print("Done. MCU will now fragment + send via LoRa.")

        # Optional: show any debug output from MCU for a few seconds
        print("Reading MCU debug for 5 seconds...")
        end = time.time() + 5
        while time.time() < end:
            line = ser.readline().decode(errors="ignore").strip()
            if line:
                print(f"[MCU] {line}")

if __name__ == "__main__":
    main()

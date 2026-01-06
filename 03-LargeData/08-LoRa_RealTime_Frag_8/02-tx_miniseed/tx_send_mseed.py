#!/usr/bin/env python3
"""
TX-side file sender for LoRa tunnel (with ARQ on MCU).

- Reads MiniSEED file from current directory.
- Base64-encodes it.
- Splits into big ASCII-safe lines:
    FILECHUNK:<filename>:<idx>:<tot>:<base64-chunk>\n
- Sends each FILECHUNK line over Serial to TX MCU.
- After each chunk, waits until MCU reports:
    [TX DONE]   -> success for that chunk
    [ABORT]     -> failure, stops

Adjust SERIAL_PORT and FILENAME before running.
"""

import base64
import serial
import time
from pathlib import Path

# ==== CONFIG ====
SERIAL_PORT = "COM9"       # <-- CHANGE THIS for TX MCU
BAUD_RATE   = 115200
FILENAME    = "synthetic_36s.mseed"   # <-- CHANGE THIS
CHUNK_SIZE  = 40000        # characters of base64 per FILECHUNK
CHUNK_SEND_TIMEOUT = 300.0 # seconds max to wait per chunk (allow SR/GBN to finish)
# =================


def wait_for_chunk_done(ser, chunk_idx, chunk_tot):
    """
    Wait for MCU to either:
      - report [TX DONE] ...  -> success
      - report [ABORT] or TX FAILED -> failure
    Returns True on success, False on failure.
    """
    deadline = time.time() + CHUNK_SEND_TIMEOUT
    ok = False
    warned = False

    while time.time() < deadline:
        line = ser.readline().decode(errors="ignore").strip()
        if not line:
            continue

        # Just echo any MCU output
        print(f"[MCU] {line}")

        if "[TX DONE]" in line:
            print(f"[INFO] MCU reports TX DONE for chunk {chunk_idx+1}/{chunk_tot}")
            ok = True
            break

        if "[ABORT]" in line or "TX FAILED" in line:
            print(f"[WARN] MCU reported a failure while sending chunk {chunk_idx+1}/{chunk_tot}")
            warned = True
            ok = False
            break

    if not ok and not warned:
        print(f"[WARN] Timeout while waiting for TX DONE for chunk {chunk_idx+1}/{chunk_tot}")

    return ok


def main():
    path = Path(FILENAME)
    if not path.is_file():
        print(f"Error: file '{FILENAME}' not found in current directory.")
        return

    raw = path.read_bytes()
    print(f"Read {len(raw)} bytes from {FILENAME}")

    b64 = base64.b64encode(raw).decode("ascii")
    print(f"Base64 length: {len(b64)} characters")

    # Split into big base64 chunks
    chunks = []
    for i in range(0, len(b64), CHUNK_SIZE):
        chunks.append(b64[i:i+CHUNK_SIZE])
    tot = len(chunks)
    print(f"Will send {tot} FILECHUNK lines to MCU")

    # Open serial
    print(f"Opening serial port {SERIAL_PORT} @ {BAUD_RATE}...")
    with serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1) as ser:
        # Give ESP32 time to boot/reset
        time.sleep(3.0)

        # Dump initial boot messages
        boot_deadline = time.time() + 3.0
        while time.time() < boot_deadline:
            line = ser.readline().decode(errors="ignore").strip()
            if line:
                print(f"[MCU-BOOT] {line}")

        # Send chunks one by one
        for idx, chunk in enumerate(chunks):
            print(f"\n=== Sending FILECHUNK {idx+1}/{tot} (len={len(chunk)}) ===")

            payload = f"FILECHUNK:{FILENAME}:{idx}:{tot}:{chunk}\n"
            ser.write(payload.encode("utf-8"))
            ser.flush()

            print(f"Waiting for MCU to finish chunk {idx+1}/{tot}...")
            ok = wait_for_chunk_done(ser, idx, tot)
            if not ok:
                print("[ERROR] Stopping due to TX failure.")
                break

        print("\nAll FILECHUNK lines sent (TX side finished).")


if __name__ == "__main__":
    main()

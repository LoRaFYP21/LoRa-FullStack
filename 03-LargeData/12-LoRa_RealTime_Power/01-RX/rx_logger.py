#!/usr/bin/env python3
"""Capture RX-side power test logs from serial into a CSV."""
import csv
import os
import time
import serial

SERIAL_PORT = "COM9"
BAUD = 115200
OUT_CSV = "rx_results.csv"

FIELDNAMES = [
    "pc_time_iso", "type", "event", "raw_line",
    "testId", "slot", "sf", "bw", "seq", "len", "toa_ms",
    "txp_dbm",
    "rssi_dbm", "snr_db",
    "pathloss_db",
    "role", "state", "cmd"
]

def ensure_csv(path: str, fieldnames: list[str]) -> None:
    if (not os.path.exists(path)) or os.path.getsize(path) == 0:
        with open(path, "w", newline="", encoding="utf-8") as f:
            csv.DictWriter(f, fieldnames=fieldnames).writeheader()

def parse_log_line(line: str) -> dict:
    parts = [p.strip() for p in line.split(",")]
    out = {"type": "", "event": "", "raw_line": line}

    if len(parts) < 2 or parts[0] != "LOG":
        return out

    out["type"] = parts[1]

    # If this is an EVENT line: LOG,EVENT,<EVENTNAME>,k1,v1,k2,v2...
    i = 2
    if out["type"] == "EVENT" and len(parts) >= 3:
        out["event"] = parts[2]
        i = 3  # start parsing pairs after event name

    # Parse remaining tokens as key,value pairs
    while i + 1 < len(parts):
        k = parts[i]
        v = parts[i + 1]
        out[k] = v
        i += 2

    return out

def main():
    ensure_csv(OUT_CSV, FIELDNAMES)

    ser = serial.Serial(SERIAL_PORT, BAUD, timeout=0.2)
    time.sleep(1.0)
    ser.reset_input_buffer()

    ser.write(b"ROLE RX\n")
    time.sleep(0.2)
    ser.write(b"ARM\n")
    time.sleep(0.2)
    ser.write(b"STATUS\n")

    print(f"[RX] Logging from {SERIAL_PORT} -> {OUT_CSV}")
    try:
        with open(OUT_CSV, "a", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, fieldnames=FIELDNAMES, extrasaction="ignore")
            while True:
                line = ser.readline().decode("utf-8", errors="replace").strip()
                if not line or not line.startswith("LOG,"):
                    continue

                ts = time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime())
                row = {"pc_time_iso": ts}
                row.update(parse_log_line(line))

                for k in FIELDNAMES:
                    row.setdefault(k, "")

                w.writerow(row)
                f.flush()
                print(line)

    except KeyboardInterrupt:
        print("\n[RX] Stopped.")
    finally:
        ser.close()

if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Send trigger commands and log TX-side metrics for the power tests."""
import csv
import os
import time
import serial

SERIAL_PORT = "COM12"
BAUD = 115200
OUT_CSV = "tx_results.csv"

FIELDNAMES = [
    "pc_time_iso", "type", "event", "raw_line",
    "testId", "slot", "sf", "bw", "seq", "len", "toa_ms",
    "txp_dbm",
    "ack", "ack_rssi_dbm", "ack_snr_db", "ack_pathloss_db",
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

    i = 2
    if out["type"] == "EVENT" and len(parts) >= 3:
        out["event"] = parts[2]
        i = 3

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

    ser.write(b"ROLE TX\n")
    time.sleep(0.2)
    ser.write(b"STATUS\n")
    time.sleep(0.2)
    ser.write(b"GO\n")

    print(f"[TX] Triggered. Logging from {SERIAL_PORT} -> {OUT_CSV}")
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
        print("\n[TX] Stopped.")
    finally:
        ser.close()

if __name__ == "__main__":
    main()

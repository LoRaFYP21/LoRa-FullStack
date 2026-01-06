#!/usr/bin/env python3
"""Plot TX ACK pathloss vs spreading factor from TX logs."""
import pandas as pd
import matplotlib.pyplot as plt

CSV_PATH = "tx_results.csv"          # change if needed
OUT_PNG  = "tx_ack_pathloss_vs_sf.png"

BW_ORDER = [125000, 250000, 500000]

def to_num(s):
    return pd.to_numeric(s, errors="coerce")

def main():
    df = pd.read_csv(CSV_PATH)

    # Keep only TX rows where ack is 1 (valid)
    df = df[df["type"] == "TX"].copy()
    df["ack"] = to_num(df.get("ack"))
    df = df[df["ack"] == 1].copy()

    # Numeric cleanup
    df["sf"] = to_num(df["sf"])
    df["bw"] = to_num(df["bw"])
    df["txp_dbm"] = to_num(df.get("txp_dbm"))
    df["ack_rssi_dbm"] = to_num(df.get("ack_rssi_dbm"))
    df["ack_pathloss_db"] = to_num(df.get("ack_pathloss_db"))

    # If ack_pathloss_db missing, compute from txp_dbm - ack_rssi_dbm
    if df["ack_pathloss_db"].isna().all():
        df["ack_pathloss_db"] = df["txp_dbm"] - df["ack_rssi_dbm"]

    df = df.dropna(subset=["sf", "bw", "ack_pathloss_db"])

    # Aggregate: mean loss per (sf,bw)
    g = df.groupby(["sf", "bw"], as_index=False)["ack_pathloss_db"].mean()

    plt.figure()
    for bw in BW_ORDER:
        sub = g[g["bw"] == bw].sort_values("sf")
        if sub.empty:
            continue
        plt.plot(sub["sf"], sub["ack_pathloss_db"], marker="o", label=f"BW {int(bw/1000)} kHz")

    plt.xlabel("Spreading Factor (SF)")
    plt.ylabel("Estimated Path Loss from ACK (dB)")
    plt.title("TX: ACK Path Loss vs SF (3 Bandwidths)")
    plt.xticks(sorted(g["sf"].dropna().unique()))
    plt.grid(True)
    plt.legend()

    plt.savefig(OUT_PNG, dpi=200, bbox_inches="tight")
    plt.show()

    print(f"[OK] Saved: {OUT_PNG}")

if __name__ == "__main__":
    main()

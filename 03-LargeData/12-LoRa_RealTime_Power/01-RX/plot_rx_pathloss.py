#!/usr/bin/env python3
"""Plot pathloss vs spreading factor from RX-side CSV logs."""
import pandas as pd
import matplotlib.pyplot as plt

CSV_PATH = "rx_results.csv"          # change if needed
OUT_PNG  = "rx_pathloss_vs_sf.png"

BW_ORDER = [125000, 250000, 500000]

def to_num(s):
    return pd.to_numeric(s, errors="coerce")

def main():
    df = pd.read_csv(CSV_PATH)

    # Keep only RX rows
    df = df[df["type"] == "RX"].copy()

    # Numeric cleanup
    df["sf"] = to_num(df["sf"])
    df["bw"] = to_num(df["bw"])
    df["txp_dbm"] = to_num(df.get("txp_dbm"))
    df["rssi_dbm"] = to_num(df.get("rssi_dbm"))
    df["pathloss_db"] = to_num(df.get("pathloss_db"))

    # If pathloss_db missing, compute from txp_dbm - rssi_dbm
    if df["pathloss_db"].isna().all():
        df["pathloss_db"] = df["txp_dbm"] - df["rssi_dbm"]

    # Drop incomplete rows
    df = df.dropna(subset=["sf", "bw", "pathloss_db"])

    # Aggregate: mean loss per (sf,bw)
    g = df.groupby(["sf", "bw"], as_index=False)["pathloss_db"].mean()

    plt.figure()
    for bw in BW_ORDER:
        sub = g[g["bw"] == bw].sort_values("sf")
        if sub.empty:
            continue
        plt.plot(sub["sf"], sub["pathloss_db"], marker="o", label=f"BW {int(bw/1000)} kHz")

    plt.xlabel("Spreading Factor (SF)")
    plt.ylabel("Estimated Path Loss (dB)")
    plt.title("RX: Path Loss vs SF (3 Bandwidths)")
    plt.xticks(sorted(g["sf"].dropna().unique()))
    plt.grid(True)
    plt.legend()

    plt.savefig(OUT_PNG, dpi=200, bbox_inches="tight")
    plt.show()

    print(f"[OK] Saved: {OUT_PNG}")

if __name__ == "__main__":
    main()

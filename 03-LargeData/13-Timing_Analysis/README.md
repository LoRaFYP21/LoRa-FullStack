# Timing Analysis (Python + venv)

This folder uses a Python virtual environment for the helper scripts (`csv_capture.py`, `csv_download.py`, `src/serial_to_udp.py`). Follow these steps on Windows PowerShell.

## One-time setup (create/refresh venv)
1) `cd 03-LargeData/13-Timing_Analysis`
2) `python -m venv .venv`  (recreate it if the existing one is stale)
3) `.\.venv\Scripts\Activate`
4) `python -m pip install --upgrade pip`
5) `pip install pyserial`  (only dependency used by the Python tools here)

## Running the scripts (with venv active)
- Capture timing CSVs: `python csv_capture.py COM11 115200`
  - Creates timestamped `Tx_*.csv` and `Rx_*.csv` in this folder.
- Download device CSVs (LittleFS): `python csv_download.py COM11 115200`
- Forward serial to UDP (from `src/`): `python src/serial_to_udp.py COM11`
- Convenience batch file: `start_csv_capture.bat` (run after activating the venv so it picks up `pyserial`).

## Deactivate
- When finished, exit the venv with `deactivate` or by closing the shell.

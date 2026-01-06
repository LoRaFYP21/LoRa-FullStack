# Lilygo

This repository contains example sketches, tools, and experiments for LilyGo (TTGO) LoRa development boards used in the FYP (final year project) LoRa experiments.

All folders now carry numeric prefixes for consistent navigation. For per-file descriptions, see `PROJECT_FILE_MAP.md`.

Contents
- `02-Bidirectional/` – Ping-pong initiator and responder sketches.
- `01-Unidirectional/` – Simple TX/RX examples with OLED displays.
- `03-LargeData/` – Real-time fragmentation, relay/end-node experiments, timing analysis, multi-media file transfer, and power tests.

Prerequisites
- Arduino IDE or PlatformIO (VS Code) for compiling and uploading the `.ino` sketches.
- Python 3.x for the helper scripts (e.g., CSV capture, serial to UDP). Install `pyserial` if you use serial scripts: `pip install pyserial`.
- The appropriate LilyGo board support package in Arduino/PlatformIO. Select the board that matches your hardware (e.g., TTGO LoRa32).

Building and uploading

Arduino IDE
- Open the `.ino` file for the example you want (for example `02-Bidirectional/01-LoRa_PingPong_Initiator/01-LoRa_PingPong_Initiator.ino`).
- Select the correct board and COM port, then click Upload.

PlatformIO (recommended for the `Timing_Analysis` module)
- Open `03-LargeData/13-Timing_Analysis` as a PlatformIO project or run from the workspace root with PlatformIO CLI.
- Example (PowerShell):

```powershell
cd "d:\\FYP_Simulations\\Lilygo\\03-LargeData\\13-Timing_Analysis"
platformio run --target upload
```

Python utilities
- `03-LargeData/13-Timing_Analysis/csv_capture.py` – captures serial timing logs and writes CSV files.
- `03-LargeData/13-Timing_Analysis/src/serial_to_udp.py` – forwards serial data over UDP.

Usage notes
- Most examples are self-contained Arduino sketches. Inspect the top of each `.ino` for hardware-specific configuration (pins, SPI, LoRa frequency, spreading factor, etc.).
- Large-packet / fragmentation examples may require matching settings on sender and receiver (fragment size, reassembly timeout).
- The `13-Timing_Analysis` folder contains helper scripts and sample CSVs generated during experiments.

Repository layout (high-level)

```
00-README.md
01-Unidirectional/
  01-LoRa_TX_OLED/01-LoRa_TX_OLED.ino
  02-LoRa_RX_OLED/02-LoRa_RX_OLED.ino
02-Bidirectional/
  01-LoRa_PingPong_Initiator/01-LoRa_PingPong_Initiator.ino
  02-LoRa_PingPong_Responder/02-LoRa_PingPong_Responder.ino
03-LargeData/
  01-05 LoRa_RealTime_Frag iterations (*.ino)
  06-08 LoRa_RealTime_Frag_* with MiniSEED TX/RX sketches
  09-LoRa_RealTime_SerialMonitor/09-LoRa_RealTime_SerialMonitor.ino
  10-End_Node_With_Relay/(end + relay sketches)
  11-LoRa_RealTime_MultiM/ (multi-media tunnel + helpers)
  12-LoRa_RealTime_Power/ (RX/TX power logging + plots)
  13-Timing_Analysis/ (PlatformIO timing experiments)
  14-Resources/ (papers and reference plots)
  15-Improvements.txt
```

Troubleshooting
- If uploads fail, double-check board selection and COM/serial port.
- If serial tools can't access a port, close other programs (Serial Monitor, other terminals) that hold the device.
- For PlatformIO issues, run `platformio update` and ensure the `platformio.ini` in `13-Timing_Analysis` matches your board.

Contributing
- Feel free to open issues or add PRs with improvements, bug fixes, or updated instructions for specific LilyGo board variants.

Contact
- If this is part of your project, update this README with author/contact info and any experiment-specific notes.

---
Updated: 2025-12-21

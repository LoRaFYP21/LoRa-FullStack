# LoRa-FullStack

This repository contains example sketches, tools, and experiments for building a full-stack LoRa messaging and large-data pipeline on LilyGo (TTGO) boards, used in the FYP (final year project) LoRa experiments.

All folders carry numeric prefixes for consistent navigation. For per-file descriptions, see `PROJECT_FILE_MAP.md`.

Contents
- `01-Node_Basics/` - Simple TX/RX examples with OLED displays.
- `02-Link_PingPong/` - Ping-pong initiator and responder sketches.
- `03-FullStack_Experiments/` - Fragmentation iterations, seismic/MiniSEED streaming, relay experiments, multi-media tunnel, power/pathloss tests, timing analysis, and the new reliable mesh.

Prerequisites
- Arduino IDE or PlatformIO (VS Code) for compiling and uploading the `.ino` sketches.
- Python 3.x for the helper scripts (e.g., CSV capture, serial to UDP). Install `pyserial` if you use serial scripts: `pip install pyserial`.
- The appropriate LilyGo board support package in Arduino/PlatformIO. Select the board that matches your hardware (e.g., TTGO LoRa32).

Building and uploading

Arduino IDE
- Open the `.ino` file for the example you want (for example `02-Link_PingPong/01-PingPong_Initiator/01-PingPong_Initiator.ino`).
- Select the correct board and COM port, then click Upload.

PlatformIO (recommended for the `Timing_Analysis` module)
- Open `03-FullStack_Experiments/13-Timing_Analysis` as a PlatformIO project or run from the workspace root with PlatformIO CLI.
- Example (PowerShell):

```powershell
cd "d:\\FYP_Simulations\\LoRa-FullStack\\03-FullStack_Experiments\\13-Timing_Analysis"
platformio run --target upload
```

Python utilities
- `03-FullStack_Experiments/13-Timing_Analysis/csv_capture.py` - captures serial timing logs and writes CSV files.
- `03-FullStack_Experiments/13-Timing_Analysis/src/serial_to_udp.py` - forwards serial data over UDP.

Usage notes
- Most examples are self-contained Arduino sketches. Inspect the top of each `.ino` for hardware-specific configuration (pins, SPI, LoRa frequency, spreading factor, etc.).
- Large-packet / fragmentation examples may require matching settings on sender and receiver (fragment size, reassembly timeout).
- The `13-Timing_Analysis` folder contains helper scripts and sample CSVs generated during experiments.

Repository layout (high-level)

```
README.md
01-Node_Basics/
  01-TX_OLED/01-TX_OLED.ino
  02-RX_OLED/02-RX_OLED.ino
02-Link_PingPong/
  01-PingPong_Initiator/01-PingPong_Initiator.ino
  02-PingPong_Responder/02-PingPong_Responder.ino
03-FullStack_Experiments/
  01-Fragmentation_v1/01-Fragmentation_v1.ino
  02-Fragmentation_v2/02-Fragmentation_v2.ino
  03-Fragmentation_v3/03-Fragmentation_v3.ino
  04-Fragmentation_v4/04-Fragmentation_v4.ino
  05-Fragmentation_v5/05-Fragmentation_v5.ino
  06-Seismic_Stream_v6/ (MiniSEED RX/TX + Python helpers)
  07-Seismic_Stream_v7/ (MiniSEED RX/TX + Python helpers)
  08-Seismic_Stream_v8/ (MiniSEED RX/TX + Python helpers)
  09-Serial_Monitor/09-Serial_Monitor.ino
  10-Relay_Experiment/(end-node + relay sketches)
  11-Multimedia_Tunnel/(ARQ multi-media tunnel + helpers)
  12-Power_Pathloss_Tests/(RX/TX metrics + plots)
  13-Timing_Analysis/(PlatformIO timing experiments)
  14-Resources/ (papers and reference plots)
  15-Improvements.txt
  16-Reliable_Mesh/(all-in-one mesh node with relays in ACK path)
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
Updated: 2026-01-06

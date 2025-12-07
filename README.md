# Lilygo

This repository contains example sketches, tools and experiments for LilyGo (TTGO) LoRa development boards used in the FYP (final year project) LoRa experiments.

Contents
 - Bidirectional examples (ping-pong initiator/responder)
 - Unidirectional examples (TX/RX with OLED)
 - LargeData experiments (real-time fragmentation, relay/end node examples, timing analysis)

**Quick Links**
- `Bidirectional/` — Ping-pong initiator & responder example sketches (.ino)
- `Unidirectional/` — Simple TX/RX examples with OLED displays
- `LargeData/` — Experiments for sending large payloads, fragmentation and timing analysis

Prerequisites
- Arduino IDE or PlatformIO (VS Code) for compiling and uploading the `.ino` sketches.
- Python 3.x for the helper scripts (e.g., CSV capture, serial to UDP). Install `pyserial` if you use serial scripts: `pip install pyserial`.
- The appropriate LilyGo board support package in Arduino/PlatformIO. Select the board that matches your hardware (e.g. TTGO LoRa32).

Building and uploading

Arduino IDE
- Open the `.ino` file for the example you want (for example `Bidirectional/LoRa_PingPong_Initiator/LoRa_PingPong_Initiator.ino`).
- Select the correct board and COM port, then click Upload.

PlatformIO (recommended for the `Timing_Analysis` module)
- Open the `LargeData/Timing_Analysis` folder as a PlatformIO project or run from the workspace root with PlatformIO CLI.
- Example (PowerShell):

```powershell
cd "d:\FYP_Simulations\Lilygo\LargeData\Timing_Analysis"
platformio run --target upload
```

Python utilities
- `LargeData/Timing_Analysis/csv_capture.py` — captures serial timing logs and writes CSV files.
- `LargeData/src/serial_to_udp.py` — forwards serial data over UDP.

Usage notes
- Most examples are self-contained Arduino sketches. Inspect the top of each `.ino` for hardware-specific configuration (pins, SPI, LoRa frequency, spreading factor, etc.).
- Large-packet / fragmentation examples may require matching settings on sender and receiver (fragment size, reassembly timeout).
- The `Timing_Analysis` folder contains helper scripts and sample CSVs generated during experiments.

Repository layout (high-level)

```
Bidirectional/
	LoRa_PingPong_Initiator/
		LoRa_PingPong_Initiator.ino
	LoRa_PingPong_Responder/
		LoRa_PingPong_Responder.ino
LargeData/
	Improvements.txt
	End node with Relay/
		end/end.ino
		relay/relay.ino
	LoRa_RealTime_Frag*/
		*.ino
	LoRa_RealTime_SerialMonitor/
		LoRa_RealTime_SerialMonitor.ino
	Timing_Analysis/
		csv_capture.py
		csv_download.py
		platformio.ini
		src/
			main.cpp
			serial_to_udp.py
Unidirectional/
	LoRa_RX_OLED/
	LoRa_TX_OLED/
```

Troubleshooting
- If uploads fail, double-check board selection and COM/serial port.
- If serial tools can't access a port, close other programs (Serial Monitor, other terminals) that hold the device.
- For PlatformIO issues, run `platformio update` and ensure the `platformio.ini` in `Timing_Analysis` matches your board.

Contributing
- Feel free to open issues or add PRs with improvements, bug fixes, or updated instructions for specific LilyGo board variants.

Contact
- If this is part of your project, update this README with author/contact info and any experiment-specific notes.

---
Updated: 2025-12-07
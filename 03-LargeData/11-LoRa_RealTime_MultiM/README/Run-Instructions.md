# Running the LoRa File Transfer Scripts (Windows + PowerShell)

This guide shows how to activate your virtual environment and run the scripts for receiving and sending files over LoRa on Windows.

## 1) Activate the venv

```powershell
Set-Location "C:\Users\HP\Desktop\Sem 7\Project\Lilygo\LargeData\LoRa_RealTime_MultiM"
.\.venv\Scripts\Activate.ps1
python -V
```

If activation is blocked, temporarily bypass policy:

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
.\.venv\Scripts\Activate.ps1
```

## 2) Install dependencies (if not already)

```powershell
pip install -r requirements.txt
# or, if no requirements file:
pip install pillow pydub
# For GUI capture/record features:
pip install sounddevice scipy opencv-python
```

## 3) Receive files

Use the correct COM port for your receiver board (replace `COM12` if needed) and set an output directory.

```powershell
python rx_receive_file.py COM12 --out-dir received_files
```

- The `received_files` folder will be created if it doesn't exist.
- Received files are saved with `"_rx"` appended before the extension, e.g. `earthquake.jpg` -> `earthquake_rx.jpg`.
- Received images/audio may be converted to common formats.

## 4) Send a text file

Replace `COM9` with your transmitter board's COM port.

```powershell
python tx_send_file.py COM9 my_notes.txt
```

## 5) Send an image (auto-convert to JPEG before sending)

```powershell
python tx_send_file.py COM9 earthquake.webp --jpeg-quality 85
```

### Image compression guidance

- `--jpeg-quality 20` provides strong compression while maintaining usable visual quality for diagrams and many images.
- Typical range: `20`–`85` (lower = smaller, more lossy).

## 6) Send audio (auto-convert to MP3 before sending)

```powershell
python tx_send_file.py COM9 human_voice.wav --mp3-bitrate 96k
```

### Audio compression guidance

- `--mp3-bitrate 8k` can still yield intelligible speech with surprisingly good perceived quality for voice; use for maximum compression.
- For general audio, consider `16k`–`64k` based on size vs. quality.

## Tips

- COM ports: Check in Device Manager under "Ports (COM & LPT)" to find the port numbers for your boards.
- Running from project root ensures paths (e.g., `received_files`) resolve correctly.
- Deactivate the venv when done:

```powershell
deactivate
```

## Troubleshooting

- "Module not found": Ensure venv is activated and dependencies are installed in that environment.
- Permission error on activation: Use the execution policy bypass snippet above.
- Wrong COM port: The script will fail to open the serial port. Verify in Device Manager and update the command.
- Image/audio conversion recursion error: Update to latest `tx_send_file.py` (we pass `--jpeg-quality` and `--mp3-bitrate` directly now; no monkeypatching). If you see `RecursionError`, pull latest changes and retry.

## GUI Usage (TX + RX in one app)

Run the GUI to send files/text and to listen/receive without CLI commands.

```powershell
Set-Location "C:\Users\HP\Desktop\Sem 7\Project\Lilygo\LargeData\LoRa_RealTime_MultiM"
.\.venv\Scripts\Activate.ps1
python gui_app.py
```

- Transmit tab:
  - Choose a file and click "Send File".
  - Type text and click "Send Text" (text is sent as a file).
  - Click "Record Voice" to capture mic audio (WAV) and send; MP3 conversion occurs on TX.
  - Click "Capture Photo" to take a camera snapshot and send.
  - Set `TX Serial Port`, `JPEG Quality`, `MP3 Bitrate` as needed.
- Receive tab:
  - Set `RX Serial Port` and `Out Dir`, click "Start Listening".
  - Saved files include `_rx` suffix (e.g., `my_notes_rx.txt`).
  - Logs show MCU messages and assembled payloads.

### GUI prerequisites

- Install optional packages in venv: `pip install sounddevice scipy opencv-python`.
- Camera uses device index 0; if multiple cameras exist, we can add selection.
- COM ports must be distinct for TX and RX (e.g., `COM9` vs `COM12`).

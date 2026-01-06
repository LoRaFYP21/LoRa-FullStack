#!/usr/bin/env python3
"""
Generic TX-side file sender for LoRa tunnel (with ARQ on MCU).

Features:
- Takes ANY file path.
- If it's an image (png, bmp, gif, etc.) -> convert to JPEG and send.
- If it's audio (wav, flac, m4a, etc.) -> convert to MP3 and send.
- For text (.txt, .csv, .json, .text) and other files -> send raw bytes.
- Base64-encodes the final bytes.
- Splits into big ASCII-safe lines:
    FILECHUNK:<filename>:<idx>:<tot>:<base64-chunk>\\n
- Sends each FILECHUNK line over Serial to TX MCU.
- Waits for MCU to print:
    [TX DONE]   -> success for that chunk/message
    [ABORT]     -> failure, stops

Usage:
    python tx_send_file.py COM9 path/to/myfile.png
"""

import argparse
import base64
import io
import mimetypes
import time
from pathlib import Path

import serial  # pip install pyserial

# Optional libraries for conversion
try:
    from PIL import Image  # pip install pillow
except ImportError:
    Image = None

try:
    from pydub import AudioSegment  # pip install pydub
except ImportError:
    AudioSegment = None

# Default settings
BAUD_RATE = 115200
CHUNK_SIZE = 40000          # characters of base64 per FILECHUNK line
CHUNK_SEND_TIMEOUT = 300.0  # seconds max to wait per chunk


TEXT_EXT = {".txt", ".csv", ".json", ".text"}
IMAGE_EXT = {".png", ".jpg", ".jpeg", ".bmp", ".gif", ".tif", ".tiff", ".webp"}
AUDIO_EXT = {".wav", ".flac", ".mp3", ".m4a", ".aac", ".ogg", ".wma", ".aif", ".aiff"}


def is_text_file(path: Path) -> bool:
    return path.suffix.lower() in TEXT_EXT or (
        mimetypes.guess_type(path.name)[0] or ""
    ).startswith("text/")


def is_image_file(path: Path) -> bool:
    if path.suffix.lower() in IMAGE_EXT:
        return True
    m = mimetypes.guess_type(path.name)[0]
    return bool(m and m.startswith("image/"))


def is_audio_file(path: Path) -> bool:
    if path.suffix.lower() in AUDIO_EXT:
        return True
    m = mimetypes.guess_type(path.name)[0]
    return bool(m and m.startswith("audio/"))


def convert_image_to_jpeg(path: Path, quality: int = 85) -> tuple[bytes, str]:
    """
    Convert any supported image to JPEG in-memory.
    Returns (jpeg_bytes, new_filename).
    """
    if Image is None:
        raise RuntimeError(
            "Pillow (PIL) not installed. Run: pip install pillow"
        )

    img = Image.open(path)
    img = img.convert("RGB")
    buf = io.BytesIO()
    img.save(buf, format="JPEG", quality=quality, optimize=True)
    return buf.getvalue(), path.with_suffix(".jpg").name


def convert_audio_to_mp3(path: Path, bitrate: str = "64k") -> tuple[bytes, str]:
    """
    Convert any supported audio file to MP3 in-memory.
    Returns (mp3_bytes, new_filename).
    """
    if AudioSegment is None:
        raise RuntimeError(
            "pydub not installed. Run: pip install pydub\n"
            "Also ensure ffmpeg is installed and in PATH."
        )

    audio = AudioSegment.from_file(path)
    buf = io.BytesIO()
    audio.export(buf, format="mp3", bitrate=bitrate)
    return buf.getvalue(), path.with_suffix(".mp3").name


def prepare_file_for_lora(path: Path, jpeg_quality: int | None = None, mp3_bitrate: str | None = None) -> tuple[bytes, str, str]:
    """
    Decide how to handle the file:
      - Image -> JPEG
      - Audio -> MP3
      - Others (text, etc.) -> raw bytes
    Returns (bytes_to_send, transmit_filename, description).
    """
    suffix = path.suffix.lower()

    if is_image_file(path) and suffix not in (".jpg", ".jpeg"):
        print(f"[INFO] Detected image '{path.name}', converting to JPEG...")
        q = jpeg_quality if jpeg_quality is not None else 85
        raw, out_name = convert_image_to_jpeg(path, quality=q)
        desc = f"image->jpeg, {len(raw)} bytes"
        return raw, out_name, desc

    if is_audio_file(path) and suffix != ".mp3":
        print(f"[INFO] Detected audio '{path.name}', converting to MP3...")
        br = mp3_bitrate if mp3_bitrate is not None else "64k"
        raw, out_name = convert_audio_to_mp3(path, bitrate=br)
        desc = f"audio->mp3, {len(raw)} bytes"
        return raw, out_name, desc

    # For text + everything else: just send raw bytes.
    raw = path.read_bytes()
    out_name = path.name
    desc = f"raw bytes, {len(raw)} bytes"
    print(f"[INFO] Sending '{path.name}' as raw bytes (no conversion).")
    return raw, out_name, desc


def wait_for_chunk_done(ser: serial.Serial, chunk_idx: int, chunk_tot: int) -> bool:
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

        print(f"[MCU] {line}")

        if "[TX DONE]" in line:
            print(f"[INFO] MCU reports TX DONE for chunk {chunk_idx+1}/{chunk_tot}")
            ok = True
            break

        if "[ABORT]" in line or "TX FAILED" in line:
            print(f"[WARN] MCU reported failure while sending chunk {chunk_idx+1}/{chunk_tot}")
            warned = True
            ok = False
            break

    if not ok and not warned:
        print(f"[WARN] Timeout waiting for TX DONE for chunk {chunk_idx+1}/{chunk_tot}")

    return ok


def send_file(serial_port: str, file_path: str, baud: int = BAUD_RATE, chunk_size: int = CHUNK_SIZE, jpeg_quality: int = 85, mp3_bitrate: str = "64k") -> None:
    """
    Programmatic API to send a file over LoRa via the TX MCU.
    """
    path = Path(file_path)
    if not path.is_file():
        raise FileNotFoundError(f"file '{path}' not found")

    raw, tx_name, desc = prepare_file_for_lora(path, jpeg_quality=jpeg_quality, mp3_bitrate=mp3_bitrate)
    print(f"[INFO] Final transmit name: {tx_name}")
    print(f"[INFO] Mode: {desc}")

    b64 = base64.b64encode(raw).decode("ascii")
    print(f"[INFO] Base64 length: {len(b64)} characters")

    chunks = [b64[i : i + chunk_size] for i in range(0, len(b64), chunk_size)]
    tot = len(chunks)
    print(f"[INFO] Will send {tot} FILECHUNK lines to MCU")

    print(f"[INFO] Opening serial port {serial_port} @ {baud}...")
    with serial.Serial(serial_port, baud, timeout=1) as ser:
        time.sleep(3.0)

        boot_deadline = time.time() + 3.0
        while time.time() < boot_deadline:
            line = ser.readline().decode(errors="ignore").strip()
            if line:
                print(f"[MCU-BOOT] {line}")

        for idx, chunk in enumerate(chunks):
            print(f"\n=== Sending FILECHUNK {idx+1}/{tot} (len={len(chunk)}) ===")
            payload = f"FILECHUNK:{tx_name}:{idx}:{tot}:{chunk}\n"
            ser.write(payload.encode("utf-8"))
            ser.flush()

            print(f"[INFO] Waiting for MCU to finish chunk {idx+1}/{tot}...")
            ok = wait_for_chunk_done(ser, idx, tot)
            if not ok:
                print("[ERROR] Stopping due to TX failure.")
                break

        print("\n[INFO] All FILECHUNK lines sent (TX side finished).")


def main():
    parser = argparse.ArgumentParser(
        description="Generic LoRa file sender (text, image->JPEG, audio->MP3)."
    )
    parser.add_argument("serial_port", help="Serial port for TX MCU (e.g., COM9 or /dev/ttyUSB0)")
    parser.add_argument("file", help="Path to the file to send")
    parser.add_argument(
        "--baud", type=int, default=BAUD_RATE, help=f"Baud rate (default {BAUD_RATE})"
    )
    parser.add_argument(
        "--chunk-size", type=int, default=CHUNK_SIZE,
        help=f"Base64 characters per FILECHUNK (default {CHUNK_SIZE})",
    )
    parser.add_argument(
        "--jpeg-quality", type=int, default=85,
        help="JPEG quality (1-100, default 85 if image conversion is used)",
    )
    parser.add_argument(
        "--mp3-bitrate", type=str, default="64k",
        help="MP3 bitrate (e.g. '64k', '96k', '128k')",
    )
    args = parser.parse_args()

    try:
        send_file(
            serial_port=args.serial_port,
            file_path=args.file,
            baud=args.baud,
            chunk_size=args.chunk_size,
            jpeg_quality=args.jpeg_quality,
            mp3_bitrate=args.mp3_bitrate,
        )
    except FileNotFoundError as e:
        print(f"Error: {e}")


if __name__ == "__main__":
    main()

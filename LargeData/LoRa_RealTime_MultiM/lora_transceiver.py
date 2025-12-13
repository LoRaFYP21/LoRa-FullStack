#!/usr/bin/env python3
"""
LoRa Serial Transceiver (ONE COM PORT owner)

Fixes the Windows COM-port conflict by opening the serial port exactly once,
while supporting BOTH:
  - Sending FILECHUNK lines to the MCU (TX)
  - Listening for MSG/FRAG lines from the MCU and reassembling files (RX)

MCU expected output lines:
  MSG,src,seq,rssi,d_m,text
  FRAG,src,seq,idx,tot,rssi,d_m,chunk
  plus status lines (e.g., [TX DONE], [ABORT], TX FAILED ...)

MCU expected input lines (from PC to MCU):
  FILECHUNK:<filename>:<idx>:<tot>:<base64_chunk>\n
  or plain text lines for chat (optional)

Usage examples:
  # Just listen + reassemble (default):
  python lora_transceiver.py COM9 --out-dir received_files

  # Send a file AND keep listening:
  python lora_transceiver.py COM9 --send path/to/file.png --out-dir received_files

  # Send a text message as a file:
  python lora_transceiver.py COM9 --send-text "hello world"

Dependencies:
  pip install pyserial
Optional:
  pip install pillow
  pip install pydub
  and install ffmpeg for audio conversion (pydub needs it)
"""

import argparse
import base64
import io
import mimetypes
import queue
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import serial  # pip install pyserial

# Optional conversion libs
try:
    from PIL import Image  # pip install pillow
except Exception:
    Image = None

try:
    from pydub import AudioSegment  # pip install pydub
except Exception:
    AudioSegment = None


# ----------------------------
# Reassembly helpers (same logic as your RX script)
# ----------------------------

class MessageReassembler:
    """Reassembles FRAG messages coming from the RX MCU."""
    def __init__(self):
        # (src, seq) -> {"tot": int, "chunks": {idx: str}}
        self.messages = {}

    def add_frag(self, src: str, seq: int, idx: int, tot: int, chunk: str) -> Optional[str]:
        key = (src, seq)
        if key not in self.messages:
            self.messages[key] = {"tot": tot, "chunks": {}}
        msg = self.messages[key]
        msg["tot"] = tot
        msg["chunks"][idx] = chunk

        if len(msg["chunks"]) == msg["tot"]:
            ordered = [msg["chunks"][i] for i in range(msg["tot"])]
            full_payload = "".join(ordered)
            del self.messages[key]
            return full_payload
        return None


class FileChunkAssembler:
    """Assembles FILECHUNK:<fname>:<idx>:<tot>:<b64> messages into final files."""
    def __init__(self, out_dir: Path):
        self.files = {}  # fname -> {"tot": int, "chunks": {idx: str}}
        self.out_dir = out_dir
        self.out_dir.mkdir(parents=True, exist_ok=True)

    def add_chunk(self, fname: str, idx: int, tot: int, b64_chunk: str) -> None:
        if fname not in self.files:
            self.files[fname] = {"tot": tot, "chunks": {}}
        entry = self.files[fname]
        entry["tot"] = tot
        entry["chunks"][idx] = b64_chunk

        print(f"[INFO] Got FILECHUNK {idx+1}/{tot} for '{fname}'")

        if len(entry["chunks"]) == entry["tot"]:
            ordered = [entry["chunks"][i] for i in range(entry["tot"])]
            full_b64 = "".join(ordered)
            del self.files[fname]

            try:
                raw = base64.b64decode(full_b64)
            except Exception as e:
                print(f"[ERROR] Base64 decode failed for '{fname}': {e}")
                return

            p = Path(fname)
            stamped = f"{p.stem}_rx{p.suffix}"
            out_path = self.out_dir / stamped
            out_path.write_bytes(raw)
            print(f"[OK] Reassembled and wrote {len(raw)} bytes to '{out_path.resolve()}'")

            # If this was a typed text (temporary name), also print its content to the RX log
            try:
                if Path(fname).stem.startswith("_tmp_text_to_send"):
                    txt = raw.decode("utf-8", errors="ignore")
                    print(f"[RX TEXT] Full received text ({len(txt)} chars):\n{txt}")
            except Exception:
                pass


def handle_full_payload(payload: str, file_asm: FileChunkAssembler) -> None:
    """Called when we have a fully reassembled payload from LoRa-level FRAGs."""
    if payload.startswith("FILECHUNK:"):
        # FILECHUNK:<fname>:<idx>:<tot>:<base64_chunk>
        try:
            _, fname, s_idx, s_tot, b64_chunk = payload.split(":", 4)
            idx = int(s_idx)
            tot = int(s_tot)
        except ValueError:
            print("[WARN] FILECHUNK payload format invalid, printing raw:")
            print(payload[:200] + "...")
            return
        file_asm.add_chunk(fname, idx, tot, b64_chunk)
        return

    if payload.startswith("FILE:"):
        # Legacy: FILE:<fname>:<base64>
        try:
            _, fname, b64 = payload.split(":", 2)
        except ValueError:
            print("[WARN] FILE payload format invalid, printing raw:")
            print(payload[:200] + "...")
            return

        print(f"[INFO] Received FILE '{fname}' (base64 length {len(b64)})")
        try:
            raw = base64.b64decode(b64)
        except Exception as e:
            print(f"[ERROR] Base64 decode failed: {e}")
            return

        p = Path(fname)
        stamped = f"{p.stem}_rx{p.suffix}"
        out_path = file_asm.out_dir / stamped
        out_path.write_bytes(raw)
        print(f"[OK] Wrote {len(raw)} bytes to '{out_path.resolve()}'")
        return

    print("[FULL PAYLOAD]", payload[:200] + ("..." if len(payload) > 200 else ""))


# ----------------------------
# TX file conversion + chunking (same logic as your TX script)
# ----------------------------

TEXT_EXT = {".txt", ".csv", ".json", ".text"}
IMAGE_EXT = {".png", ".jpg", ".jpeg", ".bmp", ".gif", ".tif", ".tiff", ".webp"}
AUDIO_EXT = {".wav", ".flac", ".mp3", ".m4a", ".aac", ".ogg", ".wma", ".aif", ".aiff"}

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
    if Image is None:
        raise RuntimeError("Pillow not installed. Run: pip install pillow")
    img = Image.open(path).convert("RGB")
    buf = io.BytesIO()
    img.save(buf, format="JPEG", quality=quality, optimize=True)
    return buf.getvalue(), path.with_suffix(".jpg").name

def convert_audio_to_mp3(path: Path, bitrate: str = "64k") -> tuple[bytes, str]:
    if AudioSegment is None:
        raise RuntimeError(
            "pydub not installed. Run: pip install pydub\n"
            "Also ensure ffmpeg is installed and in PATH."
        )
    audio = AudioSegment.from_file(path)
    buf = io.BytesIO()
    audio.export(buf, format="mp3", bitrate=bitrate)
    return buf.getvalue(), path.with_suffix(".mp3").name

def prepare_file_for_lora(path: Path, jpeg_quality: int = 85, mp3_bitrate: str = "64k") -> tuple[bytes, str, str]:
    suffix = path.suffix.lower()

    if is_image_file(path) and suffix not in (".jpg", ".jpeg"):
        print(f"[INFO] Image '{path.name}' -> JPEG")
        raw, out_name = convert_image_to_jpeg(path, quality=jpeg_quality)
        return raw, out_name, f"image->jpeg ({len(raw)} bytes)"

    if is_audio_file(path) and suffix != ".mp3":
        print(f"[INFO] Audio '{path.name}' -> MP3")
        raw, out_name = convert_audio_to_mp3(path, bitrate=mp3_bitrate)
        return raw, out_name, f"audio->mp3 ({len(raw)} bytes)"

    raw = path.read_bytes()
    return raw, path.name, f"raw bytes ({len(raw)} bytes)"


# ----------------------------
# Serial session: ONE COM owner + background reader
# ----------------------------

@dataclass
class TxResult:
    ok: bool
    reason: str = ""

class LoRaSerialSession:
    """
    Owns the COM port and runs a background reader that:
      - prints all MCU lines
      - extracts MSG/FRAG lines and reassembles
      - signals TX completion events ([TX DONE]/[ABORT]/TX FAILED)
    """
    def __init__(self, port: str, baud: int, out_dir: Path, quiet: bool = False, log_callback: Optional[callable] = None):
        self.port = port
        self.baud = baud
        self.quiet = quiet
        self._log_cb = log_callback

        self.ser: Optional[serial.Serial] = None
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None

        # RX pipeline
        self.reasm = MessageReassembler()
        self.file_asm = FileChunkAssembler(out_dir)

        # TX completion signalling
        self._tx_event = threading.Event()
        self._tx_lock = threading.Lock()
        self._tx_last: Optional[TxResult] = None

    def open(self) -> None:
        self.ser = serial.Serial(self.port, self.baud, timeout=1)
        time.sleep(2.0)

        # Drain boot lines briefly
        boot_deadline = time.time() + 1.5
        while time.time() < boot_deadline:
            line = self._readline()
            if line:
                self._log(f"[MCU-BOOT] {line}")

        self._thread = threading.Thread(target=self._reader_loop, daemon=True)
        self._thread.start()

    def close(self) -> None:
        self._stop.set()
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=1.0)
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None

    def _readline(self) -> str:
        assert self.ser is not None
        try:
            return self.ser.readline().decode(errors="ignore").strip()
        except Exception:
            return ""

    def _write(self, data: bytes) -> None:
        assert self.ser is not None
        self.ser.write(data)
        self.ser.flush()

    def _log(self, s: str) -> None:
        if self._log_cb:
            try:
                self._log_cb(s)
            except Exception:
                # Fallback to stdout if callback fails
                if not self.quiet:
                    print(s)
                return
        if not self._log_cb and not self.quiet:
            print(s)

    def _signal_tx(self, ok: bool, reason: str) -> None:
        with self._tx_lock:
            self._tx_last = TxResult(ok=ok, reason=reason)
            self._tx_event.set()

    def _consume_tx_result(self, timeout_s: float) -> TxResult:
        if not self._tx_event.wait(timeout=timeout_s):
            return TxResult(ok=False, reason="timeout waiting for [TX DONE]")
        with self._tx_lock:
            r = self._tx_last or TxResult(ok=False, reason="unknown")
            # reset for next chunk
            self._tx_last = None
            self._tx_event.clear()
            return r

    def _handle_rx_line(self, line: str) -> None:
        # MSG,src,seq,rssi,d_m,text
        if line.startswith("MSG,"):
            parts = line.split(",", 5)
            if len(parts) >= 6:
                _, src, seq, rssi, d_m, text = parts
                self._log(f"[MSG] src={src} seq={seq} rssi={rssi} d~{d_m}m text='{text[:60]}'")
                handle_full_payload(text, self.file_asm)
            return

        # FRAG,src,seq,idx,tot,rssi,d_m,chunk
        if line.startswith("FRAG,"):
            parts = line.split(",", 7)
            if len(parts) >= 8:
                _, src, seq, idx, tot, rssi, d_m, chunk = parts
                try:
                    seq_i = int(seq); idx_i = int(idx); tot_i = int(tot)
                except ValueError:
                    self._log(f"[WARN] Bad FRAG ints: {line[:120]}")
                    return
                full = self.reasm.add_frag(src, seq_i, idx_i, tot_i, chunk)
                if full is not None:
                    self._log(f"[INFO] Full payload src={src} seq={seq_i} len={len(full)}")
                    handle_full_payload(full, self.file_asm)
            return

    def _reader_loop(self) -> None:
        while not self._stop.is_set():
            line = self._readline()
            if not line:
                continue

            # Always print MCU lines (unless quiet)
            if not self.quiet and not (line.startswith("MSG,") or line.startswith("FRAG,")):
                print(f"[MCU] {line}")

            # TX completion markers
            if "[TX DONE]" in line:
                self._signal_tx(ok=True, reason="TX DONE")
                continue
            if "[ABORT]" in line or "TX FAILED" in line:
                self._signal_tx(ok=False, reason=line)
                continue

            # RX parsing
            self._handle_rx_line(line)

    # ----------------------------
    # Public TX APIs (use same serial connection)
    # ----------------------------

    def send_file(self, file_path: Path, chunk_size_chars: int = 40000,
                  jpeg_quality: int = 85, mp3_bitrate: str = "64k",
                  chunk_timeout_s: float = 300.0) -> bool:
        raw, tx_name, desc = prepare_file_for_lora(file_path, jpeg_quality=jpeg_quality, mp3_bitrate=mp3_bitrate)
        self._log(f"[INFO] Final transmit name: {tx_name}")
        self._log(f"[INFO] Mode: {desc}")

        b64 = base64.b64encode(raw).decode("ascii")
        self._log(f"[INFO] Base64 length: {len(b64)}")

        chunks = [b64[i:i + chunk_size_chars] for i in range(0, len(b64), chunk_size_chars)]
        tot = len(chunks)
        self._log(f"[INFO] Will send {tot} FILECHUNK lines")

        for idx, chunk in enumerate(chunks):
            self._log(f"\n=== Sending FILECHUNK {idx+1}/{tot} (len={len(chunk)}) ===")
            payload = f"FILECHUNK:{tx_name}:{idx}:{tot}:{chunk}\n"
            self._write(payload.encode("utf-8"))

            # Wait for MCU TX completion for THIS chunk
            r = self._consume_tx_result(timeout_s=chunk_timeout_s)
            if not r.ok:
                self._log(f"[ERROR] Chunk {idx+1}/{tot} failed: {r.reason}")
                return False

        self._log("[OK] All FILECHUNK lines sent.")
        return True

    def send_text_as_file(self, text: str, tmp_name: str = "_tmp_text_to_send.txt",
                          **kwargs) -> bool:
        tmp = Path(tmp_name)
        tmp.write_bytes(text.encode("utf-8"))
        try:
            self._log(f"[TX] Text preview ({len(text)} chars): '{text[:120]}'")
            return self.send_file(tmp, **kwargs)
        finally:
            try:
                tmp.unlink()
            except Exception:
                pass

    def send_raw_line(self, line: str, wait_done: bool = False, timeout_s: float = 30.0) -> bool:
        """
        Optional: send a raw text line to MCU.
        If your MCU prints [TX DONE] for each sent line, wait_done=True works.
        """
        self._write((line.strip() + "\n").encode("utf-8"))
        if not wait_done:
            return True
        r = self._consume_tx_result(timeout_s=timeout_s)
        return r.ok


# ----------------------------
# CLI
# ----------------------------

def main():
    ap = argparse.ArgumentParser(description="One-port LoRa transceiver (TX+RX together).")
    ap.add_argument("serial_port", help="COM port (e.g., COM9 or /dev/ttyUSB0)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--out-dir", type=str, default="received_files")
    ap.add_argument("--quiet", action="store_true", help="Reduce console logging")

    # TX options
    ap.add_argument("--send", type=str, default="", help="File path to send (optional)")
    ap.add_argument("--send-text", type=str, default="", help="Send this text as a file (optional)")
    ap.add_argument("--chunk-size", type=int, default=40000, help="Base64 characters per FILECHUNK")
    ap.add_argument("--chunk-timeout", type=float, default=300.0, help="Seconds to wait per FILECHUNK")
    ap.add_argument("--jpeg-quality", type=int, default=85)
    ap.add_argument("--mp3-bitrate", type=str, default="64k")

    # Keep alive listening
    ap.add_argument("--exit-after-send", action="store_true", help="Exit after sending completes")
    args = ap.parse_args()

    out_dir = Path(args.out_dir)
    sess = LoRaSerialSession(args.serial_port, args.baud, out_dir=out_dir, quiet=args.quiet)

    print(f"[INFO] Opening {args.serial_port} @ {args.baud} (ONE owner)...")
    sess.open()

    try:
        # If send requested, do it
        did_send = False

        if args.send:
            path = Path(args.send)
            if not path.is_file():
                print(f"[ERROR] File not found: {path}")
            else:
                did_send = True
                ok = sess.send_file(
                    path,
                    chunk_size_chars=args.chunk_size,
                    jpeg_quality=args.jpeg_quality,
                    mp3_bitrate=args.mp3_bitrate,
                    chunk_timeout_s=args.chunk_timeout
                )
                print("[RESULT] SEND FILE:", "OK" if ok else "FAILED")

        if args.send_text:
            did_send = True
            ok = sess.send_text_as_file(
                args.send_text,
                chunk_size_chars=args.chunk_size,
                jpeg_quality=args.jpeg_quality,
                mp3_bitrate=args.mp3_bitrate,
                chunk_timeout_s=args.chunk_timeout
            )
            print("[RESULT] SEND TEXT:", "OK" if ok else "FAILED")

        if args.exit_after_send and did_send:
            return

        print("[INFO] Listening... Press Ctrl+C to exit.")
        while True:
            time.sleep(0.2)

    except KeyboardInterrupt:
        print("\n[INFO] Exiting.")
    finally:
        sess.close()


if __name__ == "__main__":
    main()

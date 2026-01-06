"""
Tkinter control panel for the multi-media LoRa demo.
- Wraps `LoRaSerialSession` so TX/RX share a single COM port.
- Lets the user pick ports, chunk sizes, and launch audio/image/text/file sends.
"""

import threading
import tkinter as tk
from tkinter import ttk, filedialog, messagebox
from pathlib import Path

from lora_transceiver import LoRaSerialSession
from serial.tools import list_ports


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("LoRa File TX/RX (One-Port)")
        self.geometry("900x650")

        self.port_var = tk.StringVar(value="COM9")
        self.baud_var = tk.IntVar(value=115200)
        self.out_dir_var = tk.StringVar(value="received_files")

        self.jpeg_quality_var = tk.IntVar(value=85)
        self.mp3_bitrate_var = tk.StringVar(value="64k")
        self.chunk_size_var = tk.IntVar(value=40000)
        self.chunk_timeout_var = tk.DoubleVar(value=300.0)

        # Audio capture controls
        self.audio_sr_var = tk.IntVar(value=16000)
        self.audio_dur_var = tk.IntVar(value=5)

        self.file_path_var = tk.StringVar()

        self.sess = None

        self._build_ui()

    def _build_ui(self):
        # Root uses PACK for one main container only
        frm = ttk.Frame(self)
        frm.pack(fill=tk.BOTH, expand=True, padx=10, pady=10)

        # ---------------- Connection ----------------
        conn = ttk.LabelFrame(frm, text="Connection (ONE COM owner)")
        conn.grid(row=0, column=0, sticky="we")
        frm.columnconfigure(0, weight=1)

        ttk.Label(conn, text="Serial Port").grid(row=0, column=0, sticky="w")
        self.port_combo = ttk.Combobox(conn, textvariable=self.port_var, width=12, state="readonly")
        self.port_combo.grid(row=0, column=1, sticky="w", padx=5)

        ttk.Label(conn, text="Baud").grid(row=0, column=2, sticky="w")
        ttk.Entry(conn, textvariable=self.baud_var, width=10).grid(row=0, column=3, sticky="w", padx=5)

        ttk.Label(conn, text="Out Dir").grid(row=0, column=4, sticky="w")
        ttk.Entry(conn, textvariable=self.out_dir_var, width=30).grid(row=0, column=5, sticky="we", padx=5)
        ttk.Button(conn, text="Browse", command=self.choose_dir).grid(row=0, column=6, sticky="w")

        # Single toggle button for connect/disconnect
        self.connect_btn = ttk.Button(conn, text="Connect", command=self.connect)
        self.connect_btn.grid(row=1, column=5, sticky="e", pady=5)
        # Spacer to keep layout
        ttk.Label(conn, text="").grid(row=1, column=6, sticky="w", pady=5)
        ttk.Button(conn, text="Refresh Ports", command=self.refresh_ports).grid(row=1, column=4, sticky="w", pady=5)

        conn.columnconfigure(5, weight=1)

        # ---------------- TX controls ----------------
        tx = ttk.LabelFrame(frm, text="Transmit")
        tx.grid(row=1, column=0, sticky="we", pady=10)
        # Make multiple TX columns responsive
        for c in range(0, 12):
            tx.columnconfigure(c, weight=1)

        ttk.Label(tx, text="JPEG Quality").grid(row=0, column=0, sticky="w")
        ttk.Entry(tx, textvariable=self.jpeg_quality_var, width=6).grid(row=0, column=1, sticky="w", padx=5)

        ttk.Label(tx, text="MP3 Bitrate").grid(row=0, column=2, sticky="w")
        ttk.Entry(tx, textvariable=self.mp3_bitrate_var, width=8).grid(row=0, column=3, sticky="w", padx=5)

        ttk.Label(tx, text="Chunk Size (b64 chars)").grid(row=0, column=4, sticky="w")
        ttk.Entry(tx, textvariable=self.chunk_size_var, width=10).grid(row=0, column=5, sticky="w", padx=5)

        ttk.Label(tx, text="Chunk Timeout (s)").grid(row=0, column=6, sticky="w")
        ttk.Entry(tx, textvariable=self.chunk_timeout_var, width=10).grid(row=0, column=7, sticky="w", padx=5)

        # Audio record settings
        ttk.Label(tx, text="Record SR (Hz)").grid(row=0, column=8, sticky="w")
        ttk.Entry(tx, textvariable=self.audio_sr_var, width=8).grid(row=0, column=9, sticky="w", padx=5)
        ttk.Label(tx, text="Duration (s)").grid(row=0, column=10, sticky="w")
        ttk.Entry(tx, textvariable=self.audio_dur_var, width=6).grid(row=0, column=11, sticky="w", padx=5)

        ttk.Label(tx, text="File to Send").grid(row=1, column=0, sticky="w")
        ttk.Entry(tx, textvariable=self.file_path_var, width=60).grid(
            row=1, column=1, columnspan=6, sticky="we", padx=5
        )
        ttk.Button(tx, text="Browse", command=self.choose_file).grid(row=1, column=7, sticky="w")

        ttk.Label(tx, text="Or type text").grid(row=2, column=0, sticky="nw")

        # IMPORTANT: parent must be tx (NOT self)
        self.text_input = tk.Text(tx, height=6)
        self.text_input.grid(row=2, column=1, columnspan=7, sticky="we", padx=5, pady=5)

        self.send_file_btn = ttk.Button(tx, text="Send File", command=self.on_send_file)
        self.send_file_btn.grid(row=3, column=6, sticky="e")
        self.send_text_btn = ttk.Button(tx, text="Send Text", command=self.on_send_text)
        self.send_text_btn.grid(row=3, column=7, sticky="w")

        # Capture / Record controls
        self.capture_btn = ttk.Button(tx, text="Capture Photo", command=self.on_capture_photo)
        self.capture_btn.grid(row=4, column=6, sticky="e", pady=6)
        self.record_btn = ttk.Button(tx, text="Record Voice", command=self.on_record_voice)
        self.record_btn.grid(row=4, column=7, sticky="w", pady=6)

        # ---------------- Log ----------------
        lg = ttk.LabelFrame(frm, text="Log")
        lg.grid(row=2, column=0, sticky="nsew")
        frm.rowconfigure(2, weight=1)
        lg.rowconfigure(0, weight=1)
        lg.columnconfigure(0, weight=1)

        # Text log with scrollbar for better responsiveness
        log_frame = ttk.Frame(lg)
        log_frame.grid(row=0, column=0, sticky="nsew")
        log_frame.rowconfigure(0, weight=1)
        log_frame.columnconfigure(0, weight=1)

        self.log = tk.Text(log_frame, height=18, wrap="word")
        self.log.grid(row=0, column=0, sticky="nsew", padx=5, pady=5)
        scroll = ttk.Scrollbar(log_frame, orient="vertical", command=self.log.yview)
        scroll.grid(row=0, column=1, sticky="ns")
        self.log.configure(yscrollcommand=scroll.set)

        self._log("[INFO] Ready. Click Connect to start listening.\n")
        # Initially disable TX actions until connected
        for b in (getattr(self, 'send_file_btn', None), getattr(self, 'send_text_btn', None), getattr(self, 'capture_btn', None), getattr(self, 'record_btn', None)):
            if b:
                b.state(["disabled"])  # disable

        # Populate ports on startup
        self.refresh_ports()

    def _log(self, s: str):
        # Append and auto-scroll
        self.log.insert(tk.END, s)
        self.log.see(tk.END)
        # Trim log to last ~5000 lines to keep UI snappy
        try:
            line_count = int(self.log.index('end-1c').split('.')[0])
            if line_count > 5000:
                # delete from start to keep roughly last 4000 lines
                self.log.delete('1.0', f"{line_count-4000}.0")
        except Exception:
            pass

    def choose_dir(self):
        d = filedialog.askdirectory()
        if d:
            self.out_dir_var.set(d)

    def refresh_ports(self):
        try:
            ports = [p.device for p in list_ports.comports() if p.device]
        except Exception as e:
            self._log(f"[WARN] Could not list ports: {e}\n")
            ports = []

        self.port_combo["values"] = ports
        cur = self.port_var.get().strip()
        if ports:
            # Select current if valid, else first
            if cur in ports:
                self.port_combo.set(cur)
            else:
                self.port_combo.set(ports[0])
                self.port_var.set(ports[0])
        else:
            self.port_combo.set("")
            self.port_var.set("")
        self._log(f"[INFO] Ports: {', '.join(ports) if ports else 'none'}\n")

    def choose_file(self):
        p = filedialog.askopenfilename()
        if p:
            self.file_path_var.set(p)

    def connect(self):
        if self.sess is not None:
            messagebox.showinfo("Info", "Already connected.")
            return

        port = self.port_var.get().strip()
        if not port:
            messagebox.showerror("No port", "No serial ports detected. Click 'Refresh Ports' and select one.")
            return
        baud = int(self.baud_var.get())
        out_dir = Path(self.out_dir_var.get())

        try:
            # Provide a log callback so MCU and TX/RX logs appear instantly in GUI
            def gui_log(msg: str):
                # Ensure UI updates happen on the main thread
                self.after(0, lambda: self._log(msg + ("\n" if not msg.endswith("\n") else "")))

            self.sess = LoRaSerialSession(port, baud, out_dir=out_dir, quiet=False, log_callback=gui_log)
            self.sess.open()
            self._log(f"[INFO] Connected to {port} @ {baud}. Listening started.\n")
            # Toggle UI to connected state
            self.connect_btn.configure(text="Disconnect", command=self.disconnect)
            for b in (self.send_file_btn, self.send_text_btn, self.capture_btn, self.record_btn):
                b.state(["!disabled"])  # enable
        except Exception as e:
            self.sess = None
            messagebox.showerror("Connect failed", str(e))

    def disconnect(self):
        if self.sess is None:
            return
        try:
            self.sess.close()
        finally:
            self.sess = None
            self._log("[INFO] Disconnected.\n")
            # Toggle UI to disconnected state
            self.connect_btn.configure(text="Connect", command=self.connect)
            for b in (self.send_file_btn, self.send_text_btn, self.capture_btn, self.record_btn):
                b.state(["disabled"])  # disable

    def on_send_file(self):
        if self.sess is None:
            messagebox.showerror("Not connected", "Connect first.")
            return

        path = self.file_path_var.get().strip()
        if not path:
            messagebox.showerror("Error", "Choose a file first.")
            return

        def worker():
            try:
                self._log(f"[TX] Preparing '{Path(path).name}' for send...\n")
                ok = self.sess.send_file(
                    Path(path),
                    chunk_size_chars=int(self.chunk_size_var.get()),
                    jpeg_quality=int(self.jpeg_quality_var.get()),
                    mp3_bitrate=str(self.mp3_bitrate_var.get()),
                    chunk_timeout_s=float(self.chunk_timeout_var.get()),
                )
                self._log(f"[RESULT] Send file: {'OK' if ok else 'FAILED'}\n")
            except Exception as e:
                messagebox.showerror("Send failed", str(e))

        threading.Thread(target=worker, daemon=True).start()

    def on_send_text(self):
        if self.sess is None:
            messagebox.showerror("Not connected", "Connect first.")
            return

        text = self.text_input.get("1.0", tk.END).strip()
        if not text:
            messagebox.showerror("Error", "Type some text.")
            return

        def worker():
            try:
                self._log("[TX] Preparing text payload for send...\n")
                ok = self.sess.send_text_as_file(
                    text,
                    chunk_size_chars=int(self.chunk_size_var.get()),
                    jpeg_quality=int(self.jpeg_quality_var.get()),
                    mp3_bitrate=str(self.mp3_bitrate_var.get()),
                    chunk_timeout_s=float(self.chunk_timeout_var.get()),
                )
                self._log(f"[RESULT] Send text: {'OK' if ok else 'FAILED'}\n")
            except Exception as e:
                messagebox.showerror("Send failed", str(e))

        threading.Thread(target=worker, daemon=True).start()

    def on_capture_photo(self):
        if self.sess is None:
            messagebox.showerror("Not connected", "Connect first.")
            return

        def worker():
            try:
                self._log("[TX] Opening camera to capture photo...\n")
                import cv2  # pip install opencv-python
                cap = cv2.VideoCapture(0)
                ok, frame = cap.read()
                cap.release()
                if not ok:
                    raise RuntimeError("Failed to capture from camera")
                tmp_path = Path("_tmp_capture.png")
                # Save PNG to preserve data; conversion to JPEG happens in send_file
                cv2.imwrite(str(tmp_path), frame)
                self._log("[TX] Photo captured. Sending...\n")
                ok = self.sess.send_file(
                    tmp_path,
                    chunk_size_chars=int(self.chunk_size_var.get()),
                    jpeg_quality=int(self.jpeg_quality_var.get()),
                    mp3_bitrate=str(self.mp3_bitrate_var.get()),
                    chunk_timeout_s=float(self.chunk_timeout_var.get()),
                )
                self._log(f"[RESULT] Capture photo send: {'OK' if ok else 'FAILED'}\n")
            except Exception as e:
                messagebox.showerror("Capture failed", str(e))
            finally:
                try:
                    Path("_tmp_capture.png").unlink()
                except Exception:
                    pass

        threading.Thread(target=worker, daemon=True).start()

    def on_record_voice(self):
        if self.sess is None:
            messagebox.showerror("Not connected", "Connect first.")
            return

        # Use UI-configured recording parameters
        duration_s = int(self.audio_dur_var.get())
        sample_rate = int(self.audio_sr_var.get())

        def worker():
            try:
                self._log(f"[TX] Recording voice {duration_s}s @ {sample_rate}Hz...\n")
                import sounddevice as sd  # pip install sounddevice
                import numpy as np
                from scipy.io.wavfile import write  # pip install scipy

                data = sd.rec(int(duration_s * sample_rate), samplerate=sample_rate, channels=1, dtype='int16')
                sd.wait()

                tmp_wav = Path("_tmp_record.wav")
                write(str(tmp_wav), sample_rate, data)

                self._log("[TX] Voice recorded. Sending...\n")
                ok = self.sess.send_file(
                    tmp_wav,
                    chunk_size_chars=int(self.chunk_size_var.get()),
                    jpeg_quality=int(self.jpeg_quality_var.get()),
                    mp3_bitrate=str(self.mp3_bitrate_var.get()),
                    chunk_timeout_s=float(self.chunk_timeout_var.get()),
                )
                self._log(f"[RESULT] Voice send: {'OK' if ok else 'FAILED'}\n")
            except Exception as e:
                messagebox.showerror("Record failed", str(e))
            finally:
                try:
                    Path("_tmp_record.wav").unlink()
                except Exception:
                    pass

        threading.Thread(target=worker, daemon=True).start()


if __name__ == "__main__":
    App().mainloop()

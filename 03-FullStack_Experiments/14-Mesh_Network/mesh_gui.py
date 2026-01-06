#!/usr/bin/env python3
"""
LoRa Intelligent Mesh Network - GUI Application

User-friendly GUI for sending and receiving data through the mesh network.

Features:
- Port selection with auto-detect
- Destination node selection
- Reliability level selection
- File/text/voice/image sending
- Real-time progress tracking
- Network monitoring
- Received files display
- Route discovery
- Statistics dashboard

Dependencies:
    pip install pyserial
"""

import os
import sys
import threading
import time
import tkinter as tk
from pathlib import Path
from tkinter import ttk, filedialog, messagebox, scrolledtext
from typing import Optional

import serial
from serial.tools import list_ports

# Import our mesh network interface
try:
    from mesh_network_interface import MeshNetworkInterface, REL_NONE, REL_LOW, REL_MEDIUM, REL_HIGH, REL_CRITICAL
except ImportError:
    messagebox.showerror("Import Error", "Could not import mesh_network_interface.py\nMake sure it's in the same directory!")
    sys.exit(1)


class MeshNetworkGUI(tk.Tk):
    """Main GUI application for mesh network"""
    
    def __init__(self):
        super().__init__()
        
        self.title("LoRa Intelligent Mesh Network - Control Panel")
        self.geometry("1000x800")
        self.resizable(True, True)
        
        # Variables
        self.port_var = tk.StringVar(value="COM9")
        self.baud_var = tk.IntVar(value=115200)
        self.dest_node_var = tk.StringVar(value="Node_2")
        self.reliability_var = tk.IntVar(value=REL_MEDIUM)
        self.out_dir_var = tk.StringVar(value="received_files")
        self.file_path_var = tk.StringVar()
        
        # Connection state
        self.interface: Optional[MeshNetworkInterface] = None
        self.connected = False
        self.monitoring = False
        self.monitor_thread = None
        
        # Build UI
        self._build_ui()
        
        # Populate ports on startup
        self.refresh_ports()
    
    def _build_ui(self):
        """Build the user interface"""
        
        # Main container with padding
        main_frame = ttk.Frame(self, padding="10")
        main_frame.grid(row=0, column=0, sticky="nsew")
        
        self.grid_rowconfigure(0, weight=1)
        self.grid_columnconfigure(0, weight=1)
        
        # Configure grid weights
        main_frame.grid_rowconfigure(3, weight=1)  # Log area
        main_frame.grid_columnconfigure(0, weight=1)
        
        # ============ Connection Section ============
        conn_frame = ttk.LabelFrame(main_frame, text="📡 Connection Settings", padding="10")
        conn_frame.grid(row=0, column=0, sticky="ew", pady=(0, 10))
        
        # Port selection
        ttk.Label(conn_frame, text="Serial Port:").grid(row=0, column=0, sticky="w", padx=(0, 5))
        self.port_combo = ttk.Combobox(conn_frame, textvariable=self.port_var, width=15, state="readonly")
        self.port_combo.grid(row=0, column=1, sticky="w", padx=(0, 10))
        
        ttk.Button(conn_frame, text="🔄 Refresh", command=self.refresh_ports, width=10).grid(
            row=0, column=2, sticky="w", padx=(0, 20))
        
        # Baud rate
        ttk.Label(conn_frame, text="Baud:").grid(row=0, column=3, sticky="w", padx=(0, 5))
        ttk.Entry(conn_frame, textvariable=self.baud_var, width=10).grid(
            row=0, column=4, sticky="w", padx=(0, 20))
        
        # Connect/Disconnect button
        self.connect_btn = ttk.Button(conn_frame, text="🔌 Connect", command=self.toggle_connection, width=15)
        self.connect_btn.grid(row=0, column=5, sticky="e", padx=(20, 0))
        
        conn_frame.grid_columnconfigure(5, weight=1)
        
        # ============ Destination & Reliability Section ============
        dest_frame = ttk.LabelFrame(main_frame, text="🎯 Destination & Reliability", padding="10")
        dest_frame.grid(row=1, column=0, sticky="ew", pady=(0, 10))
        
        # Destination node
        ttk.Label(dest_frame, text="Destination Node:").grid(row=0, column=0, sticky="w", padx=(0, 5))
        dest_entry = ttk.Entry(dest_frame, textvariable=self.dest_node_var, width=15)
        dest_entry.grid(row=0, column=1, sticky="w", padx=(0, 10))
        
        # Common destinations (quick select)
        ttk.Label(dest_frame, text="Quick:").grid(row=0, column=2, sticky="w", padx=(10, 5))
        for i, node in enumerate(["Node_1", "Node_2", "Node_3", "Node_4"]):
            ttk.Button(dest_frame, text=node, width=8,
                      command=lambda n=node: self.dest_node_var.set(n)).grid(
                row=0, column=3+i, sticky="w", padx=2)
        
        # Reliability level
        ttk.Label(dest_frame, text="Reliability:").grid(row=1, column=0, sticky="w", padx=(0, 5), pady=(10, 0))
        
        rel_frame = ttk.Frame(dest_frame)
        rel_frame.grid(row=1, column=1, columnspan=6, sticky="w", pady=(10, 0))
        
        rel_options = [
            ("None (0)", REL_NONE, "Fire & forget"),
            ("Low (1)", REL_LOW, "Text, voice"),
            ("Medium (2)", REL_MEDIUM, "Images"),
            ("High (3)", REL_HIGH, "Files"),
            ("Critical (4)", REL_CRITICAL, "MiniSEED")
        ]
        
        for i, (label, value, desc) in enumerate(rel_options):
            rb = ttk.Radiobutton(rel_frame, text=label, variable=self.reliability_var, value=value)
            rb.grid(row=0, column=i*2, sticky="w", padx=(0, 5))
            ttk.Label(rel_frame, text=f"({desc})", foreground="gray").grid(
                row=0, column=i*2+1, sticky="w", padx=(0, 15))
        
        # ============ Transmit Section ============
        tx_frame = ttk.LabelFrame(main_frame, text="📤 Transmit", padding="10")
        tx_frame.grid(row=2, column=0, sticky="ew", pady=(0, 10))
        
        # File selection
        ttk.Label(tx_frame, text="File to Send:").grid(row=0, column=0, sticky="w", padx=(0, 5))
        ttk.Entry(tx_frame, textvariable=self.file_path_var, width=60).grid(
            row=0, column=1, sticky="ew", padx=(0, 5))
        ttk.Button(tx_frame, text="📁 Browse", command=self.choose_file, width=12).grid(
            row=0, column=2, sticky="w")
        
        tx_frame.grid_columnconfigure(1, weight=1)
        
        # Send buttons for file
        btn_frame1 = ttk.Frame(tx_frame)
        btn_frame1.grid(row=1, column=1, columnspan=2, sticky="e", pady=(10, 0))
        
        self.send_file_btn = ttk.Button(btn_frame1, text="📤 Send File", 
                                        command=self.send_file, width=15, state="disabled")
        self.send_file_btn.pack(side="left", padx=5)
        
        # Text message
        ttk.Label(tx_frame, text="Text Message:").grid(row=2, column=0, sticky="nw", padx=(0, 5), pady=(10, 0))
        
        text_frame = ttk.Frame(tx_frame)
        text_frame.grid(row=2, column=1, columnspan=2, sticky="ew", pady=(10, 0))
        text_frame.grid_columnconfigure(0, weight=1)
        
        self.text_input = scrolledtext.ScrolledText(text_frame, height=4, wrap="word")
        self.text_input.grid(row=0, column=0, sticky="ew", padx=(0, 5))
        
        self.send_text_btn = ttk.Button(text_frame, text="📤 Send Text", 
                                        command=self.send_text, width=15, state="disabled")
        self.send_text_btn.grid(row=0, column=1, sticky="n")
        
        # Quick actions
        quick_frame = ttk.Frame(tx_frame)
        quick_frame.grid(row=3, column=0, columnspan=3, sticky="ew", pady=(10, 0))
        
        ttk.Label(quick_frame, text="Quick Actions:").pack(side="left", padx=(0, 10))
        
        self.discover_btn = ttk.Button(quick_frame, text="🔍 Discover Route", 
                                       command=self.discover_route, width=15, state="disabled")
        self.discover_btn.pack(side="left", padx=5)
        
        self.show_routes_btn = ttk.Button(quick_frame, text="🗺️ Show Routes", 
                                          command=self.show_routes, width=15, state="disabled")
        self.show_routes_btn.pack(side="left", padx=5)
        
        self.show_stats_btn = ttk.Button(quick_frame, text="📊 Statistics", 
                                         command=self.show_stats, width=15, state="disabled")
        self.show_stats_btn.pack(side="left", padx=5)
        
        self.monitor_btn = ttk.Button(quick_frame, text="👁️ Monitor Network", 
                                      command=self.toggle_monitoring, width=18, state="disabled")
        self.monitor_btn.pack(side="left", padx=5)
        
        # ============ Log Section ============
        log_frame = ttk.LabelFrame(main_frame, text="📋 Activity Log", padding="10")
        log_frame.grid(row=3, column=0, sticky="nsew", pady=(0, 10))
        log_frame.grid_rowconfigure(0, weight=1)
        log_frame.grid_columnconfigure(0, weight=1)
        
        # Log text widget with scrollbar
        self.log = scrolledtext.ScrolledText(log_frame, wrap="word", height=15)
        self.log.grid(row=0, column=0, sticky="nsew")
        
        # Configure tags for colored output
        self.log.tag_config("info", foreground="blue")
        self.log.tag_config("success", foreground="green")
        self.log.tag_config("warning", foreground="orange")
        self.log.tag_config("error", foreground="red")
        self.log.tag_config("tx", foreground="purple")
        self.log.tag_config("rx", foreground="teal")
        
        # ============ Status Bar ============
        status_frame = ttk.Frame(main_frame)
        status_frame.grid(row=4, column=0, sticky="ew")
        
        self.status_label = ttk.Label(status_frame, text="⭕ Not Connected", relief="sunken", anchor="w")
        self.status_label.pack(side="left", fill="x", expand=True)
        
        self.progress_bar = ttk.Progressbar(status_frame, mode="indeterminate", length=200)
        self.progress_bar.pack(side="right", padx=(10, 0))
        
        # Initial log message
        self._log("Welcome to LoRa Intelligent Mesh Network Control Panel\n", "info")
        self._log("Please connect to your mesh node to begin.\n", "info")
    
    def _log(self, message: str, tag: str = ""):
        """Add message to log with optional color tag"""
        timestamp = time.strftime("%H:%M:%S")
        full_msg = f"[{timestamp}] {message}"
        
        self.log.insert("end", full_msg, tag)
        self.log.see("end")
        self.log.update()
    
    def refresh_ports(self):
        """Refresh available COM ports"""
        ports = [port.device for port in list_ports.comports()]
        self.port_combo["values"] = ports if ports else ["No ports found"]
        
        if ports and self.port_var.get() not in ports:
            self.port_var.set(ports[0])
        
        self._log(f"Found {len(ports)} serial port(s)\n", "info")
    
    def toggle_connection(self):
        """Connect or disconnect from mesh node"""
        if not self.connected:
            self.connect()
        else:
            self.disconnect()
    
    def connect(self):
        """Connect to mesh node"""
        port = self.port_var.get()
        baud = self.baud_var.get()
        
        if not port or port == "No ports found":
            messagebox.showerror("Error", "Please select a valid serial port")
            return
        
        try:
            self._log(f"Connecting to {port} @ {baud} baud...\n", "info")
            self.progress_bar.start()
            
            # Create interface
            self.interface = MeshNetworkInterface(port, baud)
            
            if self.interface.connect():
                self.connected = True
                self.connect_btn.config(text="🔌 Disconnect")
                self.status_label.config(text=f"✅ Connected to {port}")
                
                # Enable TX controls
                self.send_file_btn.config(state="normal")
                self.send_text_btn.config(state="normal")
                self.discover_btn.config(state="normal")
                self.show_routes_btn.config(state="normal")
                self.show_stats_btn.config(state="normal")
                self.monitor_btn.config(state="normal")
                
                self._log(f"Successfully connected to {port}\n", "success")
                self._log("Ready to send messages and files!\n", "success")
            else:
                self._log("Connection failed\n", "error")
                messagebox.showerror("Connection Error", "Failed to connect to mesh node")
        
        except Exception as e:
            self._log(f"Connection error: {e}\n", "error")
            messagebox.showerror("Error", f"Connection failed:\n{e}")
        
        finally:
            self.progress_bar.stop()
    
    def disconnect(self):
        """Disconnect from mesh node"""
        if self.monitoring:
            self.toggle_monitoring()
        
        if self.interface:
            self.interface.disconnect()
            self.interface = None
        
        self.connected = False
        self.connect_btn.config(text="🔌 Connect")
        self.status_label.config(text="⭕ Not Connected")
        
        # Disable TX controls
        self.send_file_btn.config(state="disabled")
        self.send_text_btn.config(state="disabled")
        self.discover_btn.config(state="disabled")
        self.show_routes_btn.config(state="disabled")
        self.show_stats_btn.config(state="disabled")
        self.monitor_btn.config(state="disabled")
        
        self._log("Disconnected from mesh node\n", "warning")
    
    def choose_file(self):
        """Open file dialog to select file"""
        filename = filedialog.askopenfilename(
            title="Select file to send",
            filetypes=[
                ("All files", "*.*"),
                ("Images", "*.jpg *.jpeg *.png *.gif *.bmp"),
                ("Audio", "*.wav *.mp3 *.ogg"),
                ("MiniSEED", "*.mseed *.miniseed"),
                ("Text", "*.txt *.log"),
                ("Documents", "*.pdf *.doc *.docx")
            ]
        )
        
        if filename:
            self.file_path_var.set(filename)
            self._log(f"Selected file: {Path(filename).name}\n", "info")
    
    def send_file(self):
        """Send selected file"""
        if not self.connected:
            messagebox.showerror("Error", "Not connected to mesh node")
            return
        
        filepath = self.file_path_var.get()
        if not filepath or not Path(filepath).exists():
            messagebox.showerror("Error", "Please select a valid file")
            return
        
        dest = self.dest_node_var.get().strip()
        if not dest:
            messagebox.showerror("Error", "Please enter destination node name")
            return
        
        reliability = self.reliability_var.get()
        
        # Disable UI during send
        self._set_tx_state("disabled")
        self.progress_bar.start()
        
        # Run in thread to avoid blocking UI
        def send_thread():
            try:
                self._log(f"\n{'='*60}\n", "tx")
                self._log(f"📤 Sending file: {Path(filepath).name}\n", "tx")
                self._log(f"   Destination: {dest}\n", "tx")
                self._log(f"   Reliability: {reliability}\n", "tx")
                self._log(f"{'='*60}\n", "tx")
                
                success = self.interface.send_file(dest, filepath, reliability)
                
                if success:
                    self._log(f"\n✅ File sent successfully!\n", "success")
                    messagebox.showinfo("Success", f"File sent to {dest}")
                else:
                    self._log(f"\n❌ File send failed\n", "error")
                    messagebox.showerror("Failed", f"Failed to send file to {dest}")
            
            except Exception as e:
                self._log(f"\n❌ Error: {e}\n", "error")
                messagebox.showerror("Error", f"Send failed:\n{e}")
            
            finally:
                self.progress_bar.stop()
                self._set_tx_state("normal")
        
        threading.Thread(target=send_thread, daemon=True).start()
    
    def send_text(self):
        """Send text message"""
        if not self.connected:
            messagebox.showerror("Error", "Not connected to mesh node")
            return
        
        text = self.text_input.get("1.0", "end-1c").strip()
        if not text:
            messagebox.showerror("Error", "Please enter text message")
            return
        
        dest = self.dest_node_var.get().strip()
        if not dest:
            messagebox.showerror("Error", "Please enter destination node name")
            return
        
        reliability = self.reliability_var.get()
        
        # Disable UI during send
        self._set_tx_state("disabled")
        self.progress_bar.start()
        
        # Run in thread
        def send_thread():
            try:
                self._log(f"\n{'='*60}\n", "tx")
                self._log(f"📤 Sending text message\n", "tx")
                self._log(f"   Destination: {dest}\n", "tx")
                self._log(f"   Length: {len(text)} chars\n", "tx")
                self._log(f"   Reliability: {reliability}\n", "tx")
                self._log(f"{'='*60}\n", "tx")
                
                success = self.interface.send_text(dest, text, reliability)
                
                if success:
                    self._log(f"\n✅ Text sent successfully!\n", "success")
                    self.text_input.delete("1.0", "end")
                else:
                    self._log(f"\n❌ Text send failed\n", "error")
                    messagebox.showerror("Failed", f"Failed to send text to {dest}")
            
            except Exception as e:
                self._log(f"\n❌ Error: {e}\n", "error")
                messagebox.showerror("Error", f"Send failed:\n{e}")
            
            finally:
                self.progress_bar.stop()
                self._set_tx_state("normal")
        
        threading.Thread(target=send_thread, daemon=True).start()
    
    def discover_route(self):
        """Discover route to destination"""
        if not self.connected:
            messagebox.showerror("Error", "Not connected to mesh node")
            return
        
        dest = self.dest_node_var.get().strip()
        if not dest:
            messagebox.showerror("Error", "Please enter destination node name")
            return
        
        self.progress_bar.start()
        
        def discover_thread():
            try:
                self._log(f"\n🔍 Discovering route to {dest}...\n", "info")
                success = self.interface.discover_route(dest)
                
                if success:
                    self._log(f"✅ Route found to {dest}\n", "success")
                else:
                    self._log(f"❌ Route discovery failed\n", "error")
            
            finally:
                self.progress_bar.stop()
        
        threading.Thread(target=discover_thread, daemon=True).start()
    
    def show_routes(self):
        """Show routing table"""
        if not self.connected:
            messagebox.showerror("Error", "Not connected to mesh node")
            return
        
        self._log(f"\n🗺️ Requesting routing table...\n", "info")
        self.interface.show_routes()
    
    def show_stats(self):
        """Show node statistics"""
        if not self.connected:
            messagebox.showerror("Error", "Not connected to mesh node")
            return
        
        self._log(f"\n📊 Requesting node statistics...\n", "info")
        self.interface.show_stats()
    
    def toggle_monitoring(self):
        """Start or stop network monitoring"""
        if not self.monitoring:
            self.start_monitoring()
        else:
            self.stop_monitoring()
    
    def start_monitoring(self):
        """Start monitoring network activity"""
        if not self.connected:
            return
        
        self.monitoring = True
        self.monitor_btn.config(text="⏹️ Stop Monitor")
        self._log(f"\n👁️ Starting network monitoring...\n", "info")
        
        def monitor_thread():
            while self.monitoring and self.connected:
                if self.interface.ser.in_waiting:
                    try:
                        line = self.interface.ser.readline().decode(errors='ignore').strip()
                        if line:
                            # Determine tag based on content
                            if "[TX]" in line or "Sent" in line:
                                tag = "tx"
                            elif "[RX]" in line or "Received" in line:
                                tag = "rx"
                            elif "[ERR]" in line or "failed" in line.lower():
                                tag = "error"
                            elif "[ROUTE]" in line:
                                tag = "info"
                            else:
                                tag = ""
                            
                            self._log(f"{line}\n", tag)
                    except Exception:
                        pass
                
                time.sleep(0.01)
        
        self.monitor_thread = threading.Thread(target=monitor_thread, daemon=True)
        self.monitor_thread.start()
    
    def stop_monitoring(self):
        """Stop monitoring network activity"""
        self.monitoring = False
        self.monitor_btn.config(text="👁️ Monitor Network")
        self._log(f"\n⏹️ Stopped network monitoring\n", "warning")
    
    def _set_tx_state(self, state: str):
        """Enable or disable transmit controls"""
        self.send_file_btn.config(state=state)
        self.send_text_btn.config(state=state)
        self.discover_btn.config(state=state)


def main():
    """Main entry point"""
    app = MeshNetworkGUI()
    
    # Handle window close
    def on_closing():
        if app.connected:
            if messagebox.askokcancel("Quit", "Disconnect and quit?"):
                app.disconnect()
                app.destroy()
        else:
            app.destroy()
    
    app.protocol("WM_DELETE_WINDOW", on_closing)
    app.mainloop()


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
LoRa Intelligent Mesh Network - Python Interface

Provides a high-level interface for sending multimedia data through the mesh network.
Supports:
- Text messages
- Images (JPEG, PNG)
- Voice/Audio (WAV, MP3)
- MiniSEED seismic data
- Adaptive reliability based on data type
- Fragmentation for large files
- Real-time progress tracking

Usage:
    # Send text message
    python mesh_network_interface.py COM9 --send-text "Hello Node_2" --dest Node_2

    # Send image with medium reliability
    python mesh_network_interface.py COM9 --send-file image.jpg --dest Node_3 --rel 2

    # Send MiniSEED with critical reliability
    python mesh_network_interface.py COM9 --send-file data.mseed --dest Node_4 --rel 4

    # Monitor network activity
    python mesh_network_interface.py COM9 --monitor

Dependencies:
    pip install pyserial
"""

import argparse
import base64
import hashlib
import mimetypes
import os
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional, Dict, List

import serial

# ==================== CONFIGURATION ====================

# Reliability levels (must match Arduino enum)
REL_NONE = 0      # No ACK
REL_LOW = 1       # Text, voice (1 retry, 2s timeout)
REL_MEDIUM = 2    # Images (2 retries, 5s timeout)
REL_HIGH = 3      # Files (3 retries, 8s timeout)
REL_CRITICAL = 4  # MiniSEED (5 retries, 15s timeout)

# File type to reliability mapping
FILE_RELIABILITY_MAP = {
    '.txt': REL_LOW,
    '.log': REL_LOW,
    '.wav': REL_LOW,
    '.mp3': REL_LOW,
    '.ogg': REL_LOW,
    '.jpg': REL_MEDIUM,
    '.jpeg': REL_MEDIUM,
    '.png': REL_MEDIUM,
    '.gif': REL_MEDIUM,
    '.bmp': REL_MEDIUM,
    '.mseed': REL_CRITICAL,
    '.miniseed': REL_CRITICAL,
    '.sac': REL_CRITICAL,
    '.pdf': REL_HIGH,
    '.doc': REL_HIGH,
    '.docx': REL_HIGH,
    '.zip': REL_HIGH,
    '.tar': REL_HIGH,
    '.gz': REL_HIGH,
}

# Chunk size for fragmentation (characters)
# Small chunks for better reliability over mesh
CHUNK_SIZE = 150  # ~150 chars = ~200 bytes with headers

# Timeouts
CHUNK_SEND_TIMEOUT = 60.0  # seconds per chunk
ROUTE_DISCOVERY_TIMEOUT = 10.0  # seconds for route discovery

# ==================== DATA STRUCTURES ====================

@dataclass
class TransmissionStats:
    """Statistics for a transmission session"""
    total_chunks: int = 0
    sent_chunks: int = 0
    failed_chunks: int = 0
    start_time: float = 0
    end_time: float = 0
    total_bytes: int = 0
    
    @property
    def success_rate(self) -> float:
        if self.total_chunks == 0:
            return 0.0
        return (self.sent_chunks / self.total_chunks) * 100
    
    @property
    def duration(self) -> float:
        if self.end_time == 0:
            return time.time() - self.start_time
        return self.end_time - self.start_time
    
    @property
    def throughput_bps(self) -> float:
        if self.duration == 0:
            return 0.0
        return (self.total_bytes * 8) / self.duration


class MeshNetworkInterface:
    """High-level interface for LoRa mesh network communication"""
    
    def __init__(self, port: str, baudrate: int = 115200):
        self.port = port
        self.baudrate = baudrate
        self.ser: Optional[serial.Serial] = None
        self.stats = TransmissionStats()
        
    def connect(self):
        """Open serial connection to mesh node"""
        try:
            self.ser = serial.Serial(self.port, self.baudrate, timeout=1)
            time.sleep(2.0)  # Wait for ESP32 to initialize
            print(f"[INFO] Connected to {self.port} @ {self.baudrate} baud")
            
            # Clear boot messages
            boot_deadline = time.time() + 2.0
            while time.time() < boot_deadline:
                if self.ser.in_waiting:
                    line = self.ser.readline().decode(errors='ignore').strip()
                    if line:
                        print(f"[NODE] {line}")
            
            return True
        except Exception as e:
            print(f"[ERROR] Failed to connect: {e}")
            return False
    
    def disconnect(self):
        """Close serial connection"""
        if self.ser and self.ser.is_open:
            self.ser.close()
            print("[INFO] Disconnected")
    
    def send_command(self, command: str) -> bool:
        """Send a command to the mesh node"""
        try:
            self.ser.write((command + '\n').encode())
            self.ser.flush()
            return True
        except Exception as e:
            print(f"[ERROR] Failed to send command: {e}")
            return False
    
    def wait_for_response(self, expected: str, timeout: float) -> bool:
        """Wait for expected response from node"""
        deadline = time.time() + timeout
        
        while time.time() < deadline:
            if self.ser.in_waiting:
                line = self.ser.readline().decode(errors='ignore').strip()
                if line:
                    print(f"[NODE] {line}")
                    
                    if expected in line:
                        return True
                    
                    # Check for errors
                    if "[ERR]" in line or "failed" in line.lower():
                        return False
        
        return False
    
    def discover_route(self, dest: str) -> bool:
        """Initiate route discovery to destination"""
        print(f"[INFO] Discovering route to {dest}...")
        
        if not self.send_command(f"DISCOVER:{dest}"):
            return False
        
        # Wait for route to be established
        deadline = time.time() + ROUTE_DISCOVERY_TIMEOUT
        
        while time.time() < deadline:
            if self.ser.in_waiting:
                line = self.ser.readline().decode(errors='ignore').strip()
                if line:
                    print(f"[NODE] {line}")
                    
                    if "[RREP]" in line and dest in line:
                        print(f"[INFO] Route to {dest} established")
                        return True
                    
                    if "Route discovery failed" in line:
                        print(f"[ERROR] Route discovery failed")
                        return False
        
        print(f"[WARN] Route discovery timeout")
        return False
    
    def send_text(self, dest: str, text: str, reliability: int = REL_LOW) -> bool:
        """Send a text message"""
        print(f"\n[TX] Sending text to {dest} (rel={reliability})")
        print(f"[TX] Message: {text[:100]}{'...' if len(text) > 100 else ''}")
        
        # Check if message fits in single packet
        # Format: SEND:<dest>:<rel>:<data>
        # Rough estimate: 200 bytes available for data
        if len(text) <= 150:
            # Single packet
            command = f"SEND:{dest}:{reliability}:{text}"
            if not self.send_command(command):
                return False
            
            # Wait for completion
            if self.wait_for_response("[CMD] Send completed", CHUNK_SEND_TIMEOUT):
                print(f"[TX] Message sent successfully")
                return True
            else:
                print(f"[TX] Message send failed")
                return False
        else:
            # Need fragmentation
            return self.send_fragmented_data(dest, text, reliability, is_binary=False)
    
    def send_file(self, dest: str, filepath: str, reliability: Optional[int] = None) -> bool:
        """Send a file through the mesh network"""
        path = Path(filepath)
        
        if not path.is_file():
            print(f"[ERROR] File not found: {filepath}")
            return False
        
        # Determine reliability if not specified
        if reliability is None:
            ext = path.suffix.lower()
            reliability = FILE_RELIABILITY_MAP.get(ext, REL_MEDIUM)
        
        print(f"\n[TX] Sending file: {path.name}")
        print(f"[TX] Size: {path.stat().st_size} bytes")
        print(f"[TX] Reliability: {reliability}")
        print(f"[TX] Destination: {dest}")
        
        # Read file
        try:
            raw_data = path.read_bytes()
        except Exception as e:
            print(f"[ERROR] Failed to read file: {e}")
            return False
        
        # Encode as base64
        b64_data = base64.b64encode(raw_data).decode('ascii')
        
        # Prepare metadata
        metadata = f"FILE:{path.name}:{len(raw_data)}:"
        full_data = metadata + b64_data
        
        print(f"[TX] Base64 length: {len(b64_data)} chars")
        print(f"[TX] Total length: {len(full_data)} chars")
        
        # Send fragmented
        return self.send_fragmented_data(dest, full_data, reliability, is_binary=True)
    
    def send_fragmented_data(self, dest: str, data: str, reliability: int, is_binary: bool) -> bool:
        """Send large data using fragmentation"""
        # Split into chunks
        chunks = []
        for i in range(0, len(data), CHUNK_SIZE):
            chunks.append(data[i:i+CHUNK_SIZE])
        
        total_chunks = len(chunks)
        
        print(f"\n[TX] Fragmentation required")
        print(f"[TX] Total chunks: {total_chunks}")
        print(f"[TX] Chunk size: ~{CHUNK_SIZE} chars")
        
        # Initialize stats
        self.stats = TransmissionStats()
        self.stats.total_chunks = total_chunks
        self.stats.total_bytes = len(data)
        self.stats.start_time = time.time()
        
        # Send chunks
        for idx, chunk in enumerate(chunks):
            print(f"\n[TX] Chunk {idx+1}/{total_chunks} ({len(chunk)} chars)")
            
            # Prepare chunk message with metadata
            # Format: FRAG:<idx>:<total>:<chunk_data>
            chunk_msg = f"FRAG:{idx}:{total_chunks}:{chunk}"
            
            # Send via SEND command
            command = f"SEND:{dest}:{reliability}:{chunk_msg}"
            
            if not self.send_command(command):
                print(f"[TX] Failed to send command for chunk {idx+1}")
                self.stats.failed_chunks += 1
                continue
            
            # Wait for this chunk to complete
            deadline = time.time() + CHUNK_SEND_TIMEOUT
            success = False
            
            while time.time() < deadline:
                if self.ser.in_waiting:
                    line = self.ser.readline().decode(errors='ignore').strip()
                    if line:
                        print(f"[NODE] {line}")
                        
                        if "[CMD] Send completed" in line:
                            print(f"[TX] Chunk {idx+1}/{total_chunks} sent successfully")
                            self.stats.sent_chunks += 1
                            success = True
                            break
                        
                        if "[CMD] Send failed" in line or "[TX] Failed" in line:
                            print(f"[TX] Chunk {idx+1}/{total_chunks} failed")
                            self.stats.failed_chunks += 1
                            break
            
            if not success and self.stats.failed_chunks == 0:
                print(f"[TX] Timeout for chunk {idx+1}")
                self.stats.failed_chunks += 1
            
            # Progress report
            progress = ((idx + 1) / total_chunks) * 100
            print(f"[PROGRESS] {progress:.1f}% complete ({self.stats.sent_chunks}/{total_chunks} chunks)")
            
            # Check if we should abort
            if self.stats.failed_chunks > total_chunks * 0.3:  # More than 30% failed
                print(f"\n[ERROR] Too many failures ({self.stats.failed_chunks}), aborting")
                self.stats.end_time = time.time()
                return False
        
        # Transmission complete
        self.stats.end_time = time.time()
        
        print(f"\n{'='*50}")
        print(f"TRANSMISSION SUMMARY")
        print(f"{'='*50}")
        print(f"Total chunks:    {self.stats.total_chunks}")
        print(f"Sent:            {self.stats.sent_chunks}")
        print(f"Failed:          {self.stats.failed_chunks}")
        print(f"Success rate:    {self.stats.success_rate:.1f}%")
        print(f"Duration:        {self.stats.duration:.1f}s")
        print(f"Data size:       {self.stats.total_bytes} bytes")
        print(f"Throughput:      {self.stats.throughput_bps:.0f} bps")
        print(f"{'='*50}\n")
        
        return self.stats.failed_chunks == 0
    
    def monitor_network(self):
        """Monitor network activity and display statistics"""
        print("[INFO] Monitoring network activity (Ctrl+C to stop)...\n")
        
        try:
            while True:
                if self.ser.in_waiting:
                    line = self.ser.readline().decode(errors='ignore').strip()
                    if line:
                        # Colorize output based on message type
                        if "[TX]" in line or "[RREQ]" in line or "[RREP]" in line:
                            prefix = "→"
                        elif "[RX]" in line or "[HELLO]" in line:
                            prefix = "←"
                        elif "[FWD]" in line or "[RELAY]" in line:
                            prefix = "↔"
                        elif "[ROUTE]" in line:
                            prefix = "☆"
                        elif "[ERR]" in line or "failed" in line.lower():
                            prefix = "✗"
                        elif "[ACK]" in line:
                            prefix = "✓"
                        else:
                            prefix = " "
                        
                        print(f"{prefix} {line}")
                
                time.sleep(0.01)
        
        except KeyboardInterrupt:
            print("\n[INFO] Monitoring stopped")
    
    def show_routes(self):
        """Request and display routing table"""
        print("[INFO] Requesting routing table...\n")
        
        if not self.send_command("ROUTES"):
            return False
        
        # Wait for routing table output
        time.sleep(1.0)
        
        while self.ser.in_waiting:
            line = self.ser.readline().decode(errors='ignore').strip()
            if line:
                print(line)
        
        return True
    
    def show_stats(self):
        """Request and display node statistics"""
        print("[INFO] Requesting node statistics...\n")
        
        if not self.send_command("STATS"):
            return False
        
        # Wait for statistics output
        time.sleep(1.0)
        
        while self.ser.in_waiting:
            line = self.ser.readline().decode(errors='ignore').strip()
            if line:
                print(line)
        
        return True


# ==================== MAIN ====================

def main():
    parser = argparse.ArgumentParser(
        description='LoRa Intelligent Mesh Network - Python Interface',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Send text message
  python mesh_network_interface.py COM9 --send-text "Hello" --dest Node_2
  
  # Send image file
  python mesh_network_interface.py COM9 --send-file image.jpg --dest Node_3
  
  # Send MiniSEED with critical reliability
  python mesh_network_interface.py COM9 --send-file data.mseed --dest Node_4 --rel 4
  
  # Monitor network
  python mesh_network_interface.py COM9 --monitor
  
  # Show routing table
  python mesh_network_interface.py COM9 --routes
        """
    )
    
    parser.add_argument('port', help='Serial port (e.g., COM9 or /dev/ttyUSB0)')
    parser.add_argument('--baud', type=int, default=115200, help='Baud rate (default: 115200)')
    
    # Transmission options
    parser.add_argument('--send-text', metavar='TEXT', help='Send text message')
    parser.add_argument('--send-file', metavar='FILE', help='Send file')
    parser.add_argument('--dest', help='Destination node name (required for sending)')
    parser.add_argument('--rel', type=int, choices=[0,1,2,3,4], 
                        help='Reliability level (0=none, 1=low, 2=med, 3=high, 4=critical)')
    
    # Monitoring options
    parser.add_argument('--monitor', action='store_true', help='Monitor network activity')
    parser.add_argument('--routes', action='store_true', help='Show routing table')
    parser.add_argument('--stats', action='store_true', help='Show node statistics')
    
    # Route discovery
    parser.add_argument('--discover', metavar='NODE', help='Discover route to node')
    
    args = parser.parse_args()
    
    # Create interface
    interface = MeshNetworkInterface(args.port, args.baud)
    
    # Connect
    if not interface.connect():
        return 1
    
    try:
        # Handle commands
        if args.monitor:
            interface.monitor_network()
        
        elif args.routes:
            interface.show_routes()
        
        elif args.stats:
            interface.show_stats()
        
        elif args.discover:
            interface.discover_route(args.discover)
        
        elif args.send_text:
            if not args.dest:
                print("[ERROR] --dest required for sending")
                return 1
            
            rel = args.rel if args.rel is not None else REL_LOW
            success = interface.send_text(args.dest, args.send_text, rel)
            
            return 0 if success else 1
        
        elif args.send_file:
            if not args.dest:
                print("[ERROR] --dest required for sending")
                return 1
            
            success = interface.send_file(args.dest, args.send_file, args.rel)
            
            return 0 if success else 1
        
        else:
            print("[ERROR] No action specified. Use --help for usage.")
            return 1
    
    finally:
        interface.disconnect()
    
    return 0


if __name__ == '__main__':
    sys.exit(main())

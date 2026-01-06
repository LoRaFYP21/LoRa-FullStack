#!/usr/bin/env python3
"""
LoRa Mesh Network - Receiver Script

Listens for incoming messages and files through the mesh network.
Automatically reassembles fragmented data and saves received files.

Usage:
    python mesh_receiver.py COM9 --out-dir received_files

Features:
    - Automatic fragment reassembly
    - File type detection and saving
    - Real-time statistics
    - Message logging

Dependencies:
    pip install pyserial
"""

import argparse
import base64
import os
import sys
import time
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional

import serial


@dataclass
class FragmentedMessage:
    """Tracks fragments of a reassembly in progress"""
    total_chunks: int
    received_chunks: Dict[int, str] = field(default_factory=dict)
    first_seen: float = field(default_factory=time.time)
    last_update: float = field(default_factory=time.time)
    source: str = ""
    
    @property
    def is_complete(self) -> bool:
        return len(self.received_chunks) == self.total_chunks
    
    @property
    def progress(self) -> float:
        if self.total_chunks == 0:
            return 0.0
        return (len(self.received_chunks) / self.total_chunks) * 100
    
    def get_reassembled(self) -> Optional[str]:
        """Reassemble fragments in order"""
        if not self.is_complete:
            return None
        
        result = ""
        for i in range(self.total_chunks):
            if i not in self.received_chunks:
                return None
            result += self.received_chunks[i]
        
        return result


@dataclass
class ReceptionStats:
    """Statistics for received data"""
    messages_received: int = 0
    fragments_received: int = 0
    files_received: int = 0
    total_bytes: int = 0
    start_time: float = field(default_factory=time.time)
    
    @property
    def uptime(self) -> float:
        return time.time() - self.start_time
    
    @property
    def throughput_bps(self) -> float:
        if self.uptime == 0:
            return 0.0
        return (self.total_bytes * 8) / self.uptime


class MeshReceiver:
    """Receiver for mesh network messages"""
    
    def __init__(self, port: str, output_dir: Path, baudrate: int = 115200):
        self.port = port
        self.baudrate = baudrate
        self.output_dir = output_dir
        self.output_dir.mkdir(parents=True, exist_ok=True)
        
        self.ser: Optional[serial.Serial] = None
        self.stats = ReceptionStats()
        
        # Track fragmented messages: (source, seq) -> FragmentedMessage
        self.fragments: Dict[tuple, FragmentedMessage] = {}
        
        # Timeout for incomplete fragments
        self.fragment_timeout = 300.0  # 5 minutes
    
    def connect(self) -> bool:
        """Connect to mesh node"""
        try:
            self.ser = serial.Serial(self.port, self.baudrate, timeout=1)
            time.sleep(2.0)  # Wait for ESP32 boot
            
            print(f"[INFO] Connected to {self.port} @ {self.baudrate} baud")
            print(f"[INFO] Output directory: {self.output_dir}")
            print(f"[INFO] Listening for messages...\n")
            
            # Clear boot messages
            boot_deadline = time.time() + 2.0
            while time.time() < boot_deadline:
                if self.ser.in_waiting:
                    self.ser.readline()
            
            return True
        
        except Exception as e:
            print(f"[ERROR] Connection failed: {e}")
            return False
    
    def disconnect(self):
        """Close connection"""
        if self.ser and self.ser.is_open:
            self.ser.close()
            print("\n[INFO] Disconnected")
    
    def parse_data_message(self, line: str) -> Optional[Dict]:
        """Parse DATA message from node output"""
        # Example: [RX] DATA from Node_1 (seq=123, hops=2)
        # Next line: [RX] Payload: <data>
        
        if "[RX] DATA from" in line:
            parts = line.split()
            if len(parts) >= 4:
                source = parts[3]  # Node_X
                
                # Extract sequence number
                seq_part = [p for p in parts if "seq=" in p]
                seq = 0
                if seq_part:
                    seq = int(seq_part[0].split("=")[1].rstrip(",)"))
                
                return {
                    "type": "DATA",
                    "source": source,
                    "seq": seq
                }
        
        return None
    
    def parse_payload_line(self, line: str) -> Optional[str]:
        """Extract payload from [RX] Payload: line"""
        if "[RX] Payload:" in line:
            idx = line.index("[RX] Payload:") + len("[RX] Payload:")
            return line[idx:].strip()
        return None
    
    def process_payload(self, source: str, payload: str):
        """Process received payload"""
        # Check if this is a fragment
        if payload.startswith("FRAG:"):
            self.handle_fragment(source, payload)
        
        # Check if this is a file
        elif payload.startswith("FILE:"):
            self.handle_file(source, payload)
        
        # Regular text message
        else:
            self.handle_text_message(source, payload)
    
    def handle_fragment(self, source: str, payload: str):
        """Handle fragment of larger message"""
        # Format: FRAG:<idx>:<total>:<chunk_data>
        try:
            parts = payload.split(":", 3)
            if len(parts) != 4:
                print(f"[WARN] Invalid fragment format from {source}")
                return
            
            idx = int(parts[1])
            total = int(parts[2])
            chunk = parts[3]
            
            # Create key for this message sequence
            key = (source, total)  # Using total as pseudo-sequence for now
            
            if key not in self.fragments:
                self.fragments[key] = FragmentedMessage(
                    total_chunks=total,
                    source=source
                )
                print(f"\n[FRAG] New fragmented message from {source} ({total} chunks)")
            
            msg = self.fragments[key]
            
            # Add this chunk
            if idx not in msg.received_chunks:
                msg.received_chunks[idx] = chunk
                msg.last_update = time.time()
                
                self.stats.fragments_received += 1
                
                print(f"[FRAG] Chunk {idx+1}/{total} from {source} ({msg.progress:.1f}% complete)")
            
            # Check if complete
            if msg.is_complete:
                print(f"[FRAG] All chunks received from {source}, reassembling...")
                
                full_data = msg.get_reassembled()
                if full_data:
                    # Remove from tracking
                    del self.fragments[key]
                    
                    # Process the complete message
                    if full_data.startswith("FILE:"):
                        self.handle_file(source, full_data)
                    else:
                        self.handle_text_message(source, full_data)
                else:
                    print(f"[ERROR] Failed to reassemble message from {source}")
        
        except Exception as e:
            print(f"[ERROR] Fragment processing error: {e}")
    
    def handle_file(self, source: str, payload: str):
        """Handle file transmission"""
        # Format: FILE:<filename>:<size>:<base64_data>
        try:
            parts = payload.split(":", 3)
            if len(parts) != 4:
                print(f"[WARN] Invalid file format from {source}")
                return
            
            filename = parts[1]
            size = int(parts[2])
            b64_data = parts[3]
            
            print(f"\n[FILE] Receiving file from {source}")
            print(f"[FILE] Name: {filename}")
            print(f"[FILE] Size: {size} bytes")
            
            # Decode base64
            try:
                file_data = base64.b64decode(b64_data)
            except Exception as e:
                print(f"[ERROR] Base64 decode failed: {e}")
                return
            
            # Generate unique filename if exists
            output_path = self.output_dir / filename
            counter = 1
            base_name = output_path.stem
            suffix = output_path.suffix
            
            while output_path.exists():
                output_path = self.output_dir / f"{base_name}_{counter}{suffix}"
                counter += 1
            
            # Save file
            output_path.write_bytes(file_data)
            
            self.stats.files_received += 1
            self.stats.total_bytes += len(file_data)
            
            print(f"[FILE] Saved to: {output_path}")
            print(f"[FILE] Verification: {len(file_data)} bytes written")
            
            if len(file_data) == size:
                print(f"[FILE] ✓ Size matches expected")
            else:
                print(f"[FILE] ✗ Size mismatch! Expected {size}, got {len(file_data)}")
        
        except Exception as e:
            print(f"[ERROR] File handling error: {e}")
    
    def handle_text_message(self, source: str, text: str):
        """Handle plain text message"""
        print(f"\n[MSG] From {source}:")
        print(f"[MSG] {text}")
        
        self.stats.messages_received += 1
        self.stats.total_bytes += len(text)
        
        # Log to file
        log_file = self.output_dir / "messages.log"
        with open(log_file, "a", encoding="utf-8") as f:
            timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
            f.write(f"[{timestamp}] {source}: {text}\n")
    
    def cleanup_old_fragments(self):
        """Remove incomplete fragments that have timed out"""
        now = time.time()
        to_remove = []
        
        for key, msg in self.fragments.items():
            if now - msg.last_update > self.fragment_timeout:
                print(f"\n[WARN] Fragment timeout for message from {msg.source} "
                      f"({len(msg.received_chunks)}/{msg.total_chunks} chunks)")
                to_remove.append(key)
        
        for key in to_remove:
            del self.fragments[key]
    
    def print_stats(self):
        """Print current statistics"""
        print(f"\n{'='*50}")
        print(f"RECEPTION STATISTICS")
        print(f"{'='*50}")
        print(f"Messages:     {self.stats.messages_received}")
        print(f"Fragments:    {self.stats.fragments_received}")
        print(f"Files:        {self.stats.files_received}")
        print(f"Total bytes:  {self.stats.total_bytes}")
        print(f"Uptime:       {self.stats.uptime:.1f}s")
        print(f"Throughput:   {self.stats.throughput_bps:.0f} bps")
        print(f"Incomplete:   {len(self.fragments)}")
        print(f"{'='*50}\n")
    
    def listen(self):
        """Main listening loop"""
        print("[INFO] Receiver started (Ctrl+C to stop)\n")
        
        last_cleanup = time.time()
        last_stats = time.time()
        
        pending_data = None  # Stores parsed DATA message waiting for payload
        
        try:
            while True:
                if self.ser.in_waiting:
                    line = self.ser.readline().decode(errors='ignore').strip()
                    
                    if not line:
                        continue
                    
                    # Check if this is a DATA message header
                    data_msg = self.parse_data_message(line)
                    if data_msg:
                        pending_data = data_msg
                        continue
                    
                    # Check if this is a payload line
                    if pending_data:
                        payload = self.parse_payload_line(line)
                        if payload:
                            # Process the complete message
                            self.process_payload(pending_data["source"], payload)
                            pending_data = None
                            continue
                    
                    # Echo all other lines (for debugging)
                    if any(marker in line for marker in ["[RX]", "[TX]", "[ROUTE]", "[HELLO]", "[ACK]"]):
                        print(f"  {line}")
                
                # Periodic cleanup
                now = time.time()
                
                if now - last_cleanup > 60.0:  # Every minute
                    self.cleanup_old_fragments()
                    last_cleanup = now
                
                if now - last_stats > 300.0:  # Every 5 minutes
                    self.print_stats()
                    last_stats = now
                
                time.sleep(0.01)
        
        except KeyboardInterrupt:
            print("\n[INFO] Receiver stopped by user")
            self.print_stats()


def main():
    parser = argparse.ArgumentParser(
        description='LoRa Mesh Network Receiver',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Example:
  python mesh_receiver.py COM9 --out-dir received_files
        """
    )
    
    parser.add_argument('port', help='Serial port (e.g., COM9 or /dev/ttyUSB0)')
    parser.add_argument('--out-dir', default='received_files', 
                        help='Output directory for received files (default: received_files)')
    parser.add_argument('--baud', type=int, default=115200, 
                        help='Baud rate (default: 115200)')
    
    args = parser.parse_args()
    
    # Create receiver
    receiver = MeshReceiver(
        port=args.port,
        output_dir=Path(args.out_dir),
        baudrate=args.baud
    )
    
    # Connect and listen
    if not receiver.connect():
        return 1
    
    try:
        receiver.listen()
    finally:
        receiver.disconnect()
    
    return 0


if __name__ == '__main__':
    sys.exit(main())

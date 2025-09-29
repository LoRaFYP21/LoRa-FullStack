#!/usr/bin/env python3
"""
LoRa CSV Data Capture Script
Captures TX_CSV and RX_CSV data from ESP32 serial output and saves to local files.

Usage:
    python csv_capture.py [COM_PORT] [BAUD_RATE]
    
Example:
    python csv_capture.py COM11 115200
"""

import serial
import datetime
import os
import sys
import signal
import threading
import time

class CSVCapture:
    def __init__(self, port='COM11', baud=115200):
        self.port = port
        self.baud = baud
        self.serial_conn = None
        self.running = False
        self.tx_file = None
        self.rx_file = None
        
        # Create timestamp for unique filenames
        self.timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        self.tx_filename = f"Tx_{self.timestamp}.csv"
        self.rx_filename = f"Rx_{self.timestamp}.csv"
        
    def setup_files(self):
        """Create and setup CSV files with headers"""
        try:
            self.tx_file = open(self.tx_filename, 'w', newline='')
            self.rx_file = open(self.rx_filename, 'w', newline='')
            print(f"✅ Created {self.tx_filename}")
            print(f"✅ Created {self.rx_filename}")
        except Exception as e:
            print(f"❌ Error creating files: {e}")
            return False
        return True
            
    def connect_serial(self):
        """Connect to ESP32 serial port"""
        try:
            self.serial_conn = serial.Serial(self.port, self.baud, timeout=1)
            print(f"✅ Connected to {self.port} at {self.baud} baud")
            return True
        except Exception as e:
            print(f"❌ Error connecting to serial: {e}")
            return False
            
    def process_line(self, line):
        """Process incoming serial line and extract CSV data"""
        line = line.strip()
        
        if line.startswith("TX_CSV_HEADER:"):
            header = line.replace("TX_CSV_HEADER:", "")
            self.tx_file.write(header + '\n')
            self.tx_file.flush()
            print(f"📝 TX Header: {header}")
            
        elif line.startswith("RX_CSV_HEADER:"):
            header = line.replace("RX_CSV_HEADER:", "")
            self.rx_file.write(header + '\n')
            self.rx_file.flush()
            print(f"📝 RX Header: {header}")
            
        elif line.startswith("TX_CSV:"):
            data = line.replace("TX_CSV:", "")
            self.tx_file.write(data + '\n')
            self.tx_file.flush()
            print(f"📤 TX: {data}")
            
        elif line.startswith("RX_CSV:"):
            data = line.replace("RX_CSV:", "")
            self.rx_file.write(data + '\n')
            self.rx_file.flush()
            print(f"📥 RX: {data}")
            
        # Also log other important messages
        elif "Node ID:" in line:
            print(f"🆔 {line}")
        elif "LoRa Chat" in line:
            print(f"🚀 {line}")
            
    def capture_loop(self):
        """Main capture loop"""
        print("🎯 Starting CSV capture... Press Ctrl+C to stop")
        self.running = True
        
        while self.running:
            try:
                if self.serial_conn.in_waiting > 0:
                    line = self.serial_conn.readline().decode('utf-8', errors='ignore')
                    if line:
                        self.process_line(line)
                else:
                    time.sleep(0.01)  # Small delay to prevent CPU spinning
                    
            except KeyboardInterrupt:
                print("\n🛑 Ctrl+C detected, stopping capture...")
                break
            except Exception as e:
                print(f"⚠️ Error reading serial: {e}")
                time.sleep(0.1)
                
    def cleanup(self):
        """Clean up resources"""
        self.running = False
        
        if self.tx_file:
            self.tx_file.close()
            size = os.path.getsize(self.tx_filename)
            print(f"💾 Saved {self.tx_filename} ({size} bytes)")
            
        if self.rx_file:
            self.rx_file.close()
            size = os.path.getsize(self.rx_filename)
            print(f"💾 Saved {self.rx_filename} ({size} bytes)")
            
        if self.serial_conn:
            self.serial_conn.close()
            print("🔌 Serial connection closed")
            
    def run(self):
        """Main run method"""
        print(f"📡 LoRa CSV Capture Tool")
        print(f"📍 Saving to: {os.getcwd()}")
        print(f"🔗 Port: {self.port} @ {self.baud} baud")
        print("-" * 50)
        
        if not self.setup_files():
            return False
            
        if not self.connect_serial():
            return False
            
        try:
            self.capture_loop()
        finally:
            self.cleanup()
            
        return True

def signal_handler(sig, frame):
    """Handle Ctrl+C gracefully"""
    print('\n🛑 Interrupted by user')
    sys.exit(0)

if __name__ == "__main__":
    # Setup signal handler for graceful exit
    signal.signal(signal.SIGINT, signal_handler)
    
    # Parse command line arguments
    port = sys.argv[1] if len(sys.argv) > 1 else 'COM11'
    baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
    
    # Create and run CSV capture
    capture = CSVCapture(port, baud)
    success = capture.run()
    
    if success:
        print("✅ CSV capture completed successfully!")
    else:
        print("❌ CSV capture failed!")
        sys.exit(1)
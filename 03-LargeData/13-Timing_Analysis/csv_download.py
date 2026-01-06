#!/usr/bin/env python3
"""
ESP32 CSV File Downloader
Downloads CSV files from ESP32 LittleFS via serial commands.

Usage:
    python csv_download.py [COM_PORT] [BAUD_RATE]
    
Example:
    python csv_download.py COM9 115200
"""

import serial
import time
import sys
import os
from datetime import datetime

class ESP32CSVDownloader:
    def __init__(self, port='COM9', baud=115200, timeout=5):
        self.port = port
        self.baud = baud
        self.timeout = timeout
        self.serial_conn = None
        
    def connect(self):
        """Connect to ESP32"""
        try:
            self.serial_conn = serial.Serial(self.port, self.baud, timeout=1)
            time.sleep(2)  # Wait for ESP32 to be ready
            print(f"✅ Connected to {self.port} at {self.baud} baud")
            return True
        except Exception as e:
            print(f"❌ Error connecting: {e}")
            return False
    
    def send_command(self, command):
        """Send command to ESP32 and return response"""
        if not self.serial_conn:
            return None
            
        print(f"📤 Sending: {command}")
        self.serial_conn.write((command + '\n').encode())
        time.sleep(0.5)  # Wait for response
        
        response_lines = []
        start_time = time.time()
        
        while time.time() - start_time < self.timeout:
            if self.serial_conn.in_waiting > 0:
                line = self.serial_conn.readline().decode('utf-8', errors='ignore').strip()
                if line:
                    response_lines.append(line)
                    print(f"📥 {line}")
            else:
                time.sleep(0.1)
                
        return response_lines
    
    def download_csv_data(self, csv_type):
        """Download specific CSV data type (tx, rx, timing)"""
        command = f"download {csv_type}"
        response = self.send_command(command)
        
        if not response:
            print(f"⚠️ No response for {csv_type} download")
            return None
            
        # Look for CSV data between BEGIN and END markers
        csv_data = []
        recording = False
        
        for line in response:
            if f"BEGIN {csv_type.upper()} CSV FILE" in line:
                recording = True
                continue
            elif f"END {csv_type.upper()} CSV FILE" in line:
                recording = False
                break
            elif recording:
                csv_data.append(line)
        
        return csv_data
    
    def save_csv_file(self, csv_data, filename):
        """Save CSV data to file"""
        if not csv_data:
            print(f"⚠️ No data to save for {filename}")
            return False
            
        try:
            with open(filename, 'w', newline='', encoding='utf-8') as f:
                for line in csv_data:
                    f.write(line + '\n')
            
            size = os.path.getsize(filename)
            print(f"💾 Saved {filename} ({size} bytes, {len(csv_data)} lines)")
            return True
            
        except Exception as e:
            print(f"❌ Error saving {filename}: {e}")
            return False
    
    def download_all_csv_files(self):
        """Download all CSV files from ESP32"""
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        
        csv_types = ['tx', 'rx', 'timing']
        downloaded_files = []
        
        # First get info
        print("📊 Getting file info...")
        self.send_command("info")
        print("-" * 50)
        
        # Download each CSV type
        for csv_type in csv_types:
            filename = f"{csv_type}_data_{timestamp}.csv"
            print(f"\n📥 Downloading {csv_type.upper()} data...")
            
            csv_data = self.download_csv_data(csv_type)
            if self.save_csv_file(csv_data, filename):
                downloaded_files.append(filename)
        
        return downloaded_files
    
    def disconnect(self):
        """Close serial connection"""
        if self.serial_conn:
            self.serial_conn.close()
            print("🔌 Disconnected from ESP32")

def main():
    # Parse command line arguments
    port = sys.argv[1] if len(sys.argv) > 1 else 'COM9'
    baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
    
    print("📡 ESP32 CSV File Downloader")
    print(f"🔗 Port: {port} @ {baud} baud")
    print("=" * 50)
    
    downloader = ESP32CSVDownloader(port, baud)
    
    try:
        if not downloader.connect():
            print("❌ Failed to connect to ESP32")
            sys.exit(1)
        
        downloaded_files = downloader.download_all_csv_files()
        
        print("\n" + "=" * 50)
        print("✅ Download Complete!")
        
        if downloaded_files:
            print("📁 Downloaded files:")
            for filename in downloaded_files:
                print(f"   - {filename}")
        else:
            print("⚠️ No files were downloaded")
            
    except KeyboardInterrupt:
        print("\n🛑 Download interrupted by user")
    except Exception as e:
        print(f"❌ Error: {e}")
    finally:
        downloader.disconnect()

if __name__ == "__main__":
    main()
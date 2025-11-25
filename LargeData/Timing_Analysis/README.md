# LoRa Timing Analysis System

A comprehensive LoRa communication system with reliable messaging, fragmentation, and detailed timing analysis for wireless network research. Built for ESP32 LilyGo T-Display with SX127x LoRa modules.

## 🎯 **Project Overview**

This system implements a robust LoRa communication protocol with:
- **Reliable messaging** with automatic repeat request (ARQ)
- **Message fragmentation** for large data transmission
- **Precise timing analysis** with CSV logging
- **Real-time statistics** and performance metrics
- **WiFi-synchronized timestamps** for multi-node deployments

Perfect for **academic research**, **IoT prototyping**, and **wireless network analysis**.

## 📋 **Table of Contents**

- [Features](#-features)
- [Hardware Requirements](#-hardware-requirements)
- [Software Requirements](#-software-requirements)
- [Installation](#-installation)
- [Configuration](#-configuration)
- [Usage](#-usage)
- [Protocol Details](#-protocol-details)
- [CSV Data Analysis](#-csv-data-analysis)
- [Serial Commands](#-serial-commands)
- [Troubleshooting](#-troubleshooting)
- [Research Applications](#-research-applications)
- [Contributing](#-contributing)

## ✨ **Features**

### **Core Communication**
- ✅ Reliable LoRa messaging with acknowledgments
- ✅ Automatic message fragmentation (>200 bytes)
- ✅ Configurable retry mechanisms (8 fragment tries, 3 message tries)
- ✅ AS923 frequency band support (923 MHz)
- ✅ Real-time OLED display feedback

### **Timing Analysis**
- ✅ Precise millisecond timestamps
- ✅ WiFi-synchronized real-world time
- ✅ Comprehensive CSV logging (TX/RX data)
- ✅ Performance metrics (PDR, goodput, RSSI/SNR)
- ✅ Cross-device time correlation
- ✅ Automatic SPIFFS filesystem management
- ✅ Persistent storage with error recovery

### **Research Tools**
- ✅ Serial command interface for data extraction
- ✅ Session statistics tracking
- ✅ SPIFFS file management with format option
- ✅ Jupyter notebook analysis integration
- ✅ Python scripts for data processing

## 🛠️ **Hardware Requirements**

### **Primary Components**
- **ESP32 LilyGo T-Display** (Built-in OLED display)
- **SX127x LoRa Module** (connected via SPI)
- **External antenna** (recommended for better range)

### **Wiring Configuration**
```
LilyGo T-Display ↔ SX127x LoRa Module
SCK  (5)  ↔ SCK
MISO (19) ↔ MISO  
MOSI (27) ↔ MOSI
SS   (18) ↔ NSS
RST  (14) ↔ RESET
DIO0 (26) ↔ DIO0
```

### **Optional Components**
- **External OLED** (SSD1306, if not using T-Display)
- **GPS module** (for enhanced timing accuracy)

## 💻 **Software Requirements**

### **Development Environment**
- **PlatformIO** (recommended) or Arduino IDE
- **Python 3.7+** (for data analysis scripts)
- **Git** (for version control)

### **Required Libraries**
```ini
adafruit/Adafruit GFX Library@^1.11.5
adafruit/Adafruit SSD1306@^2.5.7
sandeepmistry/LoRa@^0.8.0
```

### **Python Dependencies**
```bash
pip install pandas matplotlib jupyter numpy scipy
```

## 🚀 **Installation**

### **1. Clone Repository**
```bash
git clone <repository-url>
cd Timing_Analysis
```

### **2. Hardware Setup**
1. Connect SX127x LoRa module to LilyGo T-Display according to wiring diagram
2. Attach appropriate antenna to LoRa module
3. Connect devices via USB for programming

### **3. Software Setup**
```bash
# Install PlatformIO Core
pip install platformio

# Build project
pio run

# Upload to device
pio run --target upload
```

### **4. First Boot Setup**
The device will automatically initialize SPIFFS on first boot:

```
🗂️  Initializing SPIFFS filesystem...
✅ SPIFFS formatted and mounted successfully!
📊 SPIFFS: 0/1048576 bytes used (0.0%)
📝 Creating CSV files:
   TX: /tx_data_TIMESTAMP.csv
   RX: /rx_data_TIMESTAMP.csv
✅ CSV timing logging enabled
```

If SPIFFS initialization fails, the device will still work but CSV logging will be disabled. Use the `FORMAT_SPIFFS` command to manually initialize the filesystem.

## ⚙️ **Configuration**

### **WiFi & Time Sync Setup**
Edit the configuration in `src/main.cpp`:

```cpp
// WiFi credentials
const char* wifi_ssid = "YOUR_WIFI_NETWORK";
const char* wifi_password = "YOUR_PASSWORD";

// Timezone configuration
const char* time_zone = "IST-5:30";  // Sri Lanka time
// Other examples:
// "EST5EDT,M3.2.0,M11.1.0"         // US Eastern
// "CET-1CEST,M3.5.0,M10.5.0/3"     // Central European  
// "JST-9"                           // Japan Standard Time
```

### **LoRa Parameters**
```cpp
#define FREQ_HZ   923E6      // Frequency (AS923 band)
#define LORA_SF   8          // Spreading Factor (7-12)
#define LORA_SYNC 0xA5       // Sync word
```

### **Performance Tuning**
```cpp
const size_t  FRAG_CHUNK = 200;                    // Fragment size (bytes)
const int     FRAG_MAX_TRIES = 8;                  // Fragment retry limit
const int     MSG_MAX_TRIES = 3;                   // Message retry limit
const unsigned long FRAG_ACK_TIMEOUT_MS = 1000;    // Fragment ACK timeout
```

## 📖 **Usage**

### **Basic Operation**

1. **Power on devices** - Both TX and RX nodes
2. **Wait for time sync** - Devices connect to WiFi and sync time
3. **Send messages** - Type text in serial monitor and press Enter
4. **Monitor performance** - View real-time stats on OLED display

### **File System Management**

The device uses **SPIFFS** for persistent CSV storage. On first boot, you'll see:

```
🗂️  Initializing SPIFFS filesystem...
✅ SPIFFS formatted and mounted successfully!
📊 SPIFFS: 0/1048576 bytes used (0.0%)
📝 Creating CSV files:
   TX: /tx_data_061025_164530.csv
   RX: /rx_data_061025_164530.csv
✅ CSV timing logging enabled
```

**File Management Commands:**
- `LIST` - View all stored CSV files
- `DOWNLOAD_TX` / `DOWNLOAD_RX` - Download current session data
- `FORMAT_SPIFFS` - ⚠️ Reset filesystem (deletes all files)

### **Serial Interface**
Connect via serial monitor (115200 baud):

```
=== LoRa Chat (Reliable + Exact Tries + Timing Analysis) — AS923 (923 MHz) ===
115200, Newline. Type and Enter.
Node ID: A1B2C3D4E5F6

📋 Serial Commands Available:
   HELP          - Show command help
   LIST          - List CSV files  
   DOWNLOAD_TX   - Download TX data
   DOWNLOAD_RX   - Download RX data
   STATS         - Show statistics
   Or type any message to send via LoRa
```

### **Message Types**

**Short messages (≤200 bytes):**
```
Hello World!
→ Sent as single MSG packet
```

**Long messages (>200 bytes):**
```
[Long text content...]
→ Automatically fragmented into MSGF packets
→ Reassembled at receiver
```

## 📊 **Protocol Details**

### **Packet Format**

| Type | Format | Description |
|------|--------|-------------|
| MSG  | `MSG,src,dst,seq,text` | Single complete message |
| MSGF | `MSGF,src,dst,seq,idx,total,chunk` | Message fragment |
| ACK  | `ACK,src,dst,seq,rxBytes,rxPkts` | Final acknowledgment |
| ACKF | `ACKF,src,dst,seq,idx` | Fragment acknowledgment |

### **Reliability Mechanism**

1. **Fragment-level retries:** Each fragment gets up to 8 transmission attempts
2. **Message-level retries:** Complete messages get up to 3 attempts  
3. **Acknowledgment timeouts:** Configurable per-fragment and final ACK timeouts
4. **Reassembly logic:** Out-of-order fragment handling with duplicate detection

### **Performance Metrics**

- **PDR (Packet Delivery Ratio):** `(received_packets / sent_packets) × 100%`
- **Goodput:** Effective data throughput excluding protocol overhead
- **RSSI/SNR:** Signal quality measurements
- **Latency:** End-to-end message delivery time

## 📈 **CSV Data Analysis**

### **CSV File Structure**

**TX Data:** `/tx_data_TIMESTAMP.csv`
```csv
time_ms,local_time,packet_type,sequence_no,fragment_idx,total_fragments,packet_size_bytes
15234,2025-10-06_19:45:15.234,MSG,1,-1,-1,45
15890,2025-10-06_19:45:15.890,MSGF,2,0,3,67
```

**RX Data:** `/rx_data_TIMESTAMP.csv`
```csv
time_ms,local_time,packet_type,sequence_no,fragment_idx,total_fragments,packet_size_bytes
15289,2025-10-06_19:45:15.289,MSG,1,-1,-1,45
15923,2025-10-06_19:45:15.923,MSGF,2,0,3,67
```

### **Analysis Scripts**

**Download data:**
```bash
python csv_download.py    # Extract CSV files from devices
```

**Process data:**
```python
import pandas as pd
import matplotlib.pyplot as plt

# Load timing data
tx_data = pd.read_csv('TX_NODE1.csv')
rx_data = pd.read_csv('RX_NODE1.csv')

# Calculate metrics
latency = rx_data['time_ms'] - tx_data['time_ms']
pdr = len(rx_data) / len(tx_data) * 100

print(f"PDR: {pdr:.1f}%")
print(f"Avg Latency: {latency.mean():.1f}ms")
```

**Jupyter Analysis:**
Open `Analysis1.ipynb` for comprehensive data analysis and visualization.

## 🎮 **Serial Commands**

| Command | Description | Example Output |
|---------|-------------|----------------|
| `HELP` | Show command list | Available commands... |
| `LIST` | Show stored files | 📄 FILE: /tx_data_123456.csv (2048 bytes) |
| `DOWNLOAD_TX` | Download current TX file | CSV file contents... |
| `DOWNLOAD_RX` | Download current RX file | CSV file contents... |
| `DOWNLOAD:/filename` | Download specific file | Custom file contents... |
| `STATS` | Show session statistics | TX: 150 packets, RX: 145 packets... |
| `FORMAT_SPIFFS` | **⚠️ Format SPIFFS (DELETES ALL FILES!)** | SPIFFS formatted successfully! |
| `[message]` | Send LoRa message | Message transmitted via LoRa |

### **⚠️ Important: FORMAT_SPIFFS Command**
The `FORMAT_SPIFFS` command will **permanently delete all CSV files** stored on the device. Use this command only if:
- SPIFFS is corrupted and cannot be mounted
- You want to clear all stored data and start fresh
- You're experiencing persistent file system errors

**Usage:**
```
FORMAT_SPIFFS
```
**Response:**
```
⚠️  WARNING: This will DELETE ALL files in SPIFFS!
🔄 Formatting SPIFFS filesystem...
✅ SPIFFS formatted successfully!
🔄 Reinitializing CSV logging...
✅ CSV logging reinitialized!
```

## 🔧 **Troubleshooting**

### **Common Issues**

**Compilation Errors:**
```bash
# Update PlatformIO
pio upgrade

# Clean build
pio run --target clean
pio run
```

**SPIFFS File System Issues:**
```
❌ ERROR: SPIFFS not available - CSV logging disabled
📂 No files found in SPIFFS
[ERROR][vfs_api.cpp:24] open(): File system is not mounted
```

**Solutions:**
1. **Automatic fix:** Device will attempt to format SPIFFS on first boot
2. **Manual fix:** Use the `FORMAT_SPIFFS` command (⚠️ deletes all files)
3. **Check initialization:** Look for SPIFFS status messages during boot:
   ```
   🗂️  Initializing SPIFFS filesystem...
   ✅ SPIFFS formatted and mounted successfully!
   📊 SPIFFS: 0/1048576 bytes used (0.0%)
   ```

**WiFi Connection Failed:**
- Check SSID/password configuration
- Verify WiFi network accessibility
- System falls back to relative timestamps

**No LoRa Communication:**
- Verify wiring connections
- Check antenna attachment
- Confirm frequency band compatibility
- Test with shorter range

**Time Sync Issues:**
- Ensure internet connectivity
- Check NTP server accessibility
- Verify timezone configuration

**CSV Session Not Active:**
```
❌ ERROR: No CSV session active
💡 CSV session starts automatically when device boots with SPIFFS working
```
**Solution:** Restart device or use `FORMAT_SPIFFS` if SPIFFS is corrupted

### **SPIFFS Management**

**Check SPIFFS Status:**
```
LIST          # Shows all files or error if SPIFFS not mounted
STATS         # Shows current session and SPIFFS status
```

**Expected SPIFFS Boot Sequence:**
```
🗂️  Initializing SPIFFS filesystem...
✅ SPIFFS mounted successfully!
📊 SPIFFS: 1024/1048576 bytes used (0.1%)
📝 Creating CSV files:
   TX: /tx_data_061025_164530.csv
   RX: /rx_data_061025_164530.csv
✅ CSV timing logging enabled
```

**SPIFFS Capacity Management:**
- ESP32 SPIFFS partition: ~1MB available
- Monitor usage with `STATS` command
- Download and archive old files regularly
- Use `FORMAT_SPIFFS` to clear space when needed

### **Debugging Mode**
Enable verbose output:
```cpp
#define DEBUG_MODE 1  // Add to main.cpp
```

## 🔬 **Research Applications**

### **Network Performance Studies**
- **Range testing:** Measure PDR vs distance
- **Interference analysis:** Multi-node collision studies  
- **Environmental impact:** Weather/terrain effect analysis
- **Protocol optimization:** ARQ parameter tuning

### **IoT System Development**
- **Sensor networks:** Reliable data collection protocols
- **Emergency communications:** Disaster recovery systems
- **Agricultural monitoring:** Long-range crop sensing
- **Smart city infrastructure:** Wide-area connectivity

### **Academic Research**
- **Wireless communication courses:** Hands-on protocol implementation
- **Final year projects:** Complete system development
- **Research papers:** Performance evaluation data
- **Conference demonstrations:** Live system showcases

## 📄 **File Structure**

```
Timing_Analysis/
├── src/
│   └── main.cpp              # Main application code
├── include/                  # Header files
├── lib/                      # Custom libraries
├── test/                     # Unit tests
├── Analysis1.ipynb          # Jupyter analysis notebook
├── csv_capture.py           # Data capture script
├── csv_download.py          # Data extraction script
├── start_csv_capture.bat    # Windows batch file
├── platformio.ini           # PlatformIO configuration
├── *.csv                    # Sample data files
└── README.md               # This file
```

## 🤝 **Contributing**

1. **Fork the repository**
2. **Create feature branch:** `git checkout -b feature-name`
3. **Commit changes:** `git commit -am 'Add feature'`
4. **Push to branch:** `git push origin feature-name`
5. **Submit pull request**

### **Development Guidelines**
- Follow existing code style
- Add comments for complex functions
- Test on actual hardware before committing
- Update documentation for new features

## 📜 **License**

This project is licensed under the MIT License - see LICENSE file for details.

## 🙏 **Acknowledgments**

- **Sandeep Mistry** - LoRa library
- **Adafruit** - Display libraries
- **Espressif** - ESP32 platform
- **LilyGo** - T-Display hardware

## 📧 **Support**

For questions, issues, or contributions:
- **Create an issue** on GitHub
- **Email:** [your-email@domain.com]
- **Documentation:** See inline code comments

---

**Built with ❤️ for LoRa research and IoT development**

*Last updated: October 2025*
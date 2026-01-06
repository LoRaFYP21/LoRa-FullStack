# 📁 File Index - 14-Mesh_Network

Complete file listing and descriptions for the LoRa Intelligent Mesh Network project.

---

## 📄 Core Implementation Files

### 1. **MeshNode.ino**

**Type:** Arduino Firmware (C++)  
**Size:** ~35 KB  
**Purpose:** Unified node firmware for all mesh nodes

**What it does:**

- Single firmware for all nodes (only NODE_NAME differs)
- AODV-inspired routing protocol
- Automatic role adaptation (endpoint ↔ relay)
- Route discovery and maintenance
- Duplicate suppression
- Multi-hop ACK system
- Priority-based relay queue
- 5 reliability levels

**Configure here:** Change `NODE_NAME` on line 35

**Upload to:** All ESP32 nodes in your mesh network

---

### 2. **mesh_network_interface.py**

**Type:** Python Script  
**Size:** ~15 KB  
**Purpose:** High-level interface for sending data

**What it does:**

- Send text messages
- Send files (images, audio, MiniSEED, etc.)
- Automatic fragmentation for large files
- Progress tracking and statistics
- Route discovery management
- Network monitoring
- Display routing table and statistics

**Usage examples:**

```bash
# Send text
python mesh_network_interface.py COM9 --send-text "Hello" --dest Node_2

# Send file
python mesh_network_interface.py COM9 --send-file photo.jpg --dest Node_3

# Monitor network
python mesh_network_interface.py COM9 --monitor
```

**Dependencies:** `pip install pyserial`

---

### 3. **mesh_receiver.py**

**Type:** Python Script  
**Size:** ~10 KB  
**Purpose:** Receive and reassemble incoming data

**What it does:**

- Listen for incoming messages
- Automatic fragment reassembly
- File saving with type detection
- Message logging
- Real-time statistics
- Incomplete fragment cleanup

**Usage:**

```bash
python mesh_receiver.py COM8 --out-dir received_files
```

**Output:** Saves files to specified directory, logs messages to `messages.log`

---

### 4. **test_network.py**

**Type:** Python Test Script  
**Size:** ~12 KB  
**Purpose:** Automated network testing suite

**What it does:**

- 8 comprehensive tests:
  1. Connectivity test
  2. Route discovery test
  3. Single packet message
  4. Fragmented message
  5. All reliability levels
  6. Throughput measurement
  7. File transfer test
  8. Stress test (10 rapid messages)
- Detailed test reports
- Success rate calculation

**Usage:**

```bash
# Run all tests
python test_network.py COM9 --dest Node_2 --test all

# Run specific test
python test_network.py COM9 --dest Node_2 --test connectivity
```

---

## 📚 Documentation Files

### 5. **README.md**

**Type:** Markdown Documentation  
**Size:** ~25 KB  
**Purpose:** Complete technical documentation

**Contents:**

- Feature overview
- Hardware requirements
- Installation instructions
- Usage examples
- Protocol details (packet format, message types)
- Routing protocol explanation
- Reliability mechanisms
- Network topology examples
- Performance expectations
- Troubleshooting guide
- Comparison with previous experiments

**Read this:** For comprehensive understanding of the system

---

### 6. **QUICK_START.md**

**Type:** Markdown Guide  
**Size:** ~15 KB  
**Purpose:** Get started in 5 minutes

**Contents:**

- Step-by-step setup (hardware to first message)
- 4 typical network setups with diagrams
- Common tasks with examples
- Reliability settings explained
- Quick troubleshooting fixes
- Performance expectations
- Debug checklist

**Start here:** If you're new to the project

---

### 7. **CONFIGURATION.md**

**Type:** Markdown Guide  
**Size:** ~18 KB  
**Purpose:** Advanced configuration and tuning

**Contents:**

- 4 detailed example configurations:
  - Text messaging network
  - Image transfer network
  - Seismic monitoring (critical)
  - Mixed-use mesh
- Advanced firmware tuning:
  - Spreading factor adjustment
  - TX power tuning
  - Fragment size optimization
  - Queue size adjustment
  - Route timeout tuning
- Use-case specific configs
- Testing & validation procedures
- Performance optimization matrix
- Security considerations
- Tips & tricks

**Use this:** For optimization and advanced deployments

---

### 8. **ARCHITECTURE.md**

**Type:** Markdown Diagrams  
**Size:** ~12 KB  
**Purpose:** Visual system architecture reference

**Contents:**

- System architecture diagram
- Protocol stack layers
- Packet structure with examples
- Route discovery flow (step-by-step)
- Multi-hop ACK flow with timeline
- 4 network topology examples with specs
- Node state diagram
- Memory layout (ESP32)
- Python interface architecture

**Use this:** To understand how everything works together

---

### 9. **PROJECT_SUMMARY.md**

**Type:** Markdown Summary  
**Size:** ~8 KB  
**Purpose:** High-level project overview

**Contents:**

- Project overview
- What's included (all files)
- Core capabilities summary
- Quick usage examples
- Performance metrics
- Comparison with previous experiments
- Success criteria checklist

**Use this:** For quick project understanding or presentations

---

## 📊 File Organization

```
14-Mesh_Network/
├── MeshNode.ino                    # Arduino firmware (FLASH THIS)
├── mesh_network_interface.py       # Python TX interface (USE THIS TO SEND)
├── mesh_receiver.py                # Python RX interface (USE THIS TO RECEIVE)
├── test_network.py                 # Test suite (USE THIS TO TEST)
├── README.md                       # Main documentation (READ THIS FIRST)
├── QUICK_START.md                  # Quick start guide (START HERE)
├── CONFIGURATION.md                # Advanced config (TUNE HERE)
├── ARCHITECTURE.md                 # System diagrams (UNDERSTAND HERE)
└── PROJECT_SUMMARY.md              # Project overview (OVERVIEW HERE)
```

---

## 🚀 Quick Reference

### For First-Time Setup:

1. Read **QUICK_START.md**
2. Flash **MeshNode.ino** (change NODE_NAME)
3. Test with **test_network.py**

### For Daily Use:

- Send: **mesh_network_interface.py**
- Receive: **mesh_receiver.py**
- Monitor: `python mesh_network_interface.py COM9 --monitor`

### For Advanced Users:

- Tune: **CONFIGURATION.md**
- Understand: **ARCHITECTURE.md**
- Reference: **README.md**

### For Troubleshooting:

1. Check **QUICK_START.md** - Quick Troubleshooting
2. Check **README.md** - Troubleshooting section
3. Run **test_network.py** to isolate issues

---

## 📐 File Dependencies

```
MeshNode.ino
  └── Arduino Libraries:
      ├── Adafruit_GFX
      ├── Adafruit_SSD1306
      ├── LoRa (Sandeep Mistry)
      └── SPI, Wire (built-in)

mesh_network_interface.py
  └── Python packages:
      └── pyserial

mesh_receiver.py
  └── Python packages:
      └── pyserial

test_network.py
  ├── mesh_network_interface.py (imports)
  └── Python packages:
      └── pyserial
```

---

## 💾 Storage Requirements

**ESP32 Flash:** ~100 KB (firmware)  
**ESP32 RAM:** ~50-100 KB (runtime, including routing table and queues)  
**PC Storage:** Minimal (Python scripts ~40 KB total)  
**Received Files:** Varies (store in `received_files/` directory)

---

## 🔄 Update History

**Version:** 1.0 (Initial Release)  
**Date:** 2026-01-06  
**Status:** Complete and ready for deployment

**Features:**

- ✅ Unified node firmware
- ✅ AODV-inspired routing
- ✅ Multi-hop ACK system
- ✅ Adaptive reliability (5 levels)
- ✅ Python interface (TX/RX)
- ✅ Automated testing
- ✅ Comprehensive documentation

---

## 📞 File-Specific Support

| File                      | Issue Type         | Solution                      |
| ------------------------- | ------------------ | ----------------------------- |
| MeshNode.ino              | Compilation errors | Check library installation    |
| MeshNode.ino              | Upload fails       | Select correct board/port     |
| mesh_network_interface.py | Import error       | `pip install pyserial`        |
| mesh_network_interface.py | No response        | Check COM port, node power    |
| mesh_receiver.py          | No files saved     | Check --out-dir exists        |
| test_network.py           | Tests fail         | Check node connectivity first |

---

**All files documented and ready to use! 🎉**

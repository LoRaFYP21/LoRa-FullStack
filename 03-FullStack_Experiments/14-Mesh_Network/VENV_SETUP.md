# Virtual Environment Setup Guide

## Quick Setup (Windows PowerShell)

### Method 1: Use Existing Virtual Environment (Recommended!)

We'll reuse the virtual environment from `11-Multimedia_Tunnel`:

```powershell
cd "C:\Users\HP\Desktop\Sem 7\Project\Lilygo\03-FullStack_Experiments\14-Mesh_Network"

# Activate the existing venv from 11-Multimedia_Tunnel
..\11-Multimedia_Tunnel\.venv\Scripts\activate.bat

# Verify pyserial is installed (it should already be there)
pip list | findstr pyserial

# If pyserial is missing, install it:
# pip install pyserial

# Run the GUI
python mesh_gui.py
```

You should see `(.venv)` prefix in your terminal.

**Done!** The venv is shared and ready to use.

---

### Method 2: Simple Setup (No venv)

If you prefer not using venv:

```powershell
cd "C:\Users\HP\Desktop\Sem 7\Project\Lilygo\03-FullStack_Experiments\14-Mesh_Network"

# Install dependency globally
pip install pyserial

# Run the GUI
python mesh_gui.py
```

---

### Run Applications

**With venv activated (from Method 1):**

```powershell
# Make sure you're in the right directory
cd "C:\Users\HP\Desktop\Sem 7\Project\Lilygo\03-FullStack_Experiments\14-Mesh_Network"

# Option A: GUI Application (Recommended)
python mesh_gui.py

# Option B: Command Line Interface
python mesh_network_interface.py COM9 --send-text "Hello" --dest Node_2
python mesh_network_interface.py COM9 --send-file photo.jpg --dest Node_3
python mesh_network_interface.py COM9 --monitor

# Option C: Receive files
python mesh_receiver.py COM8 --out-dir received_files

# Option D: Run Tests
python test_network.py COM9 --dest Node_2 --test all
```

**When done, deactivate:**

```powershell
deactivate
```

---

## GUI Application Features

The `mesh_gui.py` provides a complete graphical interface:

### 📡 Connection

- Auto-detect and select serial ports
- Configure baud rate
- One-click connect/disconnect

### 🎯 Destination & Reliability

- Enter destination node name manually
- Quick-select buttons for common nodes (Node_1, Node_2, etc.)
- Choose reliability level (None, Low, Medium, High, Critical)
- Visual descriptions for each reliability level

### 📤 Transmit

- **File Sending:**
  - Browse and select any file
  - Automatic reliability detection based on file type
  - Progress tracking
- **Text Messaging:**
  - Multi-line text input
  - Send button
- **Quick Actions:**
  - 🔍 Discover Route - Find route to destination
  - 🗺️ Show Routes - Display routing table
  - 📊 Statistics - Show node stats
  - 👁️ Monitor Network - Real-time activity monitoring

### 📋 Activity Log

- Color-coded messages:
  - 🔵 Blue: Info
  - 🟢 Green: Success
  - 🟠 Orange: Warning
  - 🔴 Red: Error
  - 🟣 Purple: TX activity
  - 🔵 Teal: RX activity
- Auto-scroll
- Timestamps

### 📊 Status Bar

- Connection status indicator
- Progress bar for ongoing operations

---

## Typical Workflow

### Scenario 1: Send a Photo

1. **Launch GUI:**

   ```powershell
   python mesh_gui.py
   ```

2. **Connect:**

   - Select your COM port (e.g., COM9)
   - Click "🔌 Connect"

3. **Configure:**

   - Set destination: `Node_3`
   - Choose reliability: `Medium (2)` (good for images)

4. **Send:**
   - Click "📁 Browse" and select photo
   - Click "📤 Send File"
   - Watch progress in log

### Scenario 2: Send Text Message

1. **Connect to node** (as above)

2. **Type message:**

   - Enter text in the text box
   - Set destination: `Node_2`
   - Choose reliability: `Low (1)`

3. **Send:**
   - Click "📤 Send Text"

### Scenario 3: Monitor Network

1. **Connect to node**

2. **Start monitoring:**

   - Click "👁️ Monitor Network"
   - Watch real-time network activity
   - See all packets, routes, ACKs

3. **Stop monitoring:**
   - Click "⏹️ Stop Monitor"

---

## Troubleshooting

### Issue: Can't activate shared venv from 11-Multimedia_Tunnel

**Solution:**

```powershell
# Check if the venv exists
ls "..\11-Multimedia_Tunnel\.venv"

# Try activating with full path
& "C:\Users\HP\Desktop\Sem 7\Project\Lilygo\03-FullStack_Experiments\11-Multimedia_Tunnel\.venv\Scripts\activate.bat"

# Or navigate and activate
cd ..\11-Multimedia_Tunnel
.\.venv\Scripts\activate.bat
cd ..\14-Mesh_Network
python mesh_gui.py
```

### Issue: "pyserial not found"

**Solution:**

```powershell
# Activate venv first
..\11-Multimedia_Tunnel\.venv\Scripts\activate.bat

# Install pyserial
pip install pyserial

# Verify installation
pip list | findstr pyserial
```

### Issue: GUI doesn't launch

**Check:**

1. Virtual environment is activated (see `(venv)` prefix)
2. Dependencies installed: `pip list` should show `pyserial`
3. `mesh_network_interface.py` exists in same directory

**Fix:**

```powershell
pip install -r requirements.txt
python mesh_gui.py
```

### Issue: "No ports found"

**Solution:**

1. Connect ESP32 via USB
2. Click "🔄 Refresh" in GUI
3. Check Device Manager (Windows) for COM port number

### Issue: Connection fails

**Check:**

1. Correct COM port selected
2. No other program using the port (close Arduino Serial Monitor)
3. ESP32 is powered on
4. MeshNode.ino is uploaded to device

## Command-Line Alternative (No GUI)

If you prefer command-line:

````powershell
# Navigate to directory
cd "C:\Users\HP\Desktop\Sem 7\Project\Lilygo\03-FullStack_Experiments\14-Mesh_Network"

# Activate shared venv
..\11-Multimedia_Tunnel\.venv\Scripts\activate.bat

# Send file
python mesh_network_interface.py COM9 --send-file data.bin --dest Node_2 --rel 3

# Send text
python mesh_network_interface.py COM9 --send-text "Test message" --dest Node_3 --rel 1

# Discover route
python mesh_network_interface.py COM9 --discover Node_4

# Monitor
python mesh_network_interface.py COM9 --monitor

# Receive (on another node)
python mesh_receiver.py COM8 --out-dir received_files
```eceive (on another node)
python mesh_receiver.py COM8 --out-dir received_files
````

## Best Practices

1. **Share venv** - Both 11-Multimedia_Tunnel and 14-Mesh_Network use the same venv
2. **One port per application** - Can't run GUI and CLI simultaneously on same port
3. **Close when done** - Disconnect before closing GUI
4. **Check logs** - Activity log shows all details
5. **Update routes** - Use "Discover Route" before sending to new destinations

---

## Quick Launch Script

Create `run_gui.bat` in the 14-Mesh_Network folder:

```batch
@echo off
cd "C:\Users\HP\Desktop\Sem 7\Project\Lilygo\03-FullStack_Experiments\14-Mesh_Network"
call ..\11-Multimedia_Tunnel\.venv\Scripts\activate.bat
python mesh_gui.py
pause
```

**Double-click `run_gui.bat` to launch the GUI instantly!**

```

Double-click to launch GUI with venv activated!

---

**Enjoy your user-friendly mesh network interface! 🎉**
```

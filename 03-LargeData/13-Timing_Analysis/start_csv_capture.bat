@echo off
REM Launch csv_capture.py to record TX/RX timing logs from the serial port.
echo ===================================
echo    LoRa CSV Data Capture Tool
echo ===================================
echo.
echo This will capture CSV data from your ESP32 and save it to this folder
echo Press Ctrl+C to stop capturing
echo.

REM Check if Python is available
python --version >nul 2>&1
if %errorlevel% neq 0 (
    echo ERROR: Python is not installed or not in PATH
    echo Please install Python 3.x and try again
    pause
    exit /b 1
)

REM Check if pyserial is installed
python -c "import serial" >nul 2>&1
if %errorlevel% neq 0 (
    echo Installing required pyserial library...
    pip install pyserial
    if %errorlevel% neq 0 (
        echo ERROR: Failed to install pyserial
        echo Please run: pip install pyserial
        pause
        exit /b 1
    )
)

echo Starting CSV capture on COM10...
python csv_capture.py COM10 115200

echo.
echo CSV capture finished. Files saved in this directory.
pause

# serial_to_udp.py
# pip install pyserial
import sys, socket, serial

PORT = 'COM5' if len(sys.argv) < 2 else sys.argv[1]
BAUD = 115200 if len(sys.argv) < 3 else int(sys.argv[2])
HOST = '127.0.0.1' if len(sys.argv) < 4 else sys.argv[3]
UDP_PORT = 5555 if len(sys.argv) < 5 else int(sys.argv[4])

print(f"Reading {PORT} @ {BAUD} → UDP {HOST}:{UDP_PORT}")
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
ser = serial.Serial(PORT, BAUD, timeout=0.05)

buf = bytearray()
CHUNK = 256               # send in chunks so Wireshark shows packets
while True:
    data = ser.read(4096)
    if data:
        buf += data
        while len(buf) >= CHUNK:
            sock.sendto(buf[:CHUNK], (HOST, UDP_PORT))
            del buf[:CHUNK]
    else:
        # flush whatever we have so far (even small fragments)
        if buf:
            sock.sendto(buf, (HOST, UDP_PORT))
            buf.clear()

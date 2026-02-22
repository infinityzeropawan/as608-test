#!/usr/bin/env python3
import serial
import time

PORT = '/dev/ttyACM0'
ser = serial.Serial(PORT, 115200, timeout=0.5)
print(f"Reading from {PORT}...")

start = time.time()
while time.time() - start < 12:
    try:
        line = ser.readline()
        if line:
            text = line.decode('utf-8', errors='replace').strip()
            if text and not all(c in '▒░▐' for c in text):  # Skip garbage chars
                print(text)
    except:
        pass
    time.sleep(0.01)

ser.close()

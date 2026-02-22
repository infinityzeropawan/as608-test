#!/usr/bin/env python3
import serial
import sys
import time

PORT = '/dev/ttyACM0'
BAUD = 115200

try:
    ser = serial.Serial(PORT, BAUD, timeout=0.1)
    print(f"[MONITOR] Connected to {PORT} @ {BAUD} baud")
    print("[MONITOR] Press Ctrl+C to exit\n")
    
    boot_time = time.time()
    skip_count = 0
    
    while True:
        if ser.in_waiting:
            chunk = ser.read(ser.in_waiting)
            try:
                text = chunk.decode('utf-8', errors='replace')
                # Skip garbage from bootloader (first 2 seconds)
                if time.time() - boot_time > 2:
                    sys.stdout.write(text)
                    sys.stdout.flush()
                else:
                    skip_count += len(text)
            except:
                pass
        time.sleep(0.01)
        
except KeyboardInterrupt:
    print("\n[MONITOR] Stopped by user")
except Exception as e:
    print(f"[ERROR] {e}")
finally:
    if 'ser' in locals() and ser.is_open:
        ser.close()
        print("[MONITOR] Port closed")

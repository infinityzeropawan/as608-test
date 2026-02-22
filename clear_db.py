#!/usr/bin/env python3
"""
Clear fingerprint sensor database - Fresh start for enrollment testing
"""
import requests
import time

ESP_IP = "10.67.226.245"  # Change if needed
PORT = 80

def clear_database():
    try:
        print("🔄 Clearing sensor database...")
        resp = requests.post(f"http://{ESP_IP}:{PORT}/api/erase-all", timeout=5)
        if resp.status_code == 200:
            data = resp.json()
            print(f"✓ {data['message']}")
            return True
        else:
            print(f"✗ Error: {resp.text}")
            return False
    except Exception as e:
        print(f"✗ Failed: {e}")
        return False

if __name__ == "__main__":
    print("=== FINGERPRINT DATABASE CLEAR ===\n")
    if clear_database():
        print("\n✓ Database cleared!")
        print("\nNow you can:")
        print("1. Go to http://10.67.226.245")
        print("2. Click 'Enroll' on any slot")
        print("3. Follow prompts to enroll fingerprint")
        print("4. Test with touch button\n")
    else:
        print("\n✗ Could not clear database")
        print("Make sure ESP is at 10.67.226.245")

#!/usr/bin/env python3
"""
Macropad WiFi Listener — receives UDP commands from ESP32 macropad
and simulates keyboard shortcuts for Google Meet / MS Teams.
Sends state feedback back to the ESP32 so the overlay stays in sync.

Usage:
    python macropad_listener.py [--app meet|teams] [--port 13579]

Requirements:
    pip install pyautogui

How it works:
    The ESP32 macropad in WiFi mode broadcasts UDP packets to port 13579.
    This script listens for those packets and presses the corresponding
    keyboard shortcut on your PC.

    After executing a toggle command, the script sends the resulting state
    back to the ESP32 on port 13580 (e.g. "mic:0", "video:1").

    Commands received:
        vol_up        → Volume Up media key
        vol_down      → Volume Down media key
        video_toggle  → Ctrl+E (Meet) or Ctrl+Shift+O (Teams)
        mic_toggle    → Ctrl+D (Meet) or Ctrl+Shift+M (Teams)
        end_meeting   → Ctrl+W (Meet) or Ctrl+Shift+H (Teams)
"""

import argparse
import socket
import sys

try:
    import pyautogui
except ImportError:
    print("ERROR: pyautogui is required.  Install with:  pip install pyautogui")
    sys.exit(1)

# Disable pyautogui fail-safe (move mouse to corner to abort) — optional
pyautogui.FAILSAFE = True

FEEDBACK_PORT = 13580  # Port to send state feedback to ESP32

# Shortcut maps per app
SHORTCUTS = {
    "meet": {
        "video_toggle": lambda: pyautogui.hotkey("ctrl", "e"),
        "mic_toggle":   lambda: pyautogui.hotkey("ctrl", "d"),
        "end_meeting":  lambda: pyautogui.hotkey("ctrl", "w"),
    },
    "teams": {
        "video_toggle": lambda: pyautogui.hotkey("ctrl", "shift", "o"),
        "mic_toggle":   lambda: pyautogui.hotkey("ctrl", "shift", "m"),
        "end_meeting":  lambda: pyautogui.hotkey("ctrl", "shift", "h"),
    },
}

# Common commands (same for both apps)
COMMON = {
    "vol_up":   lambda: pyautogui.hotkey("volumeup"),
    "vol_down": lambda: pyautogui.hotkey("volumedown"),
}


def send_state(sock, esp_addr, mic_on, video_on):
    """Send current mic/video state back to ESP32."""
    for msg in (f"mic:{int(mic_on)}", f"video:{int(video_on)}"):
        sock.sendto(msg.encode(), (esp_addr, FEEDBACK_PORT))


def main():
    parser = argparse.ArgumentParser(description="Macropad WiFi Listener")
    parser.add_argument("--app", choices=["meet", "teams"], default="meet",
                        help="Target app for shortcuts (default: meet)")
    parser.add_argument("--port", type=int, default=13579,
                        help="UDP listen port (default: 13579)")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", args.port))

    app_map = SHORTCUTS[args.app]

    # Track toggle state (start OFF — matches ESP32 default)
    mic_on = False
    video_on = False

    print(f"Macropad listener started — app={args.app}, port={args.port}")
    print(f"State feedback → ESP32 port {FEEDBACK_PORT}")
    print("Waiting for commands from ESP32 macropad (WiFi mode)...")
    print("Press Ctrl+C to stop.\n")

    while True:
        try:
            data, addr = sock.recvfrom(256)
            cmd = data.decode("utf-8", errors="ignore").strip()
            esp_ip = addr[0]
            print(f"[{esp_ip}] → {cmd}", end="  ")

            if cmd in COMMON:
                COMMON[cmd]()
                print("OK")
            elif cmd in app_map:
                app_map[cmd]()
                # Update tracked state for toggles
                if cmd == "mic_toggle":
                    mic_on = not mic_on
                elif cmd == "video_toggle":
                    video_on = not video_on
                elif cmd == "end_meeting":
                    mic_on = False
                    video_on = False
                # Send state feedback to ESP32
                send_state(sock, esp_ip, mic_on, video_on)
                print(f"OK  [mic={'ON' if mic_on else 'OFF'} video={'ON' if video_on else 'OFF'}]")
            else:
                print("(unknown command)")
        except KeyboardInterrupt:
            print("\nStopping.")
            break
        except Exception as e:
            print(f"Error: {e}")

    sock.close()


if __name__ == "__main__":
    main()

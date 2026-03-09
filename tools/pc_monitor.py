#!/usr/bin/env python3
"""
PC Resource Monitor — sends CPU/RAM/Disk/GPU stats to ESP32 over UDP.

Usage:
    python pc_monitor.py <ESP32_IP> [port]

Requirements:
    pip install psutil
    pip install gputil   (optional, for GPU stats)
"""

import sys
import time
import json
import socket
import platform

import psutil


def get_gpu_stats():
    """Try to get GPU usage and temperature via GPUtil (optional)."""
    try:
        import GPUtil
        gpus = GPUtil.getGPUs()
        if gpus:
            return gpus[0].load * 100, gpus[0].temperature
    except (ImportError, Exception):
        pass
    return 0.0, 0.0


def main():
    if len(sys.argv) < 2:
        print("Usage: python pc_monitor.py <ESP32_IP> [port]")
        print("  ESP32_IP : IP address of your ESP32 (shown on Monitor screen)")
        print("  port     : UDP port (default 9999)")
        sys.exit(1)

    esp32_ip = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 9999

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    print(f"Sending stats to {esp32_ip}:{port} every 1 s  (Ctrl+C to stop)")

    disk_path = "C:\\" if platform.system() == "Windows" else "/"

    try:
        while True:
            cpu = psutil.cpu_percent(interval=1)
            mem = psutil.virtual_memory()
            disk = psutil.disk_usage(disk_path)
            gpu_pct, gpu_temp = get_gpu_stats()

            data = {
                "cpu":       round(cpu, 1),
                "ram_pct":   round(mem.percent, 1),
                "ram_used":  round(mem.used / (1024 ** 2)),    # MB
                "ram_total": round(mem.total / (1024 ** 2)),   # MB
                "disk_pct":  round(disk.percent, 1),
                "gpu_pct":   round(gpu_pct, 1),
                "gpu_temp":  round(gpu_temp, 1),
            }

            msg = json.dumps(data).encode()
            sock.sendto(msg, (esp32_ip, port))

            print(
                f"\rCPU: {data['cpu']:5.1f}%  "
                f"RAM: {data['ram_pct']:5.1f}%  "
                f"Disk: {data['disk_pct']:5.1f}%  "
                f"GPU: {data['gpu_pct']:5.1f}%  ",
                end="", flush=True,
            )
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        sock.close()


if __name__ == "__main__":
    main()

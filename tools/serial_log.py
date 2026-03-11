import serial, time, sys
s = serial.Serial('COM10', 115200, timeout=2)
# Don't reset - just listen to ongoing activity
time.sleep(0.5)
data = b''
for _ in range(60):  # 30 seconds of listening
    chunk = s.read(s.in_waiting or 1)
    if chunk:
        data += chunk
    time.sleep(0.5)
s.close()
text = data.decode('utf-8', 'replace')
for line in text.split('\n'):
    low = line.lower()
    if any(k in low for k in ['mic', 'radio', 'i2s', 'audio', 'codec', 'sound', 'es7210', 'es8311', 'decode', 'mp3', 'stream', 'error', 'fail', 'warn', 'connect', 'http', 'station', 'api']):
        print(line.rstrip())

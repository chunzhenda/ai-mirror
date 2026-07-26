import serial, sys, time, datetime

PORT = "COM10"
BAUD = 115200
DURATION = 300

ser = serial.Serial(PORT, BAUD, timeout=1)
start = time.time()
print(f"[monitor] {PORT} @ {BAUD}, {DURATION}s")
sys.stdout.flush()

try:
    while time.time() - start < DURATION:
        data = ser.read(4096)
        if data:
            try:
                text = data.decode("utf-8", errors="replace")
            except Exception:
                text = repr(data)
            ts = datetime.datetime.now().strftime("%H:%M:%S.")
            sys.stdout.write(text)
            sys.stdout.flush()
finally:
    ser.close()
    print("\n[monitor] done")

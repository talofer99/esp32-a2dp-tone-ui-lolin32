"""Serial monitor — writes timestamped output to docs/logs/live.log
Usage: python tools/monitor.py [COM_PORT] [BAUD]
Defaults: COM7, 115200
Stop with Ctrl+C
"""
import sys
import os
import serial
from datetime import datetime

# Force UTF-8 output so replacement characters (\ufffd) don't crash on cp1252 terminals
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM7"
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
LOG  = os.path.join(os.path.dirname(__file__), "..", "docs", "logs", "live.log")

os.makedirs(os.path.dirname(LOG), exist_ok=True)

print(f"[monitor] {PORT} @ {BAUD} -> {os.path.normpath(LOG)}")
print("[monitor] Ctrl+C to stop\n")

# Define port, set DTR/RTS=False, THEN open — prevents ESP32 reset on connect
ser = serial.Serial()
ser.port     = PORT
ser.baudrate = BAUD
ser.timeout  = 1
ser.dsrdtr   = False
ser.rtscts   = False
ser.dtr      = False  # must be set before open()
ser.rts      = False  # must be set before open()
ser.open()

with ser, open(LOG, "a", encoding="utf-8") as f:
    banner = f"\n{'='*60}\n[monitor] started {datetime.now():%Y-%m-%d %H:%M:%S}\n{'='*60}\n"
    f.write(banner)
    f.flush()
    print(banner, end="")
    while True:
        try:
            line = ser.readline()
            if not line:
                continue
            text = line.decode("utf-8", errors="replace").rstrip()
            ts   = datetime.now().strftime("%H:%M:%S.%f")[:-3]
            out  = f"[{ts}] {text}"
            print(out)
            f.write(out + "\n")
            f.flush()
        except KeyboardInterrupt:
            print("\n[monitor] stopped")
            break
        except serial.SerialException as e:
            print(f"[monitor] serial error: {e}")
            break

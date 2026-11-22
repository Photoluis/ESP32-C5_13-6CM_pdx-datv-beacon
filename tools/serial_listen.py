"""ESP32-C5 Dual-Band DATV (IZ7PDX, GPL-3.0) - ascolta la seriale per N secondi senza inviare nulla (diagnosi).

Uso: python serial_listen.py COMx 8
"""
import sys
import time

import serial


def main():
    port, sec = sys.argv[1], float(sys.argv[2])
    p = serial.Serial()
    p.port, p.baudrate, p.timeout = port, 115200, 0.2
    p.dtr = False
    p.rts = False
    p.open()
    end = time.time() + sec
    buf = b""
    while time.time() < end:
        buf += p.read(4096)
    p.close()
    sys.stdout.write(buf.decode("utf-8", "replace"))


if __name__ == "__main__":
    main()

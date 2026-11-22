"""ESP32-C5 Dual-Band DATV (IZ7PDX, GPL-3.0) - avvia il beacon (TXARM + BEACON), stampa le righe con l'ora e lo ferma
con un carattere sulla seriale all'inizio della pausa numero N (le pause sono gli unici momenti in cui la C5 legge la
seriale). Durante la TX la seriale tace: interrupt mascherati.

Uso: python beacon_cycle.py COMx 2 [timeout_s]
"""
import sys
import time

import serial


def main():
    port, stop_at = sys.argv[1], int(sys.argv[2])
    tmax = float(sys.argv[3]) if len(sys.argv) > 3 else 600
    p = serial.Serial()
    p.port, p.baudrate, p.timeout = port, 115200, 0.2
    p.dtr = False
    p.rts = False
    p.open()
    time.sleep(0.3)
    p.reset_input_buffer()
    p.write(b"TXARM\n")
    time.sleep(0.5)
    p.write(b"BEACON\n")
    pauses, buf, t_end = 0, b"", time.time() + tmax
    while time.time() < t_end:
        buf += p.read(1024)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode("utf-8", "replace").strip()
            if not text:
                continue
            print(time.strftime("%H:%M:%S"), text, flush=True)
            if text.startswith("BEACON pausa"):
                pauses += 1
                if pauses == stop_at:
                    time.sleep(2)
                    p.write(b"x")
                    print(time.strftime("%H:%M:%S"), "(inviato STOP)", flush=True)
            if text.startswith("OK beacon"):
                t_end = time.time()
    p.write(b"TXSAFE\n")
    time.sleep(0.5)
    p.close()


if __name__ == "__main__":
    main()

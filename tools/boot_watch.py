"""ESP32-C5 Dual-Band DATV (IZ7PDX, GPL-3.0) - aspetta che la porta ricompaia (ricollegamento / riaccensione della
scheda), registra le righe con l'ora e, se richiesto, ferma il beacon con un carattere alla prima pausa.

Uso: python boot_watch.py COM10 <secondi> [stop_alla_pausa 0/1]
"""
import sys
import time

import serial
from serial.tools import list_ports


def main():
    port, sec = sys.argv[1], float(sys.argv[2])
    stop = len(sys.argv) > 3 and sys.argv[3] == "1"          # 1: ferma alla prima pausa
    cancel = len(sys.argv) > 3 and sys.argv[3] == "2"        # 2: annulla durante il conto alla rovescia
    t_end = time.time() + sec
    p = None
    buf = b""
    seen_absent = False
    while time.time() < t_end:
        present = any(x.device == port for x in list_ports.comports())
        if not present:
            if not seen_absent:
                print(time.strftime("%H:%M:%S"), f"(porta {port} assente: scheda scollegata)", flush=True)
            seen_absent = True
            if p:
                try:
                    p.close()
                except Exception:
                    pass
                p = None
            time.sleep(0.1)
            continue
        if p is None:
            try:
                p = serial.Serial()
                p.port, p.baudrate, p.timeout = port, 115200, 0.2
                p.dtr = False
                p.rts = False
                p.open()
                print(time.strftime("%H:%M:%S"), f"(porta {port} aperta)", flush=True)
            except Exception:
                p = None
                time.sleep(0.2)
                continue
        try:
            buf += p.read(1024)
        except Exception:
            p = None
            continue
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode("utf-8", "replace").strip()
            if not text:
                continue
            print(time.strftime("%H:%M:%S"), text, flush=True)
            if cancel and text.startswith("BEACON parte tra"):
                time.sleep(1)
                p.write(b"x")
                print(time.strftime("%H:%M:%S"), "(inviato ANNULLA)", flush=True)
                cancel = False
            if text.startswith("BEACON annullato"):
                t_end = min(t_end, time.time() + 2)
            if stop and text.startswith("BEACON pausa"):
                time.sleep(1)
                p.write(b"x")
                print(time.strftime("%H:%M:%S"), "(inviato STOP)", flush=True)
                stop = False
            if text.startswith("BEACON fermato"):
                t_end = min(t_end, time.time() + 2)
    if p:
        p.close()


if __name__ == "__main__":
    main()

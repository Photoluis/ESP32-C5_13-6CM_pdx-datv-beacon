"""ESP32-C5 Dual-Band DATV (IZ7PDX, GPL-3.0) - invia comandi al firmware di prova e stampa le risposte.

Uso: python serial_cmd.py COMx 10 "INFO" ["RXCAP 4096" ...]
     python serial_cmd.py COMx 30 --dump out.txt "RXCAP 16380" "RXDUMP 16380"   (righe "D ..." salvate nel file)
Il firmware P2b non ha comandi che trasmettono.
"""
import sys
import time

import serial


def main():
    args = sys.argv[1:]
    port, wait = args[0], float(args[1])
    args = args[2:]
    dump = None
    if args and args[0] == "--dump":
        dump = open(args[1], "w")
        args = args[2:]
    p = serial.Serial()
    p.port, p.baudrate, p.timeout = port, 115200, 0.2
    p.dtr = False
    p.rts = False
    p.open()
    time.sleep(0.3)
    p.reset_input_buffer()
    for cmd in args:
        print(">", cmd, flush=True)
        p.write((cmd + "\n").encode())
        buf, t_end = b"", time.time() + wait
        while time.time() < t_end:
            buf += p.read(4096)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", "replace").strip()
                if not text:
                    continue
                if text.startswith("D ") and dump:
                    dump.write(text + "\n")
                    continue
                print(time.strftime("%H:%M:%S"), text, flush=True)
                if text.startswith(("OK", "ERR")):
                    t_end = time.time()
    p.close()
    if dump:
        dump.close()


if __name__ == "__main__":
    main()

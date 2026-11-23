"""ESP32-C5_13-6CM_pdx-datv-beacon (IZ7PDX, GPL-3.0) - misura della deriva del quarzo con la portante CW.

Manda TXARM e "CW ON <banda> <amp> <s>", poi registra ogni riga della seriale con l'ora del PC (righe CWT: secondi,
temperatura del chip, PLL) fino a "CW fine". Facoltativo: cambi di PLL durante la portante per provare la correzione
in trasmissione (--step S:kHz, anche più volte).

Uso: python cw_drift_log.py COMx 6 300 out.txt [--amp 300] [--step 120:5750001 --step 150:5750000]
Trasmette: lanciarlo solo con il ricevitore pronto e il carico/antenna collegati. Stop anticipato: Ctrl+C (manda CW OFF).
"""
import argparse
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("band", choices=["13", "6"])
    ap.add_argument("seconds", type=int)
    ap.add_argument("out")
    ap.add_argument("--amp", type=int, default=300)
    ap.add_argument("--guard", type=int, default=2, help="0 no, 1 ripristino, 2 force_txon ogni 100 ms")
    ap.add_argument("--tlog", type=int, default=1, help="temperatura ogni n s (0 = mai)")
    ap.add_argument("--step", action="append", default=[], help="S:kHz = CW FREQK kHz dopo S secondi")
    a = ap.parse_args()
    steps = sorted((float(s.split(":")[0]), s.split(":")[1]) for s in a.step)
    p = serial.Serial()
    p.port, p.baudrate, p.timeout = a.port, 115200, 0.1
    p.dtr = False
    p.rts = False
    p.open()
    time.sleep(0.3)
    p.reset_input_buffer()
    f = open(a.out, "w", encoding="utf-8")

    def log(text):
        line = f"{time.time():.3f} {text}"
        f.write(line + "\n")
        f.flush()
        print(time.strftime("%H:%M:%S"), text, flush=True)

    def send(cmd):
        log("> " + cmd)
        p.write((cmd + "\n").encode())

    send("TXARM")
    time.sleep(0.3)
    send(f"CW TLOG {a.tlog}")
    time.sleep(0.3)
    send(f"CW GUARD {a.guard}")
    time.sleep(0.3)
    send(f"CW ON {a.band} {a.amp} {a.seconds}")
    t0, buf = time.time(), b""
    try:
        while time.time() - t0 < a.seconds + 15:
            if steps and time.time() - t0 >= steps[0][0]:
                send("CW FREQK " + steps.pop(0)[1])
            buf += p.read(4096)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", "replace").strip()
                if text:
                    log(text)
                    if text.startswith(("CW fine", "ERR")):
                        raise StopIteration
    except (StopIteration, KeyboardInterrupt):
        pass
    finally:
        p.write(b"CW OFF\n")
        time.sleep(0.3)
        p.close()
        f.close()


if __name__ == "__main__":
    main()

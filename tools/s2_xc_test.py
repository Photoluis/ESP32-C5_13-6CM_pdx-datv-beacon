"""ESP32-C5_13-6CM_pdx-datv-beacon (IZ7PDX, GPL-3.0) - prova del pre-centraggio termico nel DVB-S2.

Manda in sequenza: TXARM, FREQK <kHz>, S2LIVE <sr> <amp> <ms>, poi (facoltativo) CW ON <banda> <amp> <s> come
riferimento della frequenza a caldo. Registra tutto con l'ora del PC. TRASMETTE.
Uso: python s2_xc_test.py COMx 5750000 250000 300 60 out.txt [--cw 60]
"""
import argparse
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("khz", type=int)
    ap.add_argument("sr", type=int)
    ap.add_argument("amp", type=int)
    ap.add_argument("seconds", type=int)
    ap.add_argument("out")
    ap.add_argument("--cw", type=int, default=0, help="secondi di portante CW dopo il DVB-S2 (0 = no)")
    ap.add_argument("--xc", default="ON", help="CW XC S2 ON|OFF")
    a = ap.parse_args()
    p = serial.Serial()
    p.port, p.baudrate, p.timeout = a.port, 115200, 0.1
    p.dtr = False
    p.rts = False
    p.open()
    time.sleep(0.3)
    p.reset_input_buffer()
    f = open(a.out, "w", encoding="utf-8")

    def log(text):
        f.write(f"{time.time():.3f} {text}\n")
        f.flush()
        print(time.strftime("%H:%M:%S"), text, flush=True)

    def send(cmd):
        log("> " + cmd)
        p.write((cmd + "\n").encode())

    def wait_for(prefixes, limit):
        t0, buf = time.time(), b""
        while time.time() - t0 < limit:
            buf += p.read(4096)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", "replace").strip()
                if text:
                    log(text)
                    if text.startswith(prefixes):
                        return text
        return None

    try:
        send("TXARM")
        wait_for(("OK", "ERR"), 2)
        send(f"CW XC S2 {a.xc}")
        wait_for(("OK", "ERR"), 2)
        send(f"FREQK {a.khz}")
        wait_for(("OK", "ERR"), 2)
        send(f"S2LIVE {a.sr} {a.amp} {a.seconds * 1000}")
        r = wait_for(("OK s2live", "ERR"), a.seconds + 30)
        if r and r.startswith("OK") and a.cw:
            band = "6" if a.khz > 4000000 else "13"
            send(f"CW ON {band} {a.amp} {a.cw}")
            wait_for(("CW fine", "ERR"), a.cw + 15)
    except KeyboardInterrupt:
        pass
    finally:
        p.write(b"CW OFF\n")
        time.sleep(0.3)
        p.close()
        f.close()


if __name__ == "__main__":
    main()

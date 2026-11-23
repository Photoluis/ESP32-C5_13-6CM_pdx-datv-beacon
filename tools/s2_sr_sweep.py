"""ESP32-C5_13-6CM_pdx-datv-beacon (IZ7PDX, GPL-3.0) - prova in RF di più symbol rate in sequenza.

Per ogni SR: S2LIVE <sr> <amp> <s*1000>, poi una pausa per cambiare l'SR del ricevitore. TRASMETTE.
Uso: python s2_sr_sweep.py COMx kHz amp secondi pausa out.txt SR1 SR2 ...
"""
import sys
import time

import serial


def main():
    port, khz, amp, sec, pause, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]), sys.argv[6]
    srs = [int(x) for x in sys.argv[7:]]
    p = serial.Serial()
    p.port, p.baudrate, p.timeout = port, 115200, 0.1
    p.dtr = False
    p.rts = False
    p.open()
    time.sleep(0.3)
    p.reset_input_buffer()
    f = open(out, "w", encoding="utf-8")

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

    t = time.time()
    for i, sr in enumerate(srs):
        log(f"PIANO SR {sr} inizio ~{time.strftime('%H:%M:%S', time.localtime(t))}")
        t += sec + 3 + pause
    send("TXARM")
    wait_for(("OK", "ERR"), 2)
    send(f"FREQK {khz}")
    wait_for(("OK", "ERR"), 2)
    for i, sr in enumerate(srs):
        log(f"=== SR {sr} per {sec} s")
        send(f"S2LIVE {sr} {amp} {sec * 1000}")
        wait_for(("OK s2live", "ERR"), sec + 30)
        if i + 1 < len(srs):
            log(f"=== pausa {pause} s: impostare il ricevitore a {srs[i + 1]} S/s")
            time.sleep(pause)
    log("=== fine sequenza")
    p.close()
    f.close()


if __name__ == "__main__":
    main()

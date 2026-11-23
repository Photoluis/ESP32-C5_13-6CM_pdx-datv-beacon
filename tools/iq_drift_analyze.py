"""ESP32-C5_13-6CM_pdx-datv-beacon (IZ7PDX, GPL-3.0) - frequenza della portante dalle catture IQ della Pluto.

Uso: python iq_drift_analyze.py CARTELLA_BIN LO_HZ [log_cw.txt] [--fs 3000000] [--out tabella.txt]
Per ogni cattura (65536 campioni int16 IQ): picco della FFT con finestra di Hann e interpolazione parabolica.
Con il log di cw_drift_log.py unisce la temperatura del chip (righe CWT) per ora.
"""
import argparse
import glob
import os
import re

import numpy as np


def peak(path, fs, fmin, fmax, wide=False):
    raw = np.fromfile(path, dtype="<i2").astype(np.float32)
    if raw.size < 2048:
        return None
    x = raw[0::2] + 1j * raw[1::2]
    x = x - x.mean()
    n = x.size
    s = np.abs(np.fft.fftshift(np.fft.fft(x * np.hanning(n)))) ** 2 + 1e-9
    f = np.fft.fftshift(np.fft.fftfreq(n, 1 / fs))
    noise = np.median(s)
    s[(f < fmin) | (f > fmax)] = 1e-9  # cerca solo dove può stare la portante (fuori dal DC della Pluto)
    if wide:                           # segnale largo (DVB-S2): baricentro della potenza sopra il rumore in [fmin, fmax]
        m = (f >= fmin) & (f <= fmax)
        p = np.clip(s[m] - noise, 0, None)
        return float(np.sum(f[m] * p) / np.sum(p)), 10 * np.log10(np.mean(s[m]) / noise)
    k = int(np.argmax(s))
    a, b, c = np.log(s[k - 1]), np.log(s[k]), np.log(s[k + 1])
    d = 0.5 * (a - c) / (a - 2 * b + c)
    snr = 10 * np.log10(s[k] / noise)
    return f[k] + d * fs / n, snr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("lo", type=float)
    ap.add_argument("cwlog", nargs="?")
    ap.add_argument("--fs", type=float, default=3e6)
    ap.add_argument("--fmin", type=float, default=20e3, help="Hz dal LO")
    ap.add_argument("--fmax", type=float, default=400e3)
    ap.add_argument("--snr", type=float, default=25.0, help="sotto questo S/N la cattura non ha portante")
    ap.add_argument("--wide", action="store_true", help="DVB-S2: baricentro tra fmin e fmax (finestra solo sul segnale)")
    ap.add_argument("--out")
    a = ap.parse_args()
    temps = []
    if a.cwlog:
        for line in open(a.cwlog, encoding="utf-8"):
            m = re.match(r"([\d.]+) CWT (\d+) s ([-\d.na]+) C", line)
            if m and m.group(3) != "nan":
                temps.append((float(m.group(1)), int(m.group(2)), float(m.group(3))))
    rows = []
    for p in sorted(glob.glob(os.path.join(a.dir, "*.bin"))):
        r = peak(p, a.fs, a.fmin, a.fmax, a.wide)
        if r:
            rows.append((float(os.path.basename(p)[:-4]), r[0], r[1]))
    on = [r for r in rows if r[2] >= a.snr]
    if not on:
        print("nessuna portante trovata")
        return
    t0 = on[0][0]
    out = ["# t_s  freq_Hz(assoluta)  offset_kHz_da_5750  snr_dB  chip_C"]
    for t, fo, snr in on:
        fabs = a.lo + fo
        tc = ""
        if temps:
            near = min(temps, key=lambda q: abs(q[0] - t))
            if abs(near[0] - t) < 1.5:
                tc = f"{near[2]:.2f}"
        out.append(f"{t - t0:7.1f} {fabs:.1f} {(fabs - 5750e6) / 1e3:9.3f} {snr:5.1f} {tc}")
    print("\n".join(out))
    if a.out:
        open(a.out, "w", encoding="utf-8").write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()

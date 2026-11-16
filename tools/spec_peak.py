"""Picco più alto della traccia gialla in una cattura 1920x1080 BGRA dello SPECTRUM HD (Pd4Pdx).

Uso: python spec_peak.py file.cut f_start_MHz f_stop_MHz
Area del grafico (1080p): x 120..1840 = start..stop, y 100 = 0 dB, y 579 = -80 dB.
"""
import sys

import numpy as np


def main():
    raw = np.fromfile(sys.argv[1], dtype=np.uint8)[:1920 * 1080 * 4].reshape(1080, 1920, 4)
    f0, f1 = float(sys.argv[2]), float(sys.argv[3])
    b, g, r = raw[..., 0].astype(int), raw[..., 1].astype(int), raw[..., 2].astype(int)
    area = np.zeros_like(r, dtype=bool)
    area[100:579, 122:1838] = True
    yellow = area & (r > 180) & (g > 180) & (b < 140)
    ys, xs = np.nonzero(yellow)
    if len(ys) == 0:
        print("nessuna traccia")
        return
    # rumore: mediana della quota più alta per colonna
    top = {}
    for y, x in zip(ys, xs):
        if x not in top or y < top[x]:
            top[x] = y
    cols = np.array(sorted(top))
    yv = np.array([top[c] for c in cols])
    i = int(np.argmin(yv))
    db = lambda y: -(y - 100) / 479.0 * 80.0
    f = f0 + (cols[i] - 120) / 1720.0 * (f1 - f0)
    print("picco %.3f MHz  %.1f dB  rumore %.1f dB  S/N %.1f dB" % (f, db(yv[i]), db(np.median(yv)), db(yv[i]) - db(np.median(yv))))


if __name__ == "__main__":
    main()

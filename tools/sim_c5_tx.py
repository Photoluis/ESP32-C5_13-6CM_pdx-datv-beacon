"""ESP32-C5 Dual-Band DATV (IZ7PDX, GPL-3.0) - simulazione del generatore QPSK del firmware (qpsk_gen_sym).

Ricostruisce i campioni che la C5 mette nel DAC (tabella RRC 15 fasi x 256 storie, 10 bit, campione ripetuto 8 volte
a 40 MS/s = 15 campioni/simbolo calcolati), li porta a 1 MS/s e salva una cattura .cs8 per tools/parrot/s2_rx.py.

Uso: python sim_c5_tx.py symbols.bin out.cs8 [--seconds 0.8] [--amp 250] [--qinv]
"""
import argparse
import struct

import numpy as np

SPS = 15  # sovrascritto da --sps


def rrc(t, b=0.35):
    t = np.asarray(t, dtype=np.float64)
    out = np.empty_like(t)
    for k, x in enumerate(t):
        if abs(x) < 1e-6:
            out[k] = 1 - b + 4 * b / np.pi
        elif abs(abs(x) - 1 / (4 * b)) < 1e-4:
            out[k] = b / np.sqrt(2) * ((1 + 2 / np.pi) * np.sin(np.pi / (4 * b)) + (1 - 2 / np.pi) * np.cos(np.pi / (4 * b)))
        else:
            out[k] = (np.sin(np.pi * x * (1 - b)) + 4 * b * x * np.cos(np.pi * x * (1 + b))) / (np.pi * x * (1 - 16 * b * b * x * x))
    return out


def table(amp):
    v = np.zeros((SPS, 256))
    for p in range(SPS):
        for h in range(256):
            m = np.arange(8)
            sym = np.where((h >> m) & 1, -1.0, 1.0)
            v[p, h] = np.sum(sym * rrc(p / SPS + m - 3.5))
    q = np.round(v / np.abs(v).max() * amp).astype(np.int64)
    return q  # valori con segno (il firmware li tronca a 10 bit: stesso valore per |v| <= 511)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("symbols")
    ap.add_argument("out")
    ap.add_argument("--seconds", type=float, default=0.8)
    ap.add_argument("--amp", type=int, default=250)
    ap.add_argument("--qinv", action="store_true")
    ap.add_argument("--ideal", action="store_true", help="filtro RRC classico invece della tabella del firmware")
    a = ap.parse_args()
    raw = open(a.symbols, "rb").read()
    assert raw[:8] == b"PDXSYM01"
    sr, nsym = struct.unpack("<II", raw[8:16])
    global SPS
    tot = round(40e6 / sr)                       # come il firmware: 16 fasi se possibile, poi 15, poi 8
    SPS = 16 if tot % 16 == 0 else 15 if tot % 15 == 0 else 8
    n = int(a.seconds * sr)
    data = np.frombuffer(raw[64:64 + (n + 3) // 4], dtype=np.uint8)
    s = np.stack([(data >> (2 * k)) & 3 for k in range(4)], axis=1).reshape(-1)[:n]
    ib = (~s) & 1
    qb = ((~s) >> 1) & 1
    if a.qinv:
        qb ^= 1
    t = table(a.amp)
    # storia: bit m = simbolo n-m; il firmware aggiorna la storia dopo 15 campioni (fase 0 del simbolo successivo)
    hi = np.zeros(n, dtype=np.int64)
    hq = np.zeros(n, dtype=np.int64)
    h1 = h2 = 0
    for k in range(n):
        hi[k], hq[k] = h1, h2          # storia usata per le 15 fasi del simbolo k (prima di inserire il simbolo k)
        h1 = ((h1 << 1) | int(ib[k])) & 255
        h2 = ((h2 << 1) | int(qb[k])) & 255
    if a.ideal:                        # riferimento: convoluzione RRC classica dei simboli a 15 campioni/simbolo
        up = np.zeros(n * SPS, dtype=np.complex128)
        up[::SPS] = (1 - 2.0 * ib) + 1j * (1 - 2.0 * qb)
        g = rrc(np.arange(-4 * SPS, 4 * SPS + 1) / SPS)
        x5 = np.convolve(up, g, mode="same")
    else:
        ph = np.tile(np.arange(SPS), n)
        I = t[ph, np.repeat(hi, SPS)].astype(np.float64)
        Q = t[ph, np.repeat(hq, SPS)].astype(np.float64)
        x5 = I + 1j * Q                # 5 MS/s (ogni valore ripetuto 8 volte a 40 MS/s: qui basta il valore)
    # passa-basso e decimazione 5 -> 1 MS/s
    taps = np.sinc(np.arange(-40, 41) / 5) / 5 * np.hamming(81)
    y = np.convolve(x5, taps, mode="same")[::5]
    y = y / np.abs(y).max() * 100
    out = np.empty(2 * len(y), dtype=np.int8)
    out[0::2] = np.clip(np.round(y.real), -127, 127)
    out[1::2] = np.clip(np.round(y.imag), -127, 127)
    out.tofile(a.out)
    print(f"{a.out}: {len(y)} campioni a {SPS * sr / 5:.0f} S/s ({SPS} fasi), {n} simboli, SR file {sr}")


if __name__ == "__main__":
    main()

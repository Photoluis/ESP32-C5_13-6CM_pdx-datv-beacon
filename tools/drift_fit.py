"""ESP32-C5_13-6CM_pdx-datv-beacon (IZ7PDX, GPL-3.0) - modello della deriva termica dalla tabella di iq_drift_analyze.py.

Modello A (tempo):   f(t) = f0 + a1*(1-exp(-t/tau1)) + a2*(1-exp(-t/tau2))
Modello B (temperatura del chip filtrata): Tf' = (T - Tf)/tau ; f = p0 + c1*(Tf-50) + c2*(Tf-50)^2
Uso: python drift_fit.py tabella.txt
"""
import sys

import numpy as np

d = np.array([[float(v) for v in l.split()] for l in open(sys.argv[1]) if not l.startswith("#") and len(l.split()) == 5])
t, f, T = d[:, 0], d[:, 2] * 1000.0, d[:, 4]           # s, Hz (offset da 5750 MHz), C
f_rel = f - f[0]

best = None
for tau1 in np.arange(5, 80, 1.0):
    for tau2 in np.arange(80, 1500, 10.0):
        X = np.c_[np.ones_like(t), 1 - np.exp(-t / tau1), 1 - np.exp(-t / tau2)]
        c, *_ = np.linalg.lstsq(X, f, rcond=None)
        r = np.sqrt(np.mean((X @ c - f) ** 2))
        if best is None or r < best[0]:
            best = (r, tau1, tau2, c)
r, tau1, tau2, c = best
print(f"A: tau1 {tau1:.0f} s a1 {c[1]:+.0f} Hz | tau2 {tau2:.0f} s a2 {c[2]:+.0f} Hz | f0 {c[0]:.0f} Hz | rms {r:.0f} Hz")


def filt(T, t, tau):
    y = np.empty_like(T)
    y[0] = T[0]
    for i in range(1, len(T)):
        k = 1 - np.exp(-(t[i] - t[i - 1]) / tau)
        y[i] = y[i - 1] + k * (T[i] - y[i - 1])
    return y


bestb = None
for tau in np.arange(0.5, 200, 0.5):
    for Tstart_off in np.arange(-15, 1, 1.0):     # il quarzo parte più freddo del chip (che si scalda subito)
        Tf = filt(np.r_[T[0] + Tstart_off, T[1:]], t, tau)
        x = Tf - 50.0
        X = np.c_[np.ones_like(x), x, x * x]
        cc, *_ = np.linalg.lstsq(X, f, rcond=None)
        rr = np.sqrt(np.mean((X @ cc - f) ** 2))
        if bestb is None or rr < bestb[0]:
            bestb = (rr, tau, Tstart_off, cc)
rr, tau, off, cc = bestb
ppm = 5750.0
print(f"B: tau {tau:.1f} s, Tf iniziale = T0{off:+.0f} C | p0 {cc[0]:.0f} Hz c1 {cc[1]:+.1f} Hz/C c2 {cc[2]:+.2f} Hz/C^2 | rms {rr:.0f} Hz")
print(f"   in ppm: c1 {cc[1]/ppm:+.4f} ppm/C c2 {cc[2]/ppm:+.5f} ppm/C^2 ; escursione misurata {f.max()-f.min():.0f} Hz")

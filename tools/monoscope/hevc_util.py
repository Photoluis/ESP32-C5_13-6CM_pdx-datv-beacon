"""ESP32-C5_13-6CM_pdx-datv-beacon (IZ7PDX, GPL-3.0) - funzioni H.265 minime per la sovrimpressione del monoscopio:
NAL Annex B, emulation prevention, lettura/scrittura di bit, intestazioni lette con trace_headers di ffmpeg."""
import os
import re
import shutil
import subprocess

FF = os.environ.get("FFMPEG") or shutil.which("ffmpeg") or "ffmpeg"   # ffmpeg con libx265 (variabile FFMPEG o PATH)


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode:
        raise SystemExit(f"ERRORE {cmd[:6]}...\n{r.stderr[-2000:]}")
    return r.stderr


def nals_pos(data: bytes):
    """[(inizio_start_code, inizio_nal, fine_nal, tipo)] di un flusso Annex B"""
    idx = [m.start() for m in re.finditer(b"\x00\x00\x01", data)]
    out = []
    for k, s in enumerate(idx):
        start = s - 1 if s > 0 and data[s - 1] == 0 else s      # start code a 4 byte
        e = idx[k + 1] if k + 1 < len(idx) else len(data)
        if k + 1 < len(idx) and data[e - 1] == 0:
            e -= 1                                              # zero dello start code a 4 byte seguente
        out.append((start, s + 3, e, (data[s + 3] >> 1) & 0x3F))
    return out


def nals(data: bytes):
    return [(t, data[b:e]) for _, b, e, t in nals_pos(data)]


def unescape(n: bytes) -> bytes:
    out, z, i = bytearray(), 0, 0
    while i < len(n):
        b = n[i]
        if z >= 2 and b == 3:
            z = 0
            i += 1
            continue
        out.append(b)
        z = z + 1 if b == 0 else 0
        i += 1
    return bytes(out)


def escape(r: bytes) -> bytes:
    out, z = bytearray(), 0
    for b in r:
        if z >= 2 and b <= 3:
            out.append(3)
            z = 0
        out.append(b)
        z = z + 1 if b == 0 else 0
    return bytes(out)


class BR:
    def __init__(self, b, pos=0):
        self.b, self.p = b, pos

    def u(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | ((self.b[self.p >> 3] >> (7 - (self.p & 7))) & 1)
            self.p += 1
        return v

    def ue(self):
        z = 0
        while self.u(1) == 0:
            z += 1
        return (1 << z) - 1 + self.u(z)


class BW:
    def __init__(self):
        self.bits = []

    def u(self, n, v):
        self.bits += [(v >> (n - 1 - i)) & 1 for i in range(n)]

    def ue(self, v):
        v += 1
        n = v.bit_length()
        self.u(n - 1, 0)
        self.u(n, v)

    def se(self, v):
        self.ue(2 * v - 1 if v > 0 else -2 * v)

    def align(self):                    # byte_alignment(): 1 poi zeri
        self.bits.append(1)
        while len(self.bits) % 8:
            self.bits.append(0)

    def bytes(self):
        return bytes(int("".join(map(str, self.bits[i:i + 8])), 2) for i in range(0, len(self.bits), 8))


def trace(path):
    """[(titolo, [(bit, nome, valore)])] dalle intestazioni stampate da trace_headers"""
    err = run([FF, "-hide_banner", "-i", str(path), "-c", "copy", "-bsf:v", "trace_headers", "-f", "null", "-"])
    secs, cur = [], None
    for line in err.splitlines():
        line = re.sub(r"^\[trace_headers @ [0-9a-fx]+\] ", "", line).rstrip()
        if not line or line.startswith("["):
            continue
        m = re.match(r"^(\d+)\s+(\S+)\s+\S+ = (-?\d+)$", line)
        if m:
            cur[1].append((int(m.group(1)), m.group(2), int(m.group(3))))
        else:
            cur = (line.strip(), [])
            secs.append(cur)
    return secs


def fields(sec):
    return {name: v for _, name, v in sec[1]}


def first(secs, title):
    return next(fields(s) for s in secs if s[0] == title)

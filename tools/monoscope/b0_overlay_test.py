"""ESP32-C5_13-6CM_pdx-datv-beacon (IZ7PDX, GPL-3.0) - fase B0: prova sul PC della sovrimpressione del nominativo.

Idea: il monoscopio neutro H.265 640x360 è codificato con CTU 32 e una slice per riga di CTU; la striscia del nominativo
occupa due righe di CTU (y 32..95). Ogni carattere è codificato a parte come immagine 32x64 (due slice da una CTU) con
gli stessi parametri. Nei fotogrammi IDR le due slice della striscia vengono sostituite da 20 + 20 slice di una CTU
(carattere o spazio), con l'intestazione riscritta (indirizzo, QP). Nessuna codifica video al momento della sostituzione:
è quello che farà la C5 in flash quando si salva il nominativo.

Uso: python b0_overlay_test.py OUTDIR "IZ7PDX/B"
"""
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

FF = os.environ.get("FFMPEG") or shutil.which("ffmpeg") or "ffmpeg"   # ffmpeg con libx265 (variabile FFMPEG o PATH)
FONT = "C\\:/Windows/Fonts/DejaVuSansCondensed-Bold.ttf"
BG = "0x101828"                      # fondo della striscia del nominativo (uguale nel monoscopio e nei caratteri)
W, H, CTU = 640, 360, 32
BAND_ROW = 1                         # prima riga di CTU della striscia (y = 32)
BAND_ROWS = 2                        # altezza 64 px
COLS = W // CTU                      # 20
CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/- "
X265 = "ctu=32:wpp=1:keyint=48:min-keyint=48:scenecut=0:open-gop=0:bframes=0:aq-mode=2:log-level=error"


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode:
        raise SystemExit(f"ERRORE {cmd[:6]}...\n{r.stderr[-2000:]}")
    return r.stderr


# ---------------------------------------------------------------- NAL e bit
def nals(data: bytes):
    """[(nal_type, nal_bytes_senza_start_code)]"""
    idx = [m.start() for m in re.finditer(b"\x00\x00\x01", data)]
    out = []
    for k, s in enumerate(idx):
        e = idx[k + 1] if k + 1 < len(idx) else len(data)
        n = data[s + 3:e]
        if k + 1 < len(idx) and n.endswith(b"\x00"):
            n = n.rstrip(b"\x00")          # zero dello start code a 4 byte seguente
        out.append(((n[0] >> 1) & 0x3F, n))
    return out


def unescape(n: bytes) -> bytes:
    return n.replace(b"\x00\x00\x03", b"\x00\x00")   # basta per i dati di x265 (nessun 000003 consecutivo)


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

    def se(self):
        k = self.ue()
        return (k + 1) // 2 if k & 1 else -(k // 2)


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


# ---------------------------------------------------------------- intestazioni con trace_headers
def trace(path):
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


# ---------------------------------------------------------------- codifiche
def encode_main(out: Path, seconds=3.0):
    vf = (f"scale={W}:{H},format=yuv420p,drawbox=x=0:y={BAND_ROW * CTU}:w={W}:h={BAND_ROWS * CTU}:color={BG}:t=fill,"
          f"drawtext=fontfile='{FONT}':text='FRAME %{{eif\\:n\\:d\\:3}}':fontcolor=white:fontsize=22:x=440:y=320")
    run([FF, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", f"smptehdbars=size={W}x{H}:rate=25",
         "-t", str(seconds), "-vf", vf, "-c:v", "libx265", "-b:v", "220k",
         "-x265-params", f"{X265}:slices={H // CTU + (H % CTU > 0)}:vbv-maxrate=220:vbv-bufsize=220", "-f", "hevc", str(out)])


def encode_glyph(ch: str, out: Path, part: int):
    """metà part (0 = sopra, 1 = sotto) del carattere 32x64 come immagine 32x32: una slice con una sola CTU
    (x265 con slices=2 su 32x64 mette entrambe le CTU nella prima slice)"""
    t = {"/": "\\/", " ": "", "-": "-"}.get(ch, ch)
    draw = (f",drawtext=fontfile='{FONT}':text='{t}':fontcolor=white:fontsize=52:x=(w-text_w)/2:y=(h-text_h)/2"
            if t else "")
    run([FF, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", f"color=c={BG}:s={CTU}x{BAND_ROWS * CTU}:r=25",
         "-frames:v", "1", "-vf", f"format=yuv420p{draw},crop={CTU}:{CTU}:0:{part * CTU}", "-c:v", "libx265", "-crf", "20",
         "-x265-params", X265, "-f", "hevc", str(out)])


# ---------------------------------------------------------------- sostituzione
def main():
    outdir, call = Path(sys.argv[1]), sys.argv[2].upper()
    outdir.mkdir(parents=True, exist_ok=True)
    main_p = outdir / "main.hevc"
    encode_main(main_p)
    mt = trace(main_p)
    msps, mpps = first(mt, "Sequence Parameter Set"), first(mt, "Picture Parameter Set")

    glyphs = {}
    for ch in sorted(set(call) | {" "}):
        parts = []
        for part in range(BAND_ROWS):
            gp = outdir / f"g_{ord(ch):02x}_{part}.hevc"
            encode_glyph(ch, gp, part)
            gt = trace(gp)
            gsps, gpps = first(gt, "Sequence Parameter Set"), first(gt, "Picture Parameter Set")
            diff = {k for k in set(msps) | set(gsps) if msps.get(k) != gsps.get(k)}
            diff |= {f"pps.{k}" for k in set(mpps) | set(gpps) if mpps.get(k) != gpps.get(k)}
            heads = [s for s in gt if s[0] == "Slice Segment Header"]
            slices = [n for t, n in nals(gp.read_bytes()) if t < 32]
            assert len(heads) == len(slices) == 1, (ch, part, len(heads), len(slices))
            hs, n = heads[0], slices[0]
            f = {name: v for _, name, v in hs[1]}
            end = max(p for p, name, _ in hs[1] if name.startswith("alignment_bit")) + 1
            assert end % 8 == 0
            rb = unescape(n)
            qp = 26 + gpps["init_qp_minus26"] + f["slice_qp_delta"]
            parts.append(dict(data=rb[end // 8:], qp=qp, sao_l=f.get("slice_sao_luma_flag", 0),
                              sao_c=f.get("slice_sao_chroma_flag", 0)))
        glyphs[ch] = parts
        print(f"carattere {ch!r}: slice {[len(p['data']) for p in parts]} byte, QP {[p['qp'] for p in parts]}, "
              f"differenze SPS/PPS: {sorted(diff) or '-'}")

    # testo centrato nelle 20 colonne
    text = call[:COLS]
    c0 = (COLS - len(text)) // 2
    row_txt = " " * c0 + text + " " * (COLS - c0 - len(text))
    ctbs = (W // CTU) * ((H + CTU - 1) // CTU)
    abits = (ctbs - 1).bit_length()
    sync = mpps.get("entropy_coding_sync_enabled_flag", 0) or mpps.get("tiles_enabled_flag", 0)

    out, replaced = bytearray(), 0
    for t, n in nals(main_p.read_bytes()):
        if t in (19, 20):                                    # slice di un IDR
            r = BR(unescape(n), 16)
            first_flag = r.u(1)
            r.u(1)                                           # no_output_of_prior_pics_flag
            r.ue()                                           # pps id
            addr = 0 if first_flag else r.u(abits)
            row = addr // COLS
            if BAND_ROW <= row < BAND_ROW + BAND_ROWS:
                for col, ch in enumerate(row_txt):
                    g = glyphs[ch][row - BAND_ROW]
                    w = BW()
                    w.u(1, 0)                                # first_slice_segment_in_pic_flag
                    w.u(1, 0)                                # no_output_of_prior_pics_flag
                    w.ue(0)                                  # slice_pic_parameter_set_id
                    w.u(abits, row * COLS + col)             # slice_segment_address
                    w.ue(2)                                  # slice_type I
                    if msps.get("sample_adaptive_offset_enabled_flag"):
                        w.u(1, g["sao_l"])
                        w.u(1, g["sao_c"])
                    w.se(g["qp"] - 26 - mpps["init_qp_minus26"])
                    if sync:
                        w.ue(0)                              # num_entry_point_offsets
                    w.align()
                    out += b"\x00\x00\x00\x01" + escape(n[:2] + w.bytes() + g["data"])
                    replaced += 1
                continue
        out += b"\x00\x00\x00\x01" + n
    res = outdir / f"overlay_{re.sub('[^A-Z0-9]', '_', call)}.hevc"
    res.write_bytes(out)
    print(f"{res}: {replaced} slice inserite, {len(out)} byte (originale {main_p.stat().st_size})")
    err = run([FF, "-hide_banner", "-loglevel", "warning", "-y", "-i", str(res), "-vf", "select='eq(n\\,0)+eq(n\\,20)+eq(n\\,48)+eq(n\\,70)'",
               "-fps_mode", "passthrough", str(outdir / "frame_%02d.png")])
    print("decodifica:", err.strip() or "nessun avviso")


if __name__ == "__main__":
    main()

"""ESP32-C5_13-6CM_pdx-datv-beacon (IZ7PDX, GPL-3.0) - monoscopio neutro con nominativo e frequenza in sovrimpressione.

Genera un file "PDXES002" per il multiplexer TS della C5: lo stesso formato PDXES001 del beacon ESP32-S3 (intestazione,
tabella delle unità video/audio con PTS, dati PES) più una sezione di sovrimpressione:
  * monoscopio H.265 640x360 25 fps, CTU 32, una slice per riga di CTU, GOP chiuso di 48 immagini, loop di 19,2 s;
  * due strisce vuote: nominativo (righe di CTU 1-2, un carattere per cella 32x64) e frequenza (riga 6, due caratteri per
    cella 32x32);
  * tabelle dei caratteri: ogni metà di cella è una slice H.265 di una sola CTU, codificata qui con gli stessi parametri;
  * per ogni IDR la posizione delle slice delle strisce, che la C5 sostituisce con quelle del testo della dashboard.
La C5 non codifica video: compone le slice (intestazione di pochi bit + dati già pronti) in un blocco in flash e il
multiplexer lo inserisce al posto delle strisce vuote.

Uso:
  python make_es_c5.py -o ../out/c5_mono_es_neutral.bin
  python make_es_c5.py -o ../out/c5_mono_es_neutral.bin --preview "IZ7PDX/B" "2375.000 MHz"   (verifica con ffmpeg)
Serve ffmpeg con libx265 (variabile FFMPEG o PATH) e il font DejaVu Sans Condensed Bold.
"""
import argparse
import struct
import sys
import tempfile
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from hevc_util import BR, BW, FF, escape, first, nals_pos, run, trace, unescape  # noqa: E402

W, H, CTU = 640, 360, 32
COLS = W // CTU
ROWS_PIC = (H + CTU - 1) // CTU
KEYINT = 48
LOOP_S = 19.2                                   # 480 immagini e 900 frame AAC esatti
X265 = "ctu=32:wpp=1:keyint=48:min-keyint=48:scenecut=0:open-gop=0:bframes=0:aq-mode=2:log-level=error"
BG = "0x101828"                                 # fondo delle strisce (uguale nel monoscopio e nei caratteri)
FONT_DIRS = [Path("C:/Windows/Fonts"), Path("/usr/share/fonts/truetype/dejavu")]
ES_MAX = 0x160000                               # un solo monoscopio per le due bande: flash 0x290000..0x3F0000 della C5

# (nome, prima riga di CTU, righe, caratteri per cella, corpo del font, insieme di caratteri)
FIELDS = [
    ("call", 1, 2, 1, 48, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/- "),
    ("freq", 6, 1, 2, 24, "0123456789. MHz"),
]
STATIC = [(5, "BEACON DATV"), (7, "DVB-S2  QPSK  H.265")]   # (riga di CTU, testo) scritte fisse


def font_path():
    for d in FONT_DIRS:
        p = d / "DejaVuSansCondensed-Bold.ttf"
        if p.exists():
            return p.as_posix().replace(":", "\\:")
    raise SystemExit("font DejaVuSansCondensed-Bold.ttf non trovato")


def ts_bitrate(sr: int) -> float:
    """TS utile di DVB-S2 QPSK 3/4, frame normali con pilot (come il firmware)"""
    kbch, nsym = 48408, 90 + 32400 + 22 * 36
    return (kbch - 80) * sr / nsym


def parse_pes(ts: bytes, pid: int):
    out, cur = [], None
    for i in range(0, len(ts) - 187, 188):
        p = ts[i:i + 188]
        if ((p[1] & 0x1F) << 8 | p[2]) != pid:
            continue
        afc = (p[3] >> 4) & 3
        off = 4 + (1 + p[4] if afc & 2 else 0)
        if not afc & 1:
            continue
        if p[1] & 0x40:
            if cur:
                out.append(cur)
            h = p[off:]
            hl = h[8]
            q = h[9:14]
            pts = ((q[0] >> 1) & 7) << 30 | q[1] << 22 | (q[2] >> 1) << 15 | q[3] << 7 | q[4] >> 1
            cur = [pts, bytearray(h[9 + hl:])]
        elif cur:
            cur[1] += p[off:]
    if cur:
        out.append(cur)
    return out


def textfile(td: Path, name: str, text: str) -> str:
    p = td / name
    p.write_text(text, encoding="utf-8")
    return p.as_posix().replace(":", "\\:")


def make_template_ts(td: Path, sr: int, path: Path):
    font = font_path()
    rate = ts_bitrate(sr)
    audio_k = 32
    video_k = int(rate * 0.80 / 1000) - audio_k
    nfr = int(round(LOOP_S * 25))
    vf = [f"scale={W}:{H}", "format=yuv420p"]
    for name, row, rows, _, _, _ in FIELDS:
        vf.append(f"drawbox=x=0:y={row * CTU}:w={W}:h={rows * CTU}:color={BG}:t=fill")
    for k, (row, text) in enumerate(STATIC):
        tf = textfile(td, f"static{k}.txt", text)
        vf.append(f"drawtext=fontfile='{font}':textfile='{tf}':fontcolor=white:fontsize=22:box=1:boxcolor=black@0.6"
                  f":boxborderw=4:x=(w-text_w)/2:y={row * CTU}+(32-text_h)/2")
    # fascia in basso come il beacon ESP32-S3: scala del loop, cursore, barra a scanner, contatore e tempo
    vf.append("drawbox=x=0:y=300:w=640:h=60:color=black@0.85:t=fill")
    vf += [f"drawbox=x={10 + int(s * 620 / LOOP_S)}:y=304:w=2:h=10:color=white:t=fill" for s in range(int(LOOP_S) + 1)]
    vf.append(f"drawtext=fontfile='{font}':text='FRAME %{{eif\\:n\\:d\\:3}}/{nfr}   "
              "T %{eif\\:n/25\\:d}.%{eif\\:mod(n\\,25)*4\\:d\\:2} s':fontcolor=white:fontsize=18:x=640-text_w-8:y=333")
    graph = (f"color=c=yellow:s=20x28:r=25[sc];color=c=0x36c2ff:s=6x14:r=25[pg];"
             f"[0:v]{','.join(vf)}[b];"
             f"[b][sc]overlay=x='10+330*(1-abs(1-2*mod(n,{KEYINT})/{KEYINT}))':y=328:shortest=1[c];"
             f"[c][pg]overlay=x='7+n*620/{nfr}':y=303:shortest=1,format=yuv420p[v]")
    run([FF, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", f"smptehdbars=size={W}x{H}:rate=25",
         "-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=48000", "-t", f"{LOOP_S + 1.0:.3f}",
         "-filter_complex", graph, "-map", "[v]", "-map", "1:a",
         "-c:v", "libx265", "-b:v", f"{video_k}k",
         "-x265-params", f"{X265}:slices={ROWS_PIC}:vbv-maxrate={video_k}:vbv-bufsize={video_k}",
         "-c:a", "aac", "-b:a", f"{audio_k}k", "-ac", "1", "-ar", "48000",
         "-f", "mpegts", "-muxrate", str(int(rate)), "-pcr_period", "40", "-mpegts_flags", "+initial_discontinuity",
         "-metadata", "service_name=NOCALL", "-metadata", "service_provider=BEACON", str(path)])
    print(f"monoscopio: video {video_k} kbit/s, audio {audio_k} kbit/s, TS {rate:.0f} bit/s")


def encode_cell_row(td: Path, key: str, field, row: int, out: Path):
    name, _, rows, cell, size, _ = field
    font = font_path()
    draw = ""
    pitch = CTU // cell                          # ogni carattere al centro della sua parte di cella (passo fisso)
    for i, ch in enumerate(key):
        if ch == " ":
            continue
        tf = textfile(td, f"t_{name}_{ord(ch):02x}.txt", ch)
        draw += (f",drawtext=fontfile='{font}':textfile='{tf}':fontcolor=white:fontsize={size}"
                 f":x={i * pitch}+({pitch}-text_w)/2:y=(h-{size})/2:y_align=font")
    run([FF, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", f"color=c={BG}:s={CTU}x{rows * CTU}:r=25",
         "-frames:v", "1", "-vf", f"format=yuv420p{draw},crop={CTU}:{CTU}:0:{row * CTU}", "-c:v", "libx265", "-crf", "20",
         "-x265-params", X265, "-f", "hevc", str(out)])


ALLOWED_DIFF = {"colour_description_present_flag", "colour_primaries", "transfer_characteristics", "matrix_coefficients",
                "general_level_idc", "pic_width_in_luma_samples", "pic_height_in_luma_samples",
                "pps.entropy_coding_sync_enabled_flag", "pps.pps_loop_filter_across_slices_enabled_flag"}


def build_glyphs(td: Path, field, msps, mpps):
    name, _, rows, cell, _, charset = field
    keys = list(charset) if cell == 1 else [a + b for a in charset for b in charset]
    glyphs = {}
    for i, key in enumerate(keys):
        parts = []
        for r in range(rows):
            gp = td / f"g_{name}_{i}_{r}.hevc"
            encode_cell_row(td, key, field, r, gp)
            gt = trace(gp)
            gsps, gpps = first(gt, "Sequence Parameter Set"), first(gt, "Picture Parameter Set")
            diff = {k for k in set(msps) | set(gsps) if msps.get(k) != gsps.get(k)}
            diff |= {f"pps.{k}" for k in set(mpps) | set(gpps) if mpps.get(k) != gpps.get(k)}
            if diff - ALLOWED_DIFF:
                raise SystemExit(f"parametri incompatibili per {key!r}: {sorted(diff - ALLOWED_DIFF)}")
            heads = [s for s in gt if s[0] == "Slice Segment Header"]
            slices = [n for _, b, e, t in nals_pos(gp.read_bytes()) for n in [gp.read_bytes()[b:e]] if t < 32]
            assert len(heads) == len(slices) == 1
            f = {nm: v for _, nm, v in heads[0][1]}
            end = max(p for p, nm, _ in heads[0][1] if nm.startswith("alignment_bit")) + 1
            assert end % 8 == 0
            rb = unescape(slices[0])
            qp = 26 + gpps["init_qp_minus26"] + f["slice_qp_delta"]
            sao = f.get("slice_sao_luma_flag", 0) | f.get("slice_sao_chroma_flag", 0) << 1
            parts.append((rb[end // 8:], qp, sao))
        glyphs[key] = parts
        if i % 40 == 39:
            print(f"  {name}: {i + 1}/{len(keys)} celle")
    return keys, glyphs


def slice_header(addr, qp, sao, g):
    """intestazione di una slice I di una CTU (stesso algoritmo del firmware)"""
    w = BW()
    w.u(1, 0)                                   # first_slice_segment_in_pic_flag
    w.u(1, 0)                                   # no_output_of_prior_pics_flag (IRAP)
    w.ue(0)                                     # slice_pic_parameter_set_id
    w.u(g["abits"], addr)                       # slice_segment_address
    w.ue(2)                                     # slice_type I
    if g["sao"]:
        w.u(1, sao & 1)
        w.u(1, sao >> 1 & 1)
    w.se(qp - 26 - g["init_qp_minus26"])        # slice_qp_delta
    if g["entry"]:
        w.ue(0)                                 # num_entry_point_offsets
    w.align()
    return w.bytes()


def build_blob(fi, text, keys, glyphs, nalh, g):
    """slice della striscia fi per il testo dato: NAL Annex B in ordine di indirizzo"""
    name, row0, rows, cell, _, charset = FIELDS[fi]
    text = "".join(c if c in charset else " " for c in (text.upper() if name == "call" else text))
    cells = [text[i:i + cell].ljust(cell) for i in range(0, len(text), cell)][:COLS]
    c0 = (COLS - len(cells)) // 2
    blank = " " * cell
    out = bytearray()
    for r in range(rows):
        for col in range(COLS):
            k = col - c0
            key = cells[k] if 0 <= k < len(cells) else blank
            data, qp, sao = glyphs[key if key in glyphs else blank][r]
            hdr = slice_header((row0 + r) * COLS + col, qp, sao, g)
            out += b"\x00\x00\x00\x01" + escape(nalh + hdr + data)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sr", type=int, default=333000, help="SR per il bitrate del video (QPSK 3/4)")
    ap.add_argument("-o", "--output", default="../out/c5_mono_es_neutral.bin")
    ap.add_argument("--preview", nargs=2, metavar=("CALL", "FREQ"), help="anteprima: compone come la C5 e decodifica")
    a = ap.parse_args()
    out_path = Path(a.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tdn:
        td = Path(tdn)
        tsp = td / "m.ts"
        make_template_ts(td, a.sr, tsp)
        ts = tsp.read_bytes()
        video, audio = parse_pes(ts, 0x100), parse_pes(ts, 0x101)
        hevc = td / "m.hevc"
        hevc.write_bytes(b"".join(bytes(d) for _, d in video))
        mt = trace(hevc)
        msps, mpps = first(mt, "Sequence Parameter Set"), first(mt, "Picture Parameter Set")
        for k, want in (("dependent_slice_segments_enabled_flag", 0), ("output_flag_present_flag", 0),
                        ("num_extra_slice_header_bits", 0), ("pps_slice_chroma_qp_offsets_present_flag", 0),
                        ("deblocking_filter_control_present_flag", 0), ("pps_loop_filter_across_slices_enabled_flag", 0),
                        ("slice_segment_header_extension_present_flag", 0), ("tiles_enabled_flag", 0)):
            if mpps.get(k, 0) != want:
                raise SystemExit(f"PPS del monoscopio: {k} = {mpps.get(k)} (atteso {want})")
        g = dict(abits=(COLS * ROWS_PIC - 1).bit_length(), sao=msps.get("sample_adaptive_offset_enabled_flag", 0),
                 entry=mpps.get("entropy_coding_sync_enabled_flag", 0), init_qp_minus26=mpps["init_qp_minus26"])

        # unità come make_es.py (ESP32-S3): video da 0 per un loop, audio 900 frame AAC ridistribuiti
        loop_ticks = int(round(LOOP_S * 90000))
        base = video[0][0]
        units = [(0, pts - base, bytes(d)) for pts, d in video if 0 <= pts - base < loop_ticks]
        frames = []
        for _, d in audio:
            i = 0
            while i + 7 <= len(d):
                fl = ((d[i + 3] & 3) << 11) | (d[i + 4] << 3) | (d[i + 5] >> 5)
                frames.append(bytes(d[i:i + fl]))
                i += fl
        need = loop_ticks // 1920
        units += [(1, k * 1920, frames[k]) for k in range(need)]
        units.sort(key=lambda u: (u[1], u[0]))

        # posizione delle strisce negli IDR
        idr = []
        for ui, (kind, _, d) in enumerate(units):
            if kind:
                continue
            nl = nals_pos(d)
            if not any(t in (19, 20) for *_, t in nl):
                continue
            rowpos = {}
            nalh = None
            for s, b, e, t in nl:
                if t not in (19, 20):
                    continue
                nalh = d[b:b + 2]
                r = BR(unescape(d[b:min(e, b + 8)]), 16)
                firstf = r.u(1)
                r.u(1)
                r.ue()
                addr = 0 if firstf else r.u(g["abits"])
                assert addr % COLS == 0, "una slice per riga attesa"
                rowpos[addr // COLS] = (s, e)
            spans = []
            for name, row0, rows, *_ in FIELDS:
                rs = [rowpos[row0 + r] for r in range(rows)]
                for (s1, e1), (s2, _) in zip(rs, rs[1:]):
                    assert e1 == s2, "slice della striscia non contigue"
                spans.append((rs[0][0], rs[-1][1] - rs[0][0]))
            idr.append((ui, nalh, spans))
        assert len({n for _, n, _ in idr}) == 1, "tipi di NAL diversi negli IDR: la striscia non sarebbe unica"
        print(f"unità {len(units)} ({sum(1 for u in units if u[0] == 0)} video), IDR {len(idr)}, "
              f"striscia vuota {[s[1] for s in idr[0][2]]} byte nel primo IDR")

        tabs = []
        for fi, field in enumerate(FIELDS):
            print(f"caratteri {field[0]}...")
            tabs.append(build_glyphs(td, field, msps, mpps))

    # ---- file PDXES002
    data = b""
    table = b""
    for kind, pts, d in units:
        table += struct.pack("<B3xIII", kind, pts, len(data), len(d))
        data += d
    hdr_len = 64
    data_off = hdr_len + len(table)
    ovl_off = (data_off + len(data) + 3) & ~3
    # sezione di sovrimpressione
    oh_len, fd_len = 28, 24
    gdata = bytearray()
    ftabs, fdesc = [], []
    gt_off = oh_len + fd_len * len(FIELDS)
    gtabs = b""
    for fi, (keys, glyphs) in enumerate(tabs):
        name, row0, rows, cell, _, _ = FIELDS[fi]
        ent = b""
        for key in keys:
            ent += key.encode().ljust(2, b"\x00")[:2] + b"\x00\x00"
            for dat, qp, sao in glyphs[key]:
                ent += struct.pack("<IHBB", 0, len(dat), qp, sao)       # offset riempito sotto
        fdesc.append([row0, rows, cell, len(keys), gt_off + len(gtabs), name])
        gtabs += ent
    idr_off = gt_off + len(gtabs)
    idr_tab = b""
    for ui, nalh, spans in idr:
        idr_tab += struct.pack("<I2s2x", ui, nalh) + b"".join(struct.pack("<II", s, n) for s, n in spans)
    gdata_off = idr_off + len(idr_tab)
    # offsets dei dati dei caratteri
    gtabs = bytearray(gtabs)
    pos = 0
    for fi, (keys, glyphs) in enumerate(tabs):
        rows = FIELDS[fi][2]
        esz = 4 + 8 * rows
        for ki, key in enumerate(keys):
            for r, (dat, _, _) in enumerate(glyphs[key]):
                o = fdesc[fi][4] - gt_off + ki * esz + 4 + r * 8
                struct.pack_into("<I", gtabs, o, gdata_off + len(gdata))
                gdata += dat
            pos += 1
    flags = (1 if g["sao"] else 0) | (2 if g["entry"] else 0)
    ovl = struct.pack("<8sIII4Bb3x", b"PDXOVL01", len(FIELDS), len(idr), idr_off, COLS, g["abits"], flags, 0,
                      g["init_qp_minus26"])
    assert len(ovl) == oh_len, len(ovl)
    for row0, rows, cell, n, off, name in fdesc:
        ovl += struct.pack("<BBBBHHI8s4x", row0, rows, cell, 0, n, 0, off, name.encode())
    ovl += bytes(gtabs) + idr_tab + bytes(gdata)
    hdr = struct.pack("<8sIIIIII32s", b"PDXES002", len(units), int(round(LOOP_S * 90000)), 0x24, 0x0F, data_off, ovl_off,
                      b"NEUTRAL C5 OVERLAY")
    blob = hdr + table + data + b"\x00" * (ovl_off - data_off - len(data)) + ovl
    print(f"dimensioni: dati video+audio {len(data)}, sovrimpressione {len(ovl)} (caratteri {len(gdata)})")
    if len(blob) > ES_MAX:
        raise SystemExit(f"file di {len(blob)} byte: oltre i {ES_MAX} di spazio nella C5")
    out_path.write_bytes(blob)
    print(f"{out_path}: {len(blob)} byte (dati {len(data)}, sovrimpressione {len(ovl)}), CRC32 {zlib.crc32(blob):08x}")

    if a.preview:
        preview(blob, a.preview, out_path.with_suffix(""))


def preview(blob: bytes, texts, stem: Path):
    """compone come la C5 (dalle sole informazioni del file) e decodifica alcune immagini con ffmpeg"""
    magic, nunits, _, _, _, data_off, ovl_off = struct.unpack_from("<8sIIIIII", blob, 0)
    assert magic == b"PDXES002"
    units = [struct.unpack_from("<B3xIII", blob, 64 + 16 * i) for i in range(nunits)]
    o = ovl_off
    _, nf, nidr, idr_off, cols, abits, flags, _, iq = struct.unpack_from("<8sIII4Bb3x", blob, o)
    g = dict(abits=abits, sao=flags & 1, entry=flags >> 1 & 1, init_qp_minus26=iq)
    fdesc = [struct.unpack_from("<BBBBHHI8s4x", blob, o + 28 + 24 * f) for f in range(nf)]
    blobs = []
    for fi, (row0, rows, cell, _, n, _, toff, _) in enumerate(fdesc):
        esz = 4 + 8 * rows
        glyphs = {}
        for k in range(n):
            e = o + toff + k * esz
            key = blob[e:e + 2].rstrip(b"\x00").decode()
            parts = []
            for r in range(rows):
                doff, dlen, qp, sao = struct.unpack_from("<IHBB", blob, e + 4 + 8 * r)
                parts.append((blob[o + doff:o + doff + dlen], qp, sao))
            glyphs[key] = parts
        blobs.append(glyphs)
    esz = 8 + 8 * nf
    idr = {}
    for i in range(nidr):
        ui, nalh = struct.unpack_from("<I2s2x", blob, o + idr_off + i * esz)
        spans = [struct.unpack_from("<II", blob, o + idr_off + i * esz + 8 + 8 * f) for f in range(nf)]
        idr[ui] = (nalh, spans)
    nalh0 = next(iter(idr.values()))[0]
    fblobs = [build_blob(fi, texts[fi], None, blobs[fi], nalh0, g) for fi in range(nf)]
    for fi, b in enumerate(fblobs):
        print(f"striscia {FIELDS[fi][0]} {texts[fi]!r}: {len(b)} byte, CRC32 {zlib.crc32(b):08x}")
    video = bytearray()
    for ui, (kind, _, off, ln) in enumerate(units):
        if kind:
            continue
        d = blob[data_off + off:data_off + off + ln]
        if ui in idr:
            nalh, spans = idr[ui]
            pieces, pos = [], 0
            for (s, n), fb in sorted(zip(spans, fblobs)):
                pieces += [d[pos:s], fb]
                pos = s + n
            pieces.append(d[pos:])
            d = b"".join(pieces)
        video += d
    hv = Path(str(stem) + "_preview.hevc")
    hv.write_bytes(video)
    err = run([FF, "-hide_banner", "-loglevel", "warning", "-y", "-r", "25", "-i", str(hv), "-vf",
               "select='eq(n\\,0)+eq(n\\,30)+eq(n\\,479)'", "-fps_mode", "passthrough", str(stem) + "_preview_%d.png"])
    print("decodifica:", err.strip() or "nessun avviso", "->", str(stem) + "_preview_*.png")


if __name__ == "__main__":
    main()

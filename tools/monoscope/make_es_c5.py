"""ESP32-C5_13-6CM_pdx-datv-beacon (IZ7PDX, GPL-3.0) - monoscopio con nominativo e frequenza in sovrimpressione.

Genera un file "PDXES002" per il multiplexer TS della C5: lo stesso formato PDXES001 del beacon ESP32-S3 (intestazione,
tabella delle unità video/audio con PTS, dati PES) più una sezione di sovrimpressione "PDXOVL02":
  * video H.265 640x360 25 fps, CTU 32, una slice per riga di CTU, GOP chiuso di 48 immagini, loop di 19,2 s (x N);
  * sfondo: barre di prova (predefinito), un'immagine (--image) o un video (--video);
  * striscia del nominativo (righe di CTU 1-2) scura al 50 % sopra lo sfondo, che lì resta fermo (con un video si usa
    il primo fotogramma), e striscia della frequenza (riga 6) scura piena;
  * tabelle dei caratteri: ogni metà di cella è una slice H.265 di una sola CTU codificata qui sopra il proprio sfondo
    (per colonna: le colonne con lo stesso sfondo condividono i caratteri);
  * per ogni IDR la posizione delle slice delle strisce, che la C5 sostituisce con quelle del testo della dashboard.
La C5 non codifica video: compone le slice (intestazione di pochi bit + dati già pronti) in flash quando cambia il testo
e il multiplexer le inserisce al posto delle strisce dei fotogrammi chiave.

Uso:
  python make_es_c5.py -o ../out/c5_mono_es_neutral.bin                         (monoscopio standard)
  python make_es_c5.py --image foto.jpg -o ../out/mio.bin                       (immagine fissa)
  python make_es_c5.py --video clip.mp4 --loops 1 --audio clip.mp4 -o ../out/mio.bin
  ... --preview "IZ7PDX/B" "2375.000 MHz"                                       (verifica: compone come la C5)
Serve ffmpeg con libx265 (variabile FFMPEG o PATH) e il font DejaVu Sans Condensed Bold.
"""
import argparse
import hashlib
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
LOOP_UNIT_S = 19.2                              # 480 immagini e 900 frame AAC esatti, multiplo del GOP
X265 = "ctu=32:wpp=1:keyint=48:min-keyint=48:scenecut=0:open-gop=0:bframes=0:aq-mode=2:log-level=error"
BG = "0x101828"                                 # colore delle strisce
FONT_DIRS = [Path("C:/Windows/Fonts"), Path("/usr/share/fonts/truetype/dejavu")]
ES_MAX = 0x160000                               # un solo monoscopio per le due bande: flash 0x290000..0x3F0000 della C5

# (nome, prima riga di CTU, righe, caratteri per cella, corpo del font, insieme di caratteri, opacità della striscia)
FIELDS = [
    ("call", 1, 2, 1, 48, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/- ", 0.5),
    ("freq", 6, 1, 2, 24, "0123456789. MHz", 1.0),
]
STATIC = [(5, "BEACON DATV"), (7, "DVB-S2  QPSK  H.265")]   # (riga di CTU, testo) scritte fisse del monoscopio standard


def font_path():
    for d in FONT_DIRS:
        p = d / "DejaVuSansCondensed-Bold.ttf"
        if p.exists():
            return p.as_posix().replace(":", "\\:")
    raise SystemExit("font DejaVuSansCondensed-Bold.ttf non trovato")


def ffpath(p) -> str:
    return Path(p).resolve().as_posix().replace(":", "\\:").replace("'", "\\'")


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


# ---------------------------------------------------------------- monoscopio
def source_args(a):
    if a.video:
        return ["-stream_loop", "-1", "-i", a.video]
    if a.image:
        return ["-loop", "1", "-framerate", "25", "-i", a.image]
    return ["-f", "lavfi", "-i", f"smptehdbars=size={W}x{H}:rate=25"]


def video_graph(td: Path, a, loop_s: float) -> str:
    """grafo dei filtri dal sorgente [0:v] all'uscita [v] (stesso grafo per il video e per lo sfondo dei caratteri)"""
    font = font_path()
    fit = (f"scale={W}:{H}:force_original_aspect_ratio=decrease,pad={W}:{H}:(ow-iw)/2:(oh-ih)/2,setsar=1,fps=25,scale=out_range=tv"
           if (a.image or a.video) else f"scale={W}:{H}")
    # sotto la striscia del nominativo lo sfondo resta fermo (primo fotogramma): la C5 la cambia solo negli IDR
    c_row, c_rows = FIELDS[0][1], FIELDS[0][2]
    g = (f"[0:v]{fit},format=yuv420p,split[m][f];"
         f"[f]trim=end_frame=1,crop={W}:{c_rows * CTU}:0:{c_row * CTU},loop=loop=-1:size=1:start=0,setpts=N/25/TB[fr];"
         f"[m][fr]overlay=0:{c_row * CTU}:shortest=1")
    vf = []
    for name, row, rows, _, _, _, alpha in FIELDS:
        vf.append(f"drawbox=x=0:y={row * CTU}:w={W}:h={rows * CTU}:color={BG}@{alpha}:t=fill")
    if a.text:
        for k, (row, text) in enumerate(STATIC):
            tf = textfile(td, f"static{k}.txt", text)
            vf.append(f"drawtext=fontfile='{font}':textfile='{tf}':fontcolor=white:fontsize=22:box=1:boxcolor=black@0.6"
                      f":boxborderw=4:x=(w-text_w)/2:y={row * CTU}+(32-text_h)/2")
    g += "," + ",".join(vf)
    if a.band:
        # fascia in basso come il beacon ESP32-S3: scala del loop, cursore, barra a scanner, contatore e tempo
        nfr = int(round(loop_s * 25))
        band = ["drawbox=x=0:y=300:w=640:h=60:color=black@0.85:t=fill"]
        band += [f"drawbox=x={10 + int(s * 620 / loop_s)}:y=304:w=2:h=10:color=white:t=fill" for s in range(int(loop_s) + 1)]
        band.append(f"drawtext=fontfile='{font}':text='FRAME %{{eif\\:n\\:d\\:3}}/{nfr}   "
                    "T %{eif\\:n/25\\:d}.%{eif\\:mod(n\\,25)*4\\:d\\:2} s':fontcolor=white:fontsize=18:x=640-text_w-8:y=333")
        g = (f"color=c=yellow:s=20x28:r=25[sc];color=c=0x36c2ff:s=6x14:r=25[pg];{g},{','.join(band)}[b];"
             f"[b][sc]overlay=x='10+330*(1-abs(1-2*mod(n,{KEYINT})/{KEYINT}))':y=328:shortest=1[c];"
             f"[c][pg]overlay=x='7+n*620/{nfr}':y=303:shortest=1,format=yuv420p[v]")
    else:
        g += ",format=yuv420p[v]"
    return g


def make_template_ts(td: Path, a, loop_s: float, path: Path, bg_png: Path):
    rate = ts_bitrate(a.sr)
    audio_k = 32
    video_k = int(rate * 0.80 / 1000) - audio_k
    graph = video_graph(td, a, loop_s)
    src = source_args(a)
    # sfondo delle strisce per i caratteri: primo fotogramma prima della codifica
    run([FF, "-hide_banner", "-loglevel", "error", "-y", *src, "-filter_complex", graph, "-map", "[v]",
         "-frames:v", "1", str(bg_png)])
    aud = (["-stream_loop", "-1", "-i", a.audio] if a.audio
           else ["-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=48000"])
    run([FF, "-hide_banner", "-loglevel", "error", "-y", *src, *aud, "-t", f"{loop_s + 1.0:.3f}",
         "-filter_complex", graph, "-map", "[v]", "-map", "1:a:0",
         "-color_range", "tv", "-c:v", "libx265", "-crf", "24",       # qualità costante, tetto VBV del canale
         "-x265-params", f"{X265}:slices={ROWS_PIC}:vbv-maxrate={video_k}:vbv-bufsize={video_k}",
         "-c:a", "aac", "-b:a", f"{audio_k}k", "-ac", "1", "-ar", "48000",
         "-f", "mpegts", "-muxrate", str(int(rate)), "-pcr_period", "40", "-mpegts_flags", "+initial_discontinuity",
         "-metadata", "service_name=NOCALL", "-metadata", "service_provider=BEACON", str(path)])
    print(f"monoscopio: video {video_k} kbit/s, audio {audio_k} kbit/s, TS {rate:.0f} bit/s, loop {loop_s} s")


# ---------------------------------------------------------------- caratteri
ALLOWED_DIFF = {"colour_description_present_flag", "colour_primaries", "transfer_characteristics", "matrix_coefficients",
                "general_level_idc", "pic_width_in_luma_samples", "pic_height_in_luma_samples",
                "pps.entropy_coding_sync_enabled_flag", "pps.pps_loop_filter_across_slices_enabled_flag"}


def column_groups(td: Path, bg_png: Path, field):
    """per ogni colonna il gruppo di sfondo (colonne con sfondo identico condividono i caratteri) e un ritaglio PNG"""
    name, row0, rows, *_ = field
    raw = td / f"strip_{name}.rgb"
    run([FF, "-hide_banner", "-loglevel", "error", "-y", "-i", str(bg_png), "-vf",
         f"crop={W}:{rows * CTU}:0:{row0 * CTU}", "-f", "rawvideo", "-pix_fmt", "rgb24", str(raw)])
    d = raw.read_bytes()
    stride = W * 3
    groups, col_g, tiles = {}, [], []
    for col in range(COLS):
        tile = b"".join(d[y * stride + col * CTU * 3:y * stride + (col + 1) * CTU * 3] for y in range(rows * CTU))
        h = hashlib.md5(tile).hexdigest()
        if h not in groups:
            groups[h] = len(groups)
            png = td / f"tile_{name}_{groups[h]}.png"
            run([FF, "-hide_banner", "-loglevel", "error", "-y", "-i", str(bg_png), "-vf",
                 f"crop={CTU}:{rows * CTU}:{col * CTU}:{row0 * CTU}", str(png)])
            tiles.append(png)
        col_g.append(groups[h])
    return col_g, tiles


def encode_cell_row(td: Path, key: str, field, tile: Path, row: int, out: Path):
    name, _, rows, cell, size, *_ = field
    font = font_path()
    draw = ""
    pitch = CTU // cell                          # ogni carattere al centro della sua parte di cella (passo fisso)
    for i, ch in enumerate(key):
        if ch == " ":
            continue
        tf = textfile(td, f"t_{name}_{ord(ch):02x}.txt", ch)
        draw += (f",drawtext=fontfile='{font}':textfile='{tf}':fontcolor=white:fontsize={size}"
                 f":x={i * pitch}+({pitch}-text_w)/2:y=(h-{size})/2:y_align=font")
    run([FF, "-hide_banner", "-loglevel", "error", "-y", "-i", str(tile),
         "-frames:v", "1", "-vf", f"format=yuv420p{draw},crop={CTU}:{CTU}:0:{row * CTU}", "-c:v", "libx265", "-crf", "20",
         "-x265-params", X265, "-f", "hevc", str(out)])


def build_glyphs(td: Path, field, tiles, msps, mpps):
    """{(chiave, gruppo): [(dati, qp, sao) per riga]}"""
    name, _, rows, cell, _, charset, _ = field
    keys = list(charset) if cell == 1 else [a + b for a in charset for b in charset]
    glyphs = {}
    n, tot = 0, len(keys) * len(tiles)
    for gi, tile in enumerate(tiles):
        for key in keys:
            parts = []
            for r in range(rows):
                gp = td / "g.hevc"
                encode_cell_row(td, key, field, tile, r, gp)
                gt = trace(gp)
                gsps, gpps = first(gt, "Sequence Parameter Set"), first(gt, "Picture Parameter Set")
                diff = {k for k in set(msps) | set(gsps) if msps.get(k) != gsps.get(k)}
                diff |= {f"pps.{k}" for k in set(mpps) | set(gpps) if mpps.get(k) != gpps.get(k)}
                if diff - ALLOWED_DIFF:
                    raise SystemExit(f"parametri incompatibili per {key!r}: {sorted(diff - ALLOWED_DIFF)}")
                heads = [s for s in gt if s[0] == "Slice Segment Header"]
                data = gp.read_bytes()
                slices = [data[b:e] for _, b, e, t in nals_pos(data) if t < 32]
                assert len(heads) == len(slices) == 1
                f = {nm: v for _, nm, v in heads[0][1]}
                end = max(p for p, nm, _ in heads[0][1] if nm.startswith("alignment_bit")) + 1
                assert end % 8 == 0
                rb = unescape(slices[0])
                qp = 26 + gpps["init_qp_minus26"] + f["slice_qp_delta"]
                sao = f.get("slice_sao_luma_flag", 0) | f.get("slice_sao_chroma_flag", 0) << 1
                parts.append((rb[end // 8:], qp, sao))
            glyphs[(key, gi)] = parts
            n += 1
            if n % 100 == 0:
                print(f"  {name}: {n}/{tot} celle")
    return keys, glyphs


# ---------------------------------------------------------------- fotogrammi P: strisce sempre copiate
def ue_bits(v):
    v += 1
    n = v.bit_length()
    return [0] * (n - 1) + [(v >> (n - 1 - i)) & 1 for i in range(n)]


def se_bits(v):
    return ue_bits(2 * v - 1 if v > 0 else -2 * v)


def bits_of(b: bytes):
    return [(x >> (7 - i)) & 1 for x in b for i in range(8)]


def bytes_of(bits):
    return bytes(int("".join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8))


def skip_donor(td: Path, mpps):
    """slice P di una riga di CTU tutte in skip (copia dal riferimento): dati e campi d'intestazione che li governano"""
    p = td / "donor.hevc"
    run([FF, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", f"color=c=gray:s={W}x{CTU}:r=25",
         "-frames:v", "2", "-vf", "format=yuv420p", "-c:v", "libx265", "-crf", "20", "-x265-params", X265,
         "-f", "hevc", str(p)])
    t = trace(p)
    heads = [s for s in t if s[0] == "Slice Segment Header"]
    data = p.read_bytes()
    sl = [data[b:e] for _, b, e, ty in nals_pos(data) if ty < 32]
    assert len(heads) == len(sl) == 2
    f = {nm: v for _, nm, v in heads[1][1]}
    assert f["slice_type"] == 1, "il secondo fotogramma del donatore non è P"
    end = max(q for q, nm, _ in heads[1][1] if nm.startswith("alignment_bit")) + 1
    gpps = first(t, "Picture Parameter Set")
    return dict(data=unescape(sl[1])[end // 8:], qp=26 + gpps["init_qp_minus26"] + f["slice_qp_delta"],
                sao_l=f.get("slice_sao_luma_flag", 0), sao_c=f.get("slice_sao_chroma_flag", 0),
                merge=f["five_minus_max_num_merge_cand"])


def copy_strips_in_p(units, mt, donor, mpps, abits):
    """nei fotogrammi P le slice delle righe delle strisce diventano 'tutto skip': le strisce composte dalla C5 negli
    IDR restano identiche fino al GOP successivo (altrimenti x265 le ritocca verso il suo sfondo)"""
    rows = {row0 + r for _, row0, nr, *_ in FIELDS for r in range(nr)}
    heads = iter([s for s in mt if s[0] == "Slice Segment Header"])
    n = 0
    for ui, (kind, pts, d) in enumerate(units):
        if kind:
            continue
        out, pos = bytearray(), 0
        for s, b, e, t in nals_pos(d):
            if t >= 32:
                continue
            hs = next(heads)
            f = {nm: v for _, nm, v in hs[1]}
            if t in (19, 20) or f.get("slice_type") != 1:
                continue
            addr = f.get("slice_segment_address", 0)
            if addr // COLS not in rows:
                continue
            rb = unescape(d[b:e])
            fl = hs[1]
            end = max(q for q, nm, _ in fl if nm.startswith("alignment_bit"))
            first_align = min(q for q, nm, _ in fl if nm.startswith("alignment_bit"))
            bits = bits_of(rb)[:first_align]
            # campi da riscrivere, dal fondo per non spostare le posizioni precedenti
            pos_of = {nm: q for q, nm, _ in fl}
            order = sorted(((q, nm) for q, nm, _ in fl if q < first_align), key=lambda x: x[0])
            nxt = {nm: (order[i + 1][0] if i + 1 < len(order) else first_align) for i, (q, nm) in enumerate(order)}
            new = {"slice_qp_delta": se_bits(donor["qp"] - 26 - mpps["init_qp_minus26"]),
                   "five_minus_max_num_merge_cand": ue_bits(donor["merge"]),
                   "slice_sao_luma_flag": [donor["sao_l"]], "slice_sao_chroma_flag": [donor["sao_c"]]}
            for nm in sorted((k for k in new if k in pos_of), key=lambda k: -pos_of[k]):
                bits = bits[:pos_of[nm]] + new[nm] + bits[nxt[nm]:]
            bits += [1] + [0] * ((-(len(bits) + 1)) % 8)
            nal = escape(bytes_of(bits) + donor["data"])
            out += d[pos:b] + nal
            pos = e
            n += 1
        if pos:
            out += d[pos:]
            units[ui] = (kind, pts, bytes(out))
    return n


# ---------------------------------------------------------------- composizione (stesso algoritmo del firmware)
def slice_header(addr, qp, sao, g):
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


def build_blob(fi, text, glyphs, col_g, nalh, g):
    """slice della striscia fi per il testo dato: NAL Annex B in ordine di indirizzo"""
    name, row0, rows, cell, _, charset, _ = FIELDS[fi]
    text = "".join(c if c in charset else " " for c in (text.upper() if name == "call" else text))
    cells = [text[i:i + cell].ljust(cell) for i in range(0, len(text), cell)][:COLS]
    c0 = (COLS - len(cells)) // 2
    blank = " " * cell
    out = bytearray()
    for r in range(rows):
        for col in range(COLS):
            k = col - c0
            key = cells[k] if 0 <= k < len(cells) else blank
            gk = (key, col_g[col]) if (key, col_g[col]) in glyphs else (blank, col_g[col])
            data, qp, sao = glyphs[gk][r]
            hdr = slice_header((row0 + r) * COLS + col, qp, sao, g)
            out += b"\x00\x00\x00\x01" + escape(nalh + hdr + data)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sr", type=int, default=333000, help="SR più basso a cui verrà trasmesso (bitrate del video)")
    ap.add_argument("--image", help="immagine di sfondo (adattata a 640x360)")
    ap.add_argument("--video", help="video di sfondo (ripetuto se più corto, tagliato se più lungo)")
    ap.add_argument("--audio", help="audio al posto del tono da 1 kHz (anche lo stesso file del video)")
    ap.add_argument("--loops", type=int, default=1, help="durata in multipli di 19,2 s")
    ap.add_argument("--text", action="store_true", help="scritte BEACON DATV / DVB-S2 (sempre nel monoscopio standard)")
    ap.add_argument("--band", action="store_true", help="fascia con contatore (sempre nel monoscopio standard)")
    ap.add_argument("-o", "--output", default="../out/c5_mono_es_neutral.bin")
    ap.add_argument("--preview", nargs=2, metavar=("CALL", "FREQ"), help="anteprima: compone come la C5 e decodifica")
    a = ap.parse_args()
    if a.image and a.video:
        raise SystemExit("--image oppure --video, non tutti e due")
    if not (a.image or a.video):
        a.text = a.band = True                  # monoscopio standard
    loop_s = LOOP_UNIT_S * a.loops
    out_path = Path(a.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tdn:
        td = Path(tdn)
        tsp, bg = td / "m.ts", td / "bg.png"
        make_template_ts(td, a, loop_s, tsp, bg)
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

        # unità come make_es.py (ESP32-S3): video da 0 per un loop, audio ridistribuito in frame AAC da 1920 tick
        loop_ticks = int(round(loop_s * 90000))
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
        if len(frames) < need:
            raise SystemExit(f"solo {len(frames)} frame AAC, ne servono {need}")
        units += [(1, k * 1920, frames[k]) for k in range(need)]
        units.sort(key=lambda u: (u[1], u[0]))
        donor = skip_donor(td, mpps)
        print(f"fotogrammi P: {copy_strips_in_p(units, mt, donor, mpps, g['abits'])} slice delle strisce rese 'copia'")

        # posizione delle strisce negli IDR
        idr = []
        for ui, (kind, _, d) in enumerate(units):
            if kind:
                continue
            nl = nals_pos(d)
            if not any(t in (19, 20) for *_, t in nl):
                continue
            rowpos, nalh = {}, None
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
        print(f"unità {len(units)} ({sum(1 for u in units if u[0] == 0)} video), IDR {len(idr)}")

        tabs = []
        for field in FIELDS:
            col_g, tiles = column_groups(td, bg, field)
            print(f"caratteri {field[0]}: {len(tiles)} sfondi diversi nelle {COLS} colonne")
            keys, glyphs = build_glyphs(td, field, tiles, msps, mpps)
            tabs.append((keys, glyphs, col_g, len(tiles)))

    # ---- file PDXES002
    data, table = b"", b""
    for kind, pts, d in units:
        table += struct.pack("<B3xIII", kind, pts, len(data), len(d))
        data += d
    data_off = 64 + len(table)
    ovl_off = (data_off + len(data) + 3) & ~3
    oh_len, fd_len = 28, 24
    gt_off = oh_len + fd_len * len(FIELDS)
    gtabs = bytearray()
    fdesc = []
    for fi, (keys, glyphs, col_g, ng) in enumerate(tabs):
        rows = FIELDS[fi][2]
        off = gt_off + len(gtabs)
        gtabs += bytes(col_g) + b"\x00" * ((-len(col_g)) % 4)          # mappa colonna -> gruppo di sfondo
        ent0 = len(gtabs)
        for (key, gi) in glyphs:
            gtabs += key.encode().ljust(2, b"\x00")[:2] + bytes([gi, 0])
            gtabs += b"\x00" * (8 * rows)                               # {off, len, qp, sao} riempiti sotto
        fdesc.append([FIELDS[fi][1], rows, FIELDS[fi][3], ng, len(glyphs), off, FIELDS[fi][0], ent0])
    idr_off = gt_off + len(gtabs)
    idr_tab = b"".join(struct.pack("<I2s2x", ui, nalh) + b"".join(struct.pack("<II", s, n) for s, n in spans)
                       for ui, nalh, spans in idr)
    gdata_off = idr_off + len(idr_tab)
    gdata = bytearray()
    for fi, (keys, glyphs, col_g, ng) in enumerate(tabs):
        rows = FIELDS[fi][2]
        esz = 4 + 8 * rows
        for ei, gk in enumerate(glyphs):
            for r, (dat, qp, sao) in enumerate(glyphs[gk][:rows]):
                struct.pack_into("<IHBB", gtabs, fdesc[fi][7] + ei * esz + 4 + 8 * r, gdata_off + len(gdata), len(dat),
                                 qp, sao)
                gdata += dat
    flags = (1 if g["sao"] else 0) | (2 if g["entry"] else 0)
    ovl = struct.pack("<8sIII4Bb3x", b"PDXOVL02", len(FIELDS), len(idr), idr_off, COLS, g["abits"], flags, 0,
                      g["init_qp_minus26"])
    for row0, rows, cell, ng, n, off, name, _ in fdesc:
        ovl += struct.pack("<BBBBHHI8s4x", row0, rows, cell, ng, n, 0, off, name.encode())
    ovl += bytes(gtabs) + idr_tab + bytes(gdata)
    label = (Path(a.image or a.video).stem if (a.image or a.video) else "NEUTRAL").encode("ascii", "replace")[:31]
    hdr = struct.pack("<8sIIIIII32s", b"PDXES002", len(units), int(round(loop_s * 90000)), 0x24, 0x0F, data_off,
                      ovl_off, label)
    blob = hdr + table + data + b"\x00" * (ovl_off - data_off - len(data)) + ovl
    print(f"dimensioni: video+audio {len(data)}, sovrimpressione {len(ovl)} (caratteri {len(gdata)})")
    if len(blob) > ES_MAX:
        raise SystemExit(f"file di {len(blob)} byte: oltre i {ES_MAX} di spazio nella C5 (ridurre --loops o la durata)")
    out_path.write_bytes(blob)
    print(f"{out_path}: {len(blob)} byte, CRC32 {zlib.crc32(blob):08x}")
    if a.preview:
        preview(blob, a.preview, out_path.with_suffix(""))


def preview(blob: bytes, texts, stem: Path):
    """compone come la C5 (dalle sole informazioni del file) e decodifica alcune immagini con ffmpeg"""
    magic, nunits, _, _, _, data_off, ovl_off = struct.unpack_from("<8sIIIIII", blob, 0)
    assert magic == b"PDXES002"
    units = [struct.unpack_from("<B3xIII", blob, 64 + 16 * i) for i in range(nunits)]
    o = ovl_off
    om, nf, nidr, idr_off, cols, abits, flags, _, iq = struct.unpack_from("<8sIII4Bb3x", blob, o)
    assert om == b"PDXOVL02"
    g = dict(abits=abits, sao=flags & 1, entry=flags >> 1 & 1, init_qp_minus26=iq)
    blobs = []
    for f in range(nf):
        row0, rows, cell, ng, n, _, toff, _ = struct.unpack_from("<BBBBHHI8s4x", blob, o + 28 + 24 * f)
        col_g = list(blob[o + toff:o + toff + cols])
        e0 = o + toff + cols + ((-cols) % 4)
        esz = 4 + 8 * rows
        glyphs = {}
        for k in range(n):
            e = e0 + k * esz
            key = blob[e:e + 2].rstrip(b"\x00").decode()
            parts = []
            for r in range(rows):
                doff, dlen, qp, sao = struct.unpack_from("<IHBB", blob, e + 4 + 8 * r)
                parts.append((blob[o + doff:o + doff + dlen], qp, sao))
            glyphs[(key, blob[e + 2])] = parts
        blobs.append((glyphs, col_g))
    esz = 8 + 8 * nf
    idr = {}
    for i in range(nidr):
        ui, nalh = struct.unpack_from("<I2s2x", blob, o + idr_off + i * esz)
        idr[ui] = (nalh, [struct.unpack_from("<II", blob, o + idr_off + i * esz + 8 + 8 * f) for f in range(nf)])
    nalh0 = next(iter(idr.values()))[0]
    fblobs = [build_blob(fi, texts[fi], blobs[fi][0], blobs[fi][1], nalh0, g) for fi in range(nf)]
    for fi, b in enumerate(fblobs):
        print(f"striscia {FIELDS[fi][0]} {texts[fi]!r}: {len(b)} byte, CRC32 {zlib.crc32(b):08x}")
    video = bytearray()
    for ui, (kind, _, off, ln) in enumerate(units):
        if kind:
            continue
        d = blob[data_off + off:data_off + off + ln]
        if ui in idr:
            pieces, pos = [], 0
            for (s, n), fb in sorted(zip(idr[ui][1], fblobs)):
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

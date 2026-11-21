/* ESP32-S3-CAM_PDX DATV Beacon - IZ7PDX - GPL-3.0
 * Codificatore DVB-S2 (vedi dvbs2_enc.h). Verificato bit per bit contro tools/dvbs2/encoder.py con il comando S2TEST. */
/* Portato sull'ESP32-C5 (P4a, misura dei tempi): memoria di lavoro nel banco 1 (0x40820000, 128 KB, libero
 * quando non si trasmette) invece della PSRAM, con un allocatore a pila azzerato da pdx_s2_work_reset(). */
#include "dvbs2_enc.h"

#include <stdlib.h>
#include <string.h>
#include "dvbs2_tables.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

static const char *RATES[] = {"1/4", "1/3", "2/5", "1/2", "3/5", "2/3", "3/4", "4/5", "5/6", "8/9", "9/10"};
static const char PLS_SCRAMBLE[] = "0111000110011101100000111100100101010011010000100010110111111010";
static const uint32_t PLS_ROWS[6] = {0x55555555u, 0x33333333u, 0x0F0F0F0Fu, 0x00FF00FFu, 0x0000FFFFu, 0xFFFFFFFFu};

/* il lavoro del codificatore sta in PSRAM: in RAM interna le continue letture/scritture del core 1 contendevano la
 * memoria al core 0 (buffer dei simboli, banco del DAC) e i campioni uscivano in ritardo (ESPLAY del 30/10) */
static uint32_t work_off;
void pdx_s2_work_reset(void) { work_off = 0; }
static void *alloc_work(size_t n)
{
    n = (n + 15u) & ~15u;
    if (work_off + n > 0x20000u) return NULL;
    void *p = (void *)(0x40820000u + work_off);
    work_off += n;
    memset(p, 0, n);
    return p;
}

/* 5.1.4: X^8+X^7+X^6+X^4+X^2+1, bit più significativo prima. C5 (21/11): a tabella costante in flash (prima bit per
 * bit: ~9000 cicli per pacchetto TS, troppo per i tempi morti del modulatore) */
static const uint8_t CRC8_TAB[256] = {
    0,213,127,170,254,43,129,84,41,252,86,131,215,2,168,125,
    82,135,45,248,172,121,211,6,123,174,4,209,133,80,250,47,
    164,113,219,14,90,143,37,240,141,88,242,39,115,166,12,217,
    246,35,137,92,8,221,119,162,223,10,160,117,33,244,94,139,
    157,72,226,55,99,182,28,201,180,97,203,30,74,159,53,224,
    207,26,176,101,49,228,78,155,230,51,153,76,24,205,103,178,
    57,236,70,147,199,18,184,109,16,197,111,186,238,59,145,68,
    107,190,20,193,149,64,234,63,66,151,61,232,188,105,195,22,
    239,58,144,69,17,196,110,187,198,19,185,108,56,237,71,146,
    189,104,194,23,67,150,60,233,148,65,235,62,106,191,21,192,
    75,158,52,225,181,96,202,31,98,183,29,200,156,73,227,54,
    25,204,102,179,231,50,152,77,48,229,79,154,206,27,177,100,
    114,167,13,216,140,89,243,38,91,142,36,241,165,112,218,15,
    32,245,95,138,222,11,161,116,9,220,118,163,247,34,136,93,
    214,3,169,124,40,253,87,130,255,42,128,85,1,212,126,171,
    132,81,251,46,122,175,5,208,173,120,210,7,83,134,44,249};

uint8_t pdx_s2_crc8(const uint8_t *p, uint32_t n)
{
    uint32_t crc = 0;
    for (uint32_t i = 0; i < n; i++) crc = CRC8_TAB[crc ^ p[i]];
    return (uint8_t)crc;
}

/* ------------------------------------------------------------ BCH (resto in 192 bit, allineato in alto) */

static void w_shl(uint32_t w[6], int n)                 /* n = 1..8 */
{
    for (int i = 0; i < 5; i++) w[i] = (w[i] << n) | (w[i + 1] >> (32 - n));
    w[5] <<= n;
}

static void bch_build(pdx_s2_t *s, const uint8_t *g, uint32_t deg)
{
    uint32_t gl[6] = {0};                                 /* coefficienti x^(deg-1) .. x^0 allineati al bit 191 */
    for (uint32_t i = 0; i < deg; i++) {
        const uint32_t src = i + 1;                       /* salta x^deg */
        if (g[src >> 3] & (0x80u >> (src & 7))) gl[i >> 5] |= 0x80000000u >> (i & 31);
    }
    for (int v = 0; v < 256; v++) {
        uint32_t r[6] = {0};
        for (int k = 7; k >= 0; k--) {
            const uint32_t fb = (r[0] >> 31) ^ ((uint32_t)(v >> k) & 1u);
            w_shl(r, 1);
            if (fb) for (int i = 0; i < 6; i++) r[i] ^= gl[i];
        }
        memcpy(s->bch_tab[v], r, sizeof(r));
    }
    s->deg = deg;
}

/* parità BCH dei primi kbch bit di cw, scritta subito dopo (deg bit) */
static void bch_encode(pdx_s2_t *s)
{
    uint32_t r[6] = {0};
    const uint32_t nb = s->kbch / 8u;
    for (uint32_t i = 0; i < nb; i++) {
        const uint32_t idx = (r[0] >> 24) ^ s->cw[i];
        w_shl(r, 8);
        const uint32_t *t = s->bch_tab[idx];
        for (int k = 0; k < 6; k++) r[k] ^= t[k];
    }
    for (uint32_t i = 0; i < s->deg / 8u; i++) s->cw[nb + i] = (uint8_t)(r[i >> 2] >> (24 - 8 * (i & 3)));
}

/* ------------------------------------------------------------ LDPC (5.3.2) */

/* Gli indirizzi di un gruppo di 360 bit di informazione sono x + m*q (m = 0..359) modulo nk = 360 q: con x = c q + a
 * diventano a + q ((c + m) mod 360). Con la parità disposta in q righe da 360 bit (P[a], bit j = parità a + q j), ogni
 * voce della tabella è lo XOR del gruppo G (bit m = informazione 360 r + m) ruotato di c in P[a]: 12 parole a 32 bit
 * invece di 360 bit uno per uno. */
#define W360 12u                                          /* parole per 360 bit (384, gli ultimi 24 inutilizzati) */
static uint8_t bitrev8[256];

static void ldpc_encode(pdx_s2_t *s)
{
    const pdx_s2_table_t *t = s->tab;
    uint8_t *info = s->cw, *par = s->cw + s->kldpc / 8u;
    const uint32_t q = t->q, nk = s->nk;
    uint32_t (*P)[W360] = s->ldpc_rows;                   /* q righe */
    memset(P, 0, q * sizeof(P[0]));
    const uint16_t *a = t->addr;
    uint32_t D[2 * W360];                                 /* gruppo raddoppiato: D[k] = G[k mod 360], k = 0..719 */
    for (uint32_t r = 0; r < t->nrows; r++) {
        const uint8_t *g8 = info + 45u * r;               /* 360 bit = 45 byte, bit più significativo per primo */
        uint32_t G[W360] = {0};
        for (uint32_t i = 0; i < 45u; i++) G[i >> 2] |= (uint32_t)bitrev8[g8[i]] << ((i & 3u) * 8u);
        memset(D, 0, sizeof(D));
        for (uint32_t w = 0; w < W360; w++) D[w] = G[w];
        for (uint32_t w = 0; w < W360; w++) {             /* copia di G a partire dal bit 360 (= parola 11, bit 8) */
            D[11 + w] |= G[w] << 8;
            D[12 + w] |= G[w] >> 24;
        }
        const uint32_t len = t->rowlen[r];
        for (uint32_t j = 0; j < len; j++) {
            const uint32_t x = a[j], c = x / q, row = x - c * q;
            const uint32_t o = 360u - c;                  /* R[j] = D[j + 360 - c] */
            const uint32_t ow = o >> 5, ob = o & 31u;
            uint32_t *pr = P[row];
            if (ob) {
                for (uint32_t w = 0; w < W360; w++) pr[w] ^= (D[ow + w] >> ob) | (D[ow + w + 1] << (32u - ob));
            } else {
                for (uint32_t w = 0; w < W360; w++) pr[w] ^= D[ow + w];
            }
        }
        a += len;
    }
    memset(par, 0, nk / 8u);                              /* di nuovo in ordine lineare: parità a + q j */
    for (uint32_t row = 0; row < q; row++) {
        P[row][11] &= 0xFFu;                              /* solo i bit 352..359 */
        for (uint32_t w = 0; w < W360; w++) {
            uint32_t v = P[row][w];
            while (v) {
                const uint32_t b = (uint32_t)__builtin_ctz(v);
                v &= v - 1u;
                const uint32_t p = row + q * (w * 32u + b);
                par[p >> 3] |= (uint8_t)(0x80u >> (p & 7));
            }
        }
    }
    uint32_t carry = 0;                                   /* p_i = p_i xor p_(i-1): xor cumulativo, bit più significativo prima */
    for (uint32_t i = 0; i < nk / 8u; i++) {
        uint32_t v = par[i];
        v ^= v >> 1;
        v ^= v >> 2;
        v ^= v >> 4;
        if (carry) v ^= 0xFFu;
        par[i] = (uint8_t)v;
        carry = v & 1u;
    }
}

/* ------------------------------------------------------------ PL framing (5.5) */

static uint8_t sym4[256];      /* byte della parola di codice -> 4 codici QPSK (5.4.1: bit 0 -> +1), primo nei bit bassi */
static uint8_t rot_lut[16];    /* codice | R << 2 -> codice ruotato */

static inline uint32_t rotate(uint32_t code, uint32_t r)   /* (I + jQ) * j^R sui codici (bit 0 = I > 0, bit 1 = Q > 0) */
{
    const uint32_t i = code & 1u, q = code >> 1;
    switch (r) {
    case 1: return (q ^ 1u) | (i << 1);                   /* (-Q,  I) */
    case 2: return (i ^ 1u) | ((q ^ 1u) << 1);            /* (-I, -Q) */
    case 3: return q | ((i ^ 1u) << 1);                   /* ( Q, -I) */
    default: return code;
    }
}

static void build_header(pdx_s2_t *s, uint32_t modcod)
{
    uint8_t y[90];
    for (int i = 0; i < 26; i++) y[i] = (uint8_t)((0x18D2E82u >> (25 - i)) & 1u);         /* SOF */
    const uint32_t pls = (modcod << 2) | ((uint32_t)s->shortf << 1) | (uint32_t)s->pilots;  /* b1..b7 */
    uint32_t cw32 = 0;
    for (int k = 0; k < 6; k++) if ((pls >> (6 - k)) & 1u) cw32 ^= PLS_ROWS[k];
    const uint32_t b7 = pls & 1u;
    for (int i = 0; i < 32; i++) {
        const uint8_t v = (uint8_t)((cw32 >> (31 - i)) & 1u);
        y[26 + 2 * i] = (uint8_t)(v ^ (PLS_SCRAMBLE[2 * i] - '0'));
        y[27 + 2 * i] = (uint8_t)((v ^ b7) ^ (PLS_SCRAMBLE[2 * i + 1] - '0'));
    }
    for (int i = 0; i < 90; i++)                          /* pi/2 BPSK: pari (da 0) I = Q = a, dispari I = -a, Q = a */
        s->hdr[i] = (i & 1) ? (uint8_t)((y[i] ? 1u : 0u) | ((y[i] ? 0u : 1u) << 1))
                            : (uint8_t)(y[i] ? 0u : 3u);
}

/* R(i) = 2 z((i + 131072) mod (2^18-1)) + z(i), z(i) = x(i) xor y(i) (codice n = 0) */
static void build_rotation(pdx_s2_t *s)
{
    uint32_t xa = 1u, ya = 0x3FFFFu, xb = 1u, yb = 0x3FFFFu;
#define XSTEP(st) (st = ((st) >> 1) | (((((st) >> 7) ^ (st)) & 1u) << 17))
#define YSTEP(st) (st = ((st) >> 1) | (((((st) >> 10) ^ ((st) >> 7) ^ ((st) >> 5) ^ (st)) & 1u) << 17))
    for (uint32_t i = 0; i < 131072u; i++) { XSTEP(xb); YSTEP(yb); }
    const uint32_t n = s->nsym - 90u;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t za = (xa ^ ya) & 1u, zb = (xb ^ yb) & 1u;
        s->rot[i >> 2] |= (uint8_t)(((zb << 1) | za) << ((i & 3u) * 2u));
        XSTEP(xa); YSTEP(ya); XSTEP(xb); YSTEP(yb);
    }
#undef XSTEP
#undef YSTEP
}

void pdx_s2_frame(pdx_s2_t *s, const uint8_t *data_field, uint16_t syncd, pdx_s2_out_t *out)
{
    const int64_t t0 = esp_timer_get_time();
    uint8_t *bb = s->cw;                                  /* BBHEADER + DATA FIELD, poi BB scrambling (5.1.6, 5.2) */
    const uint32_t dfl = s->dfl;
    bb[0] = 0xF0;                                         /* TS, singolo flusso, CCM, ISSYI 0, NPD 0, roll-off 0,35 */
    bb[1] = 0x00;
    bb[2] = 1504 >> 8;
    bb[3] = 1504 & 255;
    bb[4] = (uint8_t)(dfl >> 8);
    bb[5] = (uint8_t)dfl;
    bb[6] = 0x47;
    bb[7] = (uint8_t)(syncd >> 8);
    bb[8] = (uint8_t)syncd;
    bb[9] = pdx_s2_crc8(bb, 9);
    memcpy(bb + 10, data_field, dfl / 8u);
    for (uint32_t i = 0; i < s->kbch / 8u; i++) bb[i] ^= s->prbs[i];
    bch_encode(s);
    const int64_t t1 = esp_timer_get_time();
    ldpc_encode(s);
    const int64_t t2 = esp_timer_get_time();

    /* scrittura a byte interi: accumulatore di 4 simboli, qualunque sia l'allineamento iniziale */
    uint32_t idx = out->pos >> 2, nacc = out->pos & 3u;
    uint32_t acc = out->buf[idx] & ((1u << (2u * nacc)) - 1u);
#define EMIT(code)                                                   \
    do {                                                             \
        acc |= (uint32_t)(code) << (2u * nacc);                      \
        if (++nacc == 4u) { out->buf[idx++] = (uint8_t)acc; acc = 0; nacc = 0; } \
    } while (0)
    for (int i = 0; i < 90; i++) EMIT(s->hdr[i]);
    /* dati: ogni byte della parola di codice dà 4 simboli (sym4); lo scrambling ruota ogni simbolo (rot_lut). Le pilot
     * sono blocchi da 36 simboli (multiplo di 4), quindi la sequenza R resta allineata ai byte dei dati. */
    const uint32_t nbytes = s->nldpc / 8u;
    uint32_t ri = 0;
    for (uint32_t b = 0; b < nbytes; b++) {
        const uint32_t codes = sym4[s->cw[b]], rr = s->rot[ri >> 2];
        for (uint32_t k = 0; k < 4u; k++) EMIT(rot_lut[((codes >> (2u * k)) & 3u) | (((rr >> (2u * k)) & 3u) << 2)]);
        ri += 4u;
        if (s->pilots && (b + 1u) % 360u == 0 && b + 1u < nbytes) {   /* 1440 simboli = 360 byte; pilot non alla fine */
            for (uint32_t p = 0; p < 36u; p += 4u, ri += 4u) {
                const uint32_t rp = s->rot[ri >> 2];
                for (uint32_t k = 0; k < 4u; k++) EMIT(rot_lut[3u | (((rp >> (2u * k)) & 3u) << 2)]);
            }
        }
    }
    if (nacc) out->buf[idx] = (uint8_t)acc;
#undef EMIT
    out->pos += 90u + s->nldpc / 2u + (s->pilots ? 36u * ((s->slots - 1u) / 16u) : 0u);
    const int64_t t3 = esp_timer_get_time();
    s->t_bch_us = (uint32_t)(t1 - t0);
    s->t_ldpc_us = (uint32_t)(t2 - t1);
    s->t_map_us = (uint32_t)(t3 - t2);
}

double pdx_s2_ts_bitrate(const pdx_s2_t *s, uint32_t sr) { return (double)sr * s->dfl / s->nsym; }

void pdx_s2_free(pdx_s2_t *s)
{
    memset(s, 0, sizeof(*s));
}

const char *pdx_s2_init(pdx_s2_t *s, const char *rate, bool shortf, bool pilots)
{
    memset(s, 0, sizeof(*s));
    int ri = -1;
    for (int i = 0; i < 11; i++) if (!strcmp(rate, RATES[i])) ri = i;
    if (ri < 0 || (shortf && ri == 10)) return "rate non disponibile";
    const pdx_s2_table_t *t = &pdx_s2_tables[shortf ? 11 + ri : ri];
    s->tab = t;
    s->shortf = shortf;
    s->pilots = pilots;
    s->kbch = t->kbch;
    s->dfl = t->kbch - 80u;
    s->nldpc = t->nldpc;
    s->kldpc = t->kldpc;
    s->nk = t->nldpc - t->kldpc;
    s->slots = t->nldpc / 2u / 90u;
    s->nsym = 90u * (s->slots + 1u) + (pilots ? 36u * ((s->slots - 1u) / 16u) : 0u);

    s->bch_tab = alloc_work((size_t)(256) * (sizeof(s->bch_tab[0])));
    s->prbs = alloc_work((size_t)(1) * (s->kbch / 8u));
    s->rot = alloc_work((size_t)(1) * ((s->nsym - 90u + 3u) / 4u));
    s->cw = alloc_work(s->nldpc / 8u);
    s->ldpc_rows = alloc_work(t->q * sizeof(s->ldpc_rows[0]));
    if (!s->bch_tab || !s->prbs || !s->rot || !s->cw || !s->ldpc_rows) { pdx_s2_free(s); return "memoria"; }

    const uint8_t *g = shortf ? s2_bch_short_t12 : t->t == 12 ? s2_bch_normal_t12 : t->t == 10 ? s2_bch_normal_t10
                                                                                            : s2_bch_normal_t8;
    bch_build(s, g, shortf ? 168u : 16u * t->t);
    if (s->deg != t->nbch - t->kbch) { pdx_s2_free(s); return "grado BCH"; }

    uint8_t st[15] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0};   /* 5.2.2: stadi 1..15 = 100101010000000 */
    for (uint32_t i = 0; i < s->kbch; i++) {
        const uint8_t b = st[13] ^ st[14];
        if (b) s->prbs[i >> 3] |= (uint8_t)(0x80u >> (i & 7));
        memmove(st + 1, st, 14);
        st[0] = b;
    }
    for (uint32_t v = 0; v < 256u; v++) {
        uint32_t r = 0, c = 0;
        for (int k = 0; k < 8; k++) r |= ((v >> k) & 1u) << (7 - k);
        bitrev8[v] = (uint8_t)r;
        for (uint32_t k = 0; k < 4u; k++) {               /* simbolo k: bit 7-2k (I) e 6-2k (Q) */
            const uint32_t b0 = (v >> (7u - 2u * k)) & 1u, b1 = (v >> (6u - 2u * k)) & 1u;
            c |= ((b0 ^ 1u) | ((b1 ^ 1u) << 1)) << (2u * k);
        }
        sym4[v] = (uint8_t)c;
    }
    for (uint32_t i = 0; i < 16u; i++) rot_lut[i] = (uint8_t)rotate(i & 3u, i >> 2);
    build_header(s, t->modcod);
    build_rotation(s);
    return NULL;
}

/* =====================================================================================================================
 * C5 (P4b): codifica a passi brevi e mappatura "pigra" per il TS continuo con un solo core.
 * Stesso algoritmo delle funzioni sopra (che restano il riferimento di S2LTEST); niente tabelle prbs/rot in RAM:
 * lo scrambling BB e la rotazione PL si generano al volo. */
#include "esp_attr.h"

const char *pdx_s2l_init(pdx_s2_t *s, const char *rate, bool shortf, bool pilots, void *bch_mem, void *rows_mem,
                         uint32_t rows_cap)
{
    memset(s, 0, sizeof(*s));
    int ri = -1;
    for (int i = 0; i < 11; i++) if (!strcmp(rate, RATES[i])) ri = i;
    if (ri < 0 || (shortf && ri == 10)) return "rate non disponibile";
    const pdx_s2_table_t *t = &pdx_s2_tables[shortf ? 11 + ri : ri];
    if (t->q > rows_cap) return "rate troppo basso per la memoria LDPC";
    s->tab = t;
    s->shortf = shortf;
    s->pilots = pilots;
    s->kbch = t->kbch;
    s->dfl = t->kbch - 80u;
    s->nldpc = t->nldpc;
    s->kldpc = t->kldpc;
    s->nk = t->nldpc - t->kldpc;
    s->slots = t->nldpc / 2u / 90u;
    s->nsym = 90u * (s->slots + 1u) + (pilots ? 36u * ((s->slots - 1u) / 16u) : 0u);
    s->bch_tab = bch_mem;
    s->ldpc_rows = rows_mem;
    memset(bch_mem, 0, 256u * sizeof(s->bch_tab[0]));
    const uint8_t *g = shortf ? s2_bch_short_t12 : t->t == 12 ? s2_bch_normal_t12 : t->t == 10 ? s2_bch_normal_t10
                                                                                            : s2_bch_normal_t8;
    bch_build(s, g, shortf ? 168u : 16u * t->t);
    if (s->deg != t->nbch - t->kbch) return "grado BCH";
    for (uint32_t v = 0; v < 256u; v++) {
        uint32_t r = 0, c = 0;
        for (int k = 0; k < 8; k++) r |= ((v >> k) & 1u) << (7 - k);
        bitrev8[v] = (uint8_t)r;
        for (uint32_t k = 0; k < 4u; k++) {
            const uint32_t b0 = (v >> (7u - 2u * k)) & 1u, b1 = (v >> (6u - 2u * k)) & 1u;
            c |= ((b0 ^ 1u) | ((b1 ^ 1u) << 1)) << (2u * k);
        }
        sym4[v] = (uint8_t)c;
    }
    for (uint32_t i = 0; i < 16u; i++) rot_lut[i] = (uint8_t)rotate(i & 3u, i >> 2);
    build_header(s, t->modcod);
    return NULL;
}

/* PLHEADER del frame DUMMY (MODCOD 0, TYPE 0) */
void pdx_s2l_dummy_header(const pdx_s2_t *s, uint8_t hdr[90])
{
    pdx_s2_t d = *s;
    d.shortf = false;
    d.pilots = false;
    build_header(&d, 0);
    memcpy(hdr, d.hdr, 90);
}

void pdx_s2l_job_start(pdx_s2l_job_t *j, pdx_s2_t *s, uint8_t *cw, uint16_t syncd)
{
    j->s = s;
    j->cw = cw;
    j->stage = 0;
    j->i = 0;
    j->prbs = 0xA9u;                                      /* 5.2.2: stadi 1..15 = 100101010000000 (bit k = stadio k+1) */
    memset(j->r, 0, sizeof(j->r));
    const uint32_t dfl = s->dfl;
    cw[0] = 0xF0;
    cw[1] = 0x00;
    cw[2] = 1504 >> 8;
    cw[3] = 1504 & 255;
    cw[4] = (uint8_t)(dfl >> 8);
    cw[5] = (uint8_t)dfl;
    cw[6] = 0x47;
    cw[7] = (uint8_t)(syncd >> 8);
    cw[8] = (uint8_t)syncd;
    cw[9] = pdx_s2_crc8(cw, 9);
}

#define S2L_SCR_BYTES 256u
#define S2L_BCH_BYTES 16u
#define S2L_XOR_BYTES 128u

/* un passo (al massimo qualche migliaio di cicli); true quando la parola di codice è completa */
bool pdx_s2l_job_step(pdx_s2l_job_t *j)                      /* in flash: non critico, ogni passo è misurato */
{
    pdx_s2_t *s = j->s;
    uint8_t *cw = j->cw;
    const pdx_s2_table_t *t = s->tab;
    switch (j->stage) {
    case 0: {                                             /* BB scrambling al volo */
        const uint32_t nb = s->kbch / 8u;
        uint32_t i = j->i, st = j->prbs;
        const uint32_t e = i + S2L_SCR_BYTES < nb ? i + S2L_SCR_BYTES : nb;
        for (; i < e; i++) {                              /* 8 bit per volta: con x^15 + x^14 + 1 dipendono tutti */
            const uint32_t by = ((st >> 6) ^ (st >> 7)) & 0xFFu;   /* dallo stato precedente (verificato sul PC) */
            st = ((st << 8) | by) & 0x7FFFu;
            cw[i] ^= (uint8_t)by;
        }
        j->i = i;
        j->prbs = st;
        if (i == nb) { j->stage = 1; j->i = 0; }
        return false;
    }
    case 1: {                                             /* BCH */
        const uint32_t nb = s->kbch / 8u;
        uint32_t i = j->i;
        uint32_t *r = j->r;
        const uint32_t e = i + S2L_BCH_BYTES < nb ? i + S2L_BCH_BYTES : nb;
        for (; i < e; i++) {
            const uint32_t idx = (r[0] >> 24) ^ cw[i];
            for (int k = 0; k < 5; k++) r[k] = (r[k] << 8) | (r[k + 1] >> 24);
            r[5] <<= 8;
            const uint32_t *tb = s->bch_tab[idx];
            for (int k = 0; k < 6; k++) r[k] ^= tb[k];
        }
        j->i = i;
        if (i == nb) {
            for (uint32_t k = 0; k < s->deg / 8u; k++) cw[nb + k] = (uint8_t)(r[k >> 2] >> (24 - 8 * (k & 3)));
            memset(s->ldpc_rows, 0, t->q * sizeof(s->ldpc_rows[0]));
            j->stage = 2;
            j->i = 0;
            j->a = t->addr;
        }
        return false;
    }
    case 2: {                                             /* LDPC: un gruppo di 360 bit di informazione per passo */
        const uint32_t r = j->i, q = t->q;
        uint32_t (*P)[W360] = s->ldpc_rows;
        const uint8_t *g8 = cw + 45u * r;
        uint32_t G[W360] = {0}, D[2 * W360] = {0};
        for (uint32_t i = 0; i < 45u; i++) G[i >> 2] |= (uint32_t)bitrev8[g8[i]] << ((i & 3u) * 8u);
        for (uint32_t w = 0; w < W360; w++) D[w] = G[w];
        for (uint32_t w = 0; w < W360; w++) {
            D[11 + w] |= G[w] << 8;
            D[12 + w] |= G[w] >> 24;
        }
        const uint32_t len = t->rowlen[r];
        const uint16_t *a = j->a;
        for (uint32_t k = 0; k < len; k++) {
            const uint32_t x = a[k], c = x / q, row = x - c * q;
            const uint32_t o = 360u - c, ow = o >> 5, ob = o & 31u;
            uint32_t *pr = P[row];
            if (ob) {
                for (uint32_t w = 0; w < W360; w++) pr[w] ^= (D[ow + w] >> ob) | (D[ow + w + 1] << (32u - ob));
            } else {
                for (uint32_t w = 0; w < W360; w++) pr[w] ^= D[ow + w];
            }
        }
        j->a = a + len;
        if (++j->i == t->nrows) {
            memset(cw + s->kldpc / 8u, 0, s->nk / 8u);
            j->stage = 3;
            j->i = 0;
            j->r[0] = 0;
        }
        return false;
    }
    case 3: {                                             /* parità in ordine lineare: un terzo di riga per passo */
        const uint32_t row = j->i, q = t->q, w0 = j->r[0];
        uint8_t *par = cw + s->kldpc / 8u;
        uint32_t *pr = s->ldpc_rows[row];
        if (w0 == 0) pr[11] &= 0xFFu;
        for (uint32_t w = w0; w < w0 + 4u; w++) {
            uint32_t v = pr[w];
            while (v) {
                const uint32_t b = (uint32_t)__builtin_ctz(v);
                v &= v - 1u;
                const uint32_t p = row + q * (w * 32u + b);
                par[p >> 3] |= (uint8_t)(0x80u >> (p & 7));
            }
        }
        if (w0 + 4u < W360) { j->r[0] = w0 + 4u; return false; }
        j->r[0] = 0;
        if (++j->i == q) { j->stage = 4; j->i = 0; j->carry = 0; }
        return false;
    }
    case 4: {                                             /* xor cumulativo della parità */
        uint8_t *par = cw + s->kldpc / 8u;
        const uint32_t nb = s->nk / 8u;
        uint32_t i = j->i, carry = j->carry;
        const uint32_t e = i + S2L_XOR_BYTES < nb ? i + S2L_XOR_BYTES : nb;
        for (; i < e; i++) {
            uint32_t v = par[i];
            v ^= v >> 1;
            v ^= v >> 2;
            v ^= v >> 4;
            if (carry) v ^= 0xFFu;
            par[i] = (uint8_t)v;
            carry = v & 1u;
        }
        j->i = i;
        j->carry = carry;
        if (i == nb) { j->stage = 5; return true; }
        return false;
    }
    default:
        return true;
    }
}

/* ---- mappatura pigra: un simbolo alla volta dalla parola di codice (o frame DUMMY con cw = NULL) ---- */
static uint32_t gold_xb0, gold_yb0;

void pdx_s2l_map_init(void)
{
    uint32_t xb = 1u, yb = 0x3FFFFu;
    for (uint32_t i = 0; i < 131072u; i++) {
        xb = (xb >> 1) | ((((xb >> 7) ^ xb) & 1u) << 17);
        yb = (yb >> 1) | ((((yb >> 10) ^ (yb >> 7) ^ (yb >> 5) ^ yb) & 1u) << 17);
    }
    gold_xb0 = xb;
    gold_yb0 = yb;
}

void pdx_s2l_map_start(pdx_s2l_map_t *m, const pdx_s2_t *s, const uint8_t *cw, const uint8_t *hdr)
{
    m->cw = cw;
    m->hdr = hdr;
    m->k = 0;
    m->d = 0;
    m->blk = 0;
    m->pil = 0;
    m->xa = 1u;
    m->ya = 0x3FFFFu;
    m->xb = gold_xb0;
    m->yb = gold_yb0;
    if (cw) {
        m->ndata = s->nldpc / 2u;
        m->pilots = s->pilots;
        m->nsym = s->nsym;
    } else {                                              /* DUMMY: 36 slot di simboli non modulati */
        m->ndata = 36u * 90u;
        m->pilots = false;
        m->nsym = 90u + 36u * 90u;
    }
}

IRAM_ATTR uint32_t pdx_s2l_map_next(pdx_s2l_map_t *m)
{
    const uint32_t k = m->k++;
    if (k < 90u) return m->hdr[k];
    uint32_t code;
    if (m->pil) {                                         /* blocco pilota: (1+j) */
        m->pil--;
        code = 3u;
    } else {
        const uint32_t d = m->d++;
        if (m->cw) {
            const uint32_t by = m->cw[d >> 2], sh = 6u - 2u * (d & 3u);
            const uint32_t b0 = (by >> (sh + 1u)) & 1u, b1 = (by >> sh) & 1u;
            code = (b0 ^ 1u) | ((b1 ^ 1u) << 1);
        } else {
            code = 3u;
        }
        if (m->pilots && ++m->blk == 1440u && m->d < m->ndata) { m->blk = 0; m->pil = 36u; }
    }
    const uint32_t za = (m->xa ^ m->ya) & 1u, zb = (m->xb ^ m->yb) & 1u;
    m->xa = (m->xa >> 1) | ((((m->xa >> 7) ^ m->xa) & 1u) << 17);
    m->ya = (m->ya >> 1) | ((((m->ya >> 10) ^ (m->ya >> 7) ^ (m->ya >> 5) ^ m->ya) & 1u) << 17);
    m->xb = (m->xb >> 1) | ((((m->xb >> 7) ^ m->xb) & 1u) << 17);
    m->yb = (m->yb >> 1) | ((((m->yb >> 10) ^ (m->yb >> 7) ^ (m->yb >> 5) ^ m->yb) & 1u) << 17);
    return rot_lut[code | (((zb << 1) | za) << 2)];
}

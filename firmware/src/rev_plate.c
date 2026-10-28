/* SPDX-License-Identifier: GPL-3.0-only */
/* SNOOP 0.4: PLATE and SPACE, the reverb's two new TYPEs (FX > REV 2). ROOM is SLOOP's reverb (fx.c), bit for bit.
 *
 * Both are Jon Dattorro's plate ("Effect Design, Part 1", JAES 1997), the network VCV Plateau grows from: a band
 * limit, a pre-delay, four input all-passes, then a "figure eight" tank of two halves, each a modulated all-pass, a
 * long delay, a damping low-pass, the decay, a second all-pass and a second delay, each half feeding the other.
 * Seven taps on the tank's lines for each side. Here:
 *  - it runs at half the rate, 22.05 kHz (fs/2): half the memory (38 KB) and half the work; the send is averaged in
 *    pairs going in, the return interpolated coming out (the tail has nothing above 11 kHz: a reverb's air, not its
 *    body);
 *  - every length (the lines and the taps) is Dattorro's in seconds, times SIZE: 0.12 .. 1 (PLATE 0.25 .. 0.8);
 *    a SIZE change moves the read heads a sample per block (a slow sweep, as Plateau's, no click);
 *  - the two modulated all-passes swing up to +-40 samples (SPACE: two sines, 0.5 / 0.63 Hz, a slow random drift on
 *    top) or +-8 (PLATE, 0.9 Hz): MOD;
 *  - a high-pass (~14 Hz) in each half: long decays never build up rumble or DC;
 *  - DECAY (the REV/CHO page's SIZE knob): PLATE 0.30 .. 0.93, SPACE 0.45 .. 0.995 of loop gain (seconds to minutes);
 *    DAMP: the tank's low-pass, 9 kHz .. 0.9 kHz;
 *  - FREEZE (the punch-in): the loop gain exactly 1, no damping, no high-pass: the tail holds.
 * Memory: the input all-passes, the modulated all-passes and the first long delays in SLOOP's rev_line (ROOM's lines:
 * one TYPE plays at a time, a TYPE change clears them), the rest in plt_ext (.bss). */

#define PLT_FS 22050
#define PLT_SC(n) (((n) * 2u * PLT_FS + 29761u) / (2u * 29761u))   /* Dattorro's 29761 Hz lengths, at PLT_FS (rounded) */
#define PLT_MOD_MAX 40                       /* the modulated all-passes' swing, +- samples */
#define PLT_PRE_MAX (PLT_FS / 10)            /* 100 ms */
/* the lines, Dattorro's numbering: 0..3 the input all-passes; 4 / 8 the modulated all-passes (+ room for the swing);
 * 5 / 9 the long delays; 6 / 10 the decay all-passes; 7 / 11 the second delays; 12 the pre-delay */
static const uint16_t PLT_LEN[13] = {
    PLT_SC(142), PLT_SC(107), PLT_SC(379), PLT_SC(277),
    PLT_SC(672), PLT_SC(4453), PLT_SC(1800), PLT_SC(3720),
    PLT_SC(908), PLT_SC(4217), PLT_SC(2656), PLT_SC(3163),
    0,
};
#define PLT_IN_LOW (PLT_SC(142) + PLT_SC(107) + PLT_SC(379) + PLT_SC(277) + 8 + PLT_SC(672) + 2 * PLT_MOD_MAX + 4 + \
                    PLT_SC(4453) + 2 + PLT_SC(908) + 2 * PLT_MOD_MAX + 4 + PLT_SC(4217) + 2)
#define PLT_EXT (PLT_SC(1800) + PLT_SC(3720) + PLT_SC(2656) + PLT_SC(3163) + 8 + PLT_PRE_MAX + 2)
_Static_assert(PLT_IN_LOW <= sizeof rev_line / sizeof rev_line[0], "PLATE: its first lines fit in ROOM's rev_line");
static int16_t plt_ext[PLT_EXT];
/* the taps (Dattorro's table, at 29761 Hz): the line, its offset back, its sign; left then right */
static const struct { uint8_t line; int8_t sign; uint16_t at; } PLT_TAP[14] = {
    {9, 1, 266}, {9, 1, 2974}, {10, -1, 1913}, {11, 1, 1996}, {5, -1, 1990}, {6, -1, 187}, {7, -1, 1066},
    {5, 1, 353}, {5, 1, 3627}, {6, -1, 1228}, {7, 1, 2673}, {9, -1, 2111}, {10, -1, 335}, {11, -1, 121},
};

typedef struct { int16_t *b; uint16_t cap, w; } pl_line_t;
static struct {
    pl_line_t ln[13];
    uint16_t len[13];                        /* each line's length now (SIZE) */
    uint16_t tap[14];
    int32_t size;                            /* SIZE now, Q15 (glides to the knob) */
    int32_t bw, lp[2], hp[2], tail[2];       /* the band limit; each half's damping, high-pass (Q8 more), output */
    int32_t last_l, last_r;                  /* the previous 22 kHz output (the interpolation) */
    uint32_t ph[2], rnd;
    int32_t drift, drift_to;                 /* SPACE's slow random swing, Q8 of -32..31 */
    int32_t modg;                            /* the modulation's share, Q15: glides to 0 under FREEZE (an exact loop:
                                              * a moving read interpolates, a slow low-pass the held tail would fade by) */
    uint8_t type, inited, blk;
} plt;

static inline int32_t plt_mul(int32_t a, int32_t b) { return (a * b + 16384) >> 15; }   /* rounded: FREEZE holds */
static inline int32_t plt_rd(const pl_line_t *l, uint32_t back)
{
    int32_t i = (int32_t)l->w - (int32_t)back;
    return l->b[i < 0 ? i + l->cap : i];
}
static inline void plt_wr(pl_line_t *l, int32_t v)
{
    l->b[l->w] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
    if (++l->w >= l->cap)
        l->w = 0;
}
static inline int32_t plt_rdq(const pl_line_t *l, int32_t back_q8)        /* fractional, Q8 */
{
    uint32_t b = (uint32_t)back_q8 >> 8;
    int32_t a = plt_rd(l, b), c = plt_rd(l, b + 1u);
    return a + (((c - a) * (back_q8 & 255)) >> 8);
}
/* an all-pass of length len: v = x - g z, y = z + g v */
static inline int32_t plt_ap(pl_line_t *l, uint32_t len, int32_t x, int32_t g)
{
    int32_t z = plt_rd(l, len), v = x - plt_mul(z, g);
    plt_wr(l, v);
    return z + plt_mul(v, g);
}

static void plt_init(void)
{
    uint32_t i;
    int16_t *lo = rev_line, *ex = plt_ext;
    static const uint8_t IN_LOW[7] = {0, 1, 2, 3, 4, 5, 8};
    for (i = 0; i < 13u; i++) {
        uint32_t cap = i == 12u ? PLT_PRE_MAX + 2u : PLT_LEN[i] + ((i == 4u || i == 8u) ? 2u * PLT_MOD_MAX + 4u : 2u);
        uint32_t k, low = 0;
        for (k = 0; k < 7u; k++)
            low |= IN_LOW[k] == i;
        if (i == 9u)
            low = 1;
        plt.ln[i].b = low ? lo : ex;
        plt.ln[i].cap = (uint16_t)cap;
        plt.ln[i].w = 0;
        if (low)
            lo += cap;
        else
            ex += cap;
    }
    memset(rev_line, 0, sizeof rev_line);
    memset(plt_ext, 0, sizeof plt_ext);
    memset(plt.len, 0, sizeof plt.len);
    plt.bw = plt.lp[0] = plt.lp[1] = plt.hp[0] = plt.hp[1] = plt.tail[0] = plt.tail[1] = 0;
    plt.last_l = plt.last_r = 0;
    plt.size = -1;
    plt.modg = 32768;
    plt.inited = 1;
}

/* FREEZE begins (punch.c): every line at half (the held loop has room: no clip), as ROOM's */
static void plt_freeze_halve(void)
{
    uint32_t i;
    for (i = 0; i < sizeof rev_line / sizeof rev_line[0]; i++)
        rev_line[i] = (int16_t)HALF0(rev_line[i]);
    for (i = 0; i < PLT_EXT; i++)
        plt_ext[i] = (int16_t)HALF0(plt_ext[i]);
    plt.lp[0] = HALF0(plt.lp[0]), plt.lp[1] = HALF0(plt.lp[1]);
    plt.tail[0] = HALF0(plt.tail[0]), plt.tail[1] = HALF0(plt.tail[1]);
}

/* one block (n even, CTL) of PLATE (type 1) or SPACE (2): the mono send in, the stereo return out */
static void plt_block(const int32_t *in, int32_t *out_l, int32_t *out_r, uint32_t n, uint32_t type)
{
    const int space = type == 2u, frz = rev_frz != 0;
    int32_t v = song.g[G_RSIZE], decay, dd2, damp, mod, size_to, pre, m0[2], m1[2], dm[2], gain;
    uint32_t i, k;
    if (!plt.inited || plt.type != type) {
        plt_init();
        plt.type = (uint8_t)type;
    }
    /* DECAY (an easing curve: finer at the long end) */
    v = v * (254 - v);                                  /* 0 .. 16129 */
    decay = space ? 14746 + (int32_t)((17859LL * v) / 16129) : 9830 + (int32_t)((20644LL * v) / 16129);
    dd2 = decay + 4915;                                 /* Dattorro: decay + 0.15, 0.25 .. 0.5 */
    dd2 = dd2 < 8192 ? 8192 : dd2 > 16384 ? 16384 : dd2;
    damp = 32767 - song.g[G_RDAMP] * 230;               /* the tank's low-pass: 1 (open) .. ~0.1 */
    if (space)
        damp = damp * 7 / 8;                            /* (SPACE: darker, as a big room) */
    if (frz)
        decay = 32768, damp = 32767;
    plt.modg += frz ? (plt.modg > 256 ? -256 : -plt.modg) : (plt.modg < 32768 - 256 ? 256 : 32768 - plt.modg);   /* (~90 ms) */
    mod = (int32_t)(((int64_t)song.g[G_RMOD] * (space ? PLT_MOD_MAX * 256 : 8 * 256) / 127 * plt.modg) >> 15);   /* Q8 */
    size_to = space ? 3932 + song.g[G_RSZ] * 227 : 8192 + song.g[G_RSZ] * 141;   /* Q15: 0.12 .. 1 / 0.25 .. 0.8 */
    pre = song.g[G_RPRE] * PLT_PRE_MAX / 100;           /* ms -> samples */
    if (pre < 1)
        pre = 1;
    /* SIZE: glides; the lengths and taps follow, one sample a block at most each */
    if (plt.size < 0)
        plt.size = size_to;
    plt.size += (size_to - plt.size) >> 3;
    for (k = 0; k < 12u; k++) {
        uint32_t want = k < 4u ? PLT_LEN[k] : (PLT_LEN[k] * (uint32_t)plt.size) >> 15;
        if (want < 4u) want = 4u;
        if (plt.len[k] == 0 || plt.len[k] > plt.ln[k].cap - 2u)
            plt.len[k] = (uint16_t)want;
        else if (want > plt.len[k])
            plt.len[k]++;
        else if (want < plt.len[k])
            plt.len[k]--;
    }
    for (k = 0; k < 14u; k++) {
        uint32_t t = ((uint32_t)PLT_SC(PLT_TAP[k].at) * (uint32_t)plt.size) >> 15, L = plt.len[PLT_TAP[k].line];
        plt.tap[k] = (uint16_t)(t >= L ? L - 1u : t < 1u ? 1u : t);
    }
    /* the modulation: two sines (PLATE: one, a quarter turn apart), + SPACE's drift; Q8 samples, at both block ends */
    for (k = 0; k < 2u; k++)                            /* (3/4 the sine, 1/4 the drift: within +-mod) */
        m0[k] = (int32_t)(((int64_t)osc_sine(plt.ph[k]) * mod * 3) >> 17) + (space ? (plt.drift * mod) >> 15 : 0);
    plt.ph[0] += (uint32_t)((space ? 0.5 : 0.9) * 4294967296.0 * CTL / FS);
    plt.ph[1] = space ? plt.ph[1] + (uint32_t)(0.63 * 4294967296.0 * CTL / FS) : plt.ph[0] + 0x40000000u;
    if (space && !plt.blk--) {
        plt.blk = 200;
        plt.rnd = plt.rnd * 1664525u + 1013904223u;
        plt.drift_to = (int32_t)(plt.rnd >> 26) - 32;     /* -32 .. 31 */
    }
    plt.drift += ((plt.drift_to << 8) - plt.drift) >> 7;   /* (~0.1 s) */
    for (k = 0; k < 2u; k++) {
        m1[k] = (int32_t)(((int64_t)osc_sine(plt.ph[k]) * mod * 3) >> 17) + (space ? (plt.drift * mod) >> 15 : 0);
        dm[k] = (m1[k] - m0[k]) / (int32_t)(n / 2u);
    }
    gain = space ? 20500 : 27000;                       /* (the first 0.5 s of the return as loud as ROOM's, the defaults) */
    for (i = 0; i < n; i += 2u) {
        int32_t x, y, l, r, t0, t1, j = (int32_t)(i >> 1);
        /* in: a pair averaged, the band limit, the pre-delay, the four input all-passes */
        x = (in[i] + in[i + 1u]) >> 1;
        x = mulq15(x < -98304 ? -98304 : x > 98304 ? 98304 : x, 8000);
        plt.bw += plt_mul(x - plt.bw, space ? 22000 : 29000);
        plt_wr(&plt.ln[12], plt.bw);
        x = plt_rd(&plt.ln[12], (uint32_t)pre);
        x = plt_ap(&plt.ln[0], plt.len[0], x, 24576);   /* 0.75 */
        x = plt_ap(&plt.ln[1], plt.len[1], x, 24576);
        x = plt_ap(&plt.ln[2], plt.len[2], x, 20480);   /* 0.625 */
        x = plt_ap(&plt.ln[3], plt.len[3], x, 20480);
        /* the tank: each half takes the other's tail of the last sample */
        t0 = plt.tail[1], t1 = plt.tail[0];
        for (k = 0; k < 2u; k++) {
            uint32_t b = k ? 8u : 4u;                   /* 4..7 the left half, 8..11 the right */
            int32_t z, vv, back = ((int32_t)plt.len[b] << 8) + m0[k] + dm[k] * j + (PLT_MOD_MAX << 8);
            y = x + plt_mul(k ? t1 : t0, decay);
            /* the modulated all-pass (Dattorro's -0.7: the sign as his) */
            z = plt_rdq(&plt.ln[b], back < (2 << 8) ? 2 << 8 : back);
            vv = y + plt_mul(z, 22938);
            plt_wr(&plt.ln[b], vv);
            y = z - plt_mul(vv, 22938);
            plt_wr(&plt.ln[b + 1u], y);                 /* the long delay */
            y = plt_rd(&plt.ln[b + 1u], plt.len[b + 1u]);
            if (!frz) {
                plt.lp[k] += plt_mul(y - plt.lp[k], damp);
                plt.hp[k] += ((plt.lp[k] << 8) - plt.hp[k]) >> 8;   /* (~14 Hz) */
                y = plt.lp[k] - (plt.hp[k] >> 8);
            }
            y = plt_mul(y, decay);
            y = plt_ap(&plt.ln[b + 2u], plt.len[b + 2u], y, dd2);   /* decay diffusion 2 */
            plt_wr(&plt.ln[b + 3u], y);
            plt.tail[k] = plt_rd(&plt.ln[b + 3u], plt.len[b + 3u]);
        }
        /* the taps */
        l = r = 0;
        for (k = 0; k < 7u; k++) {
            int32_t a = plt_rd(&plt.ln[PLT_TAP[k].line], plt.tap[k]), c = plt_rd(&plt.ln[PLT_TAP[k + 7u].line], plt.tap[k + 7u]);
            l += PLT_TAP[k].sign > 0 ? a : -a;
            r += PLT_TAP[k + 7u].sign > 0 ? c : -c;
        }
        l = mulq15(l, gain), r = mulq15(r, gain);
        out_l[i] = (plt.last_l + l) >> 1, out_r[i] = (plt.last_r + r) >> 1;   /* back to 44.1 kHz */
        out_l[i + 1u] = l, out_r[i + 1u] = r;
        plt.last_l = l, plt.last_r = r;
    }
}

/* SPDX-License-Identifier: GPL-3.0-only */
/* SNOOP: STRETCH, a time-stretching granular engine in the spirit of Paulstretch (within the FM-1's means: in the
 * time domain, no FFT).
 *
 * A playhead moves through the source SPED times slower than it was recorded (FREEZ: not at all, up to 1x), from POS;
 * at its end it starts again from POS. Around the playhead, long grains (SIZE: 40 ms .. 2 s, Hann windows) overlap
 * DENS deep (1..8), each read at the note's pitch (PTCH, and the key against the zone's root) from a spot SPRD
 * scatters around the playhead (up to a whole grain), each a few cents off (SPRD too, up to +-12 cents): the
 * spectrum stays, the time smears out into a cloud. TONE: a low-pass, as GRAIN's.
 *
 * Sources (SRC): the SAMPLE sets and the user slots USR1..USR4 (as GRAIN, through the same ADPCM seek index: a grain
 * starts at its exact sample), and LIVE: LIV1..LIV3 record synth track 1..3, LIVD the drums, the last 1.49 s at
 * 22.05 kHz, into the upper half of the delay's buffer (fx.c live_*: while a part plays a LIVE source the delay has
 * the lower half, 0.74 s). A note holds the recording: the ring stops while a STRETCH voice of that part sounds, so
 * what you stretch is what was played just before (POS: 0 the oldest, 127 the newest), never the stretch itself; once
 * the voices end it records again. A LIVE source plays the key against C4.
 *
 * State: the part's engine arena (engines.c), the grains and the seek index; per voice s[0] the zone (-1 none), s[1]
 * the countdown to the next grain, s[2] TONE's low-pass, s[3] the playhead (Q12 source samples). The cost is bounded
 * as GRAIN's: at most ST_NG grains a part, a decode load cap (ADPCM), one grain start per voice and block. */
#define ST_POLY 3
#define ST_NG 24
#define ST_LIVE_SRC SMP_NALL                 /* SRC >= this: LIVE (track 1..3, drums) */
#define ST_NSRC (SMP_NALL + 4u)
#define LIVE_LEN 32768u                      /* the ring: 1.49 s at 22.05 kHz (the delay buffer's upper half) */
#define ST_LOAD_MAX (8u << 16)               /* ADPCM decodes per output sample, all the part's grains */

/* fx.c: the ring (the delay buffer's upper half) */
static int16_t *live_buf(void);
static volatile uint8_t live_want[NPART];    /* this block: the part plays a LIVE source (its track + 1), 0 none */
static volatile uint8_t live_hold[NPART];    /* this block: a voice of it sounds (the ring holds still) */
static uint32_t live_w;                      /* the ring's write index (fx.c; the oldest sample while it holds) */
static uint8_t live_on;                      /* fx.c: the ring is recording / held (the delay on its lower half) */
static int32_t live_cap[CTL];                /* the recorded track's block (fx.c mix_part_run, drums.c drums_mix) */
static volatile uint8_t live_cap_src;        /* that track + 1 (4: the drums), 0 none: set for the next block */

static const char *const ST_SRC_NAMES[ST_NSRC] = {SMP_SET_NAMES_INIT, "USR1", "USR2", "USR3", "USR4",
                                                  "LIV1", "LIV2", "LIV3", "LIVD"};
/* SPED: how many times slower than recorded */
static const char *const ST_SPEED_NAMES[] = {"FREEZ", "1000x", "500x", "250x", "100x", "50x", "25x", "16x",
                                             "10x", "8x", "6x", "4x", "3x", "2x", "1.5x", "1x"};
static const uint16_t ST_SPEED_DIV[16] = {0, 16000, 8000, 4000, 1600, 800, 400, 256, 160, 128, 96, 64, 48, 32, 24, 16};   /* x16 */

typedef struct {
    uint32_t pos;                /* the next sample to read (ADPCM: to decode) */
    int32_t frac;                /* Q16 between a and b */
    uint32_t step;               /* Q16 source samples per output sample */
    uint32_t wph, winc, left;    /* the window's phase and step; output samples to go */
    int32_t a, b, pred, idx;
    int32_t gain;
    const smp_zone_t *z;         /* 0: LIVE */
    uint32_t base;               /* LIVE: the ring's oldest sample when it held */
    uint8_t owner;               /* voice + 1, 0 free */
} st_grain_t;

typedef struct {
    st_grain_t g[ST_NG];
    int16_t ipred[GR_NIDX];      /* the seek index, as GRAIN's (eng_grain.c) */
    uint8_t iidx[GR_NIDX];
    uint16_t zbase[GR_MAXZ], zcnt[GR_MAXZ], zdone[GR_MAXZ];
    uint32_t stamp;
    int32_t rng;
    uint8_t src, nz;
} st_part_t;

static const smp_zone_t ST_LIVE_ZONE = {0, LIVE_LEN, LIVE_LEN, LIVE_LEN, 32768, 60 * 16, 0, 0, 0, 127, 0};

static void st_reset(st_part_t *P, uint32_t src, uint32_t stamp)
{
    uint32_t i, tot = 0, nz = src < SMP_NALL ? gr_nz(src) : 0u;
    P->src = (uint8_t)(src + 1u);
    P->stamp = stamp;
    P->nz = (uint8_t)(nz < GR_MAXZ ? nz : GR_MAXZ);
    for (i = 0; i < ST_NG; i++)
        P->g[i].owner = 0;
    for (i = 0; i < P->nz; i++) {
        uint32_t c = (gr_zone(src, i)->n + GR_SEG - 1u) >> GR_SEG_LOG2;
        if (tot + c > GR_NIDX)
            c = 0;
        P->zbase[i] = (uint16_t)tot;
        P->zcnt[i] = (uint16_t)c;
        P->zdone[i] = c ? 1u : 0u;
        if (c)
            P->ipred[tot] = 0, P->iidx[tot] = 0;
        tot += c;
    }
}
static void st_build(st_part_t *P, const track_t *t)   /* one index entry: a sounding voice's zone first */
{
    uint32_t i, zl = GR_MAXZ, k, e, pos;
    int32_t pred, idx;
    const smp_zone_t *z;
    for (i = 0; i < ST_POLY && zl == GR_MAXZ; i++) {
        int32_t vz = t->v[i].s[0];
        if (t->v[i].active && vz >= 0 && vz < P->nz && P->zdone[vz] < P->zcnt[vz])
            zl = (uint32_t)vz;
    }
    for (i = 0; i < P->nz && zl == GR_MAXZ; i++)
        if (P->zdone[i] < P->zcnt[i])
            zl = i;
    if (zl == GR_MAXZ)
        return;
    z = gr_zone(P->src - 1u, zl);
    k = P->zdone[zl];
    e = P->zbase[zl] + k - 1u;
    pred = P->ipred[e], idx = P->iidx[e];
    pos = (k - 1u) << GR_SEG_LOG2;
    for (i = 0; i < GR_SEG; i++)
        gr_dec(z, pos + i, &pred, &idx);
    P->ipred[e + 1u] = (int16_t)pred;
    P->iidx[e + 1u] = (uint8_t)idx;
    P->zdone[zl] = (uint16_t)(k + 1u);
}

static uint32_t st_len(const int16_t *p)        /* SIZE: 40 ms .. 2 s, exponential (output samples) */
{
    return ((pow2_q16(p[P_E3] * 17 / 2) >> 8) * 1764u) >> 8;
}

/* a grain of voice vi at the playhead */
static void st_spawn(st_part_t *P, const track_t *t, voice_t *v, uint32_t vi, uint32_t len, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t src = (uint32_t)p[P_E0] % ST_NSRC, live = src >= ST_LIVE_SRC, zl = (uint32_t)v->s[0], i, n, step, span, c;
    const smp_zone_t *z = live ? &ST_LIVE_ZONE : gr_zone(src, zl);
    st_grain_t *g = 0;
    uint32_t r1 = noise32(&P->rng), r2 = noise32(&P->rng);
    int32_t d16, jit, pos, head = (int32_t)((uint32_t)v->s[3] >> 12);
    for (i = 0; i < ST_NG && !g; i++)
        if (!P->g[i].owner)
            g = &P->g[i];
    n = z->n;
    if (!g || n < 64u || (!live && !P->zcnt[zl]))
        return;
    /* pitch: the key against the root, PTCH, a few cents of scatter (SPRD: up to +-2/16 semitone) */
    d16 = m->pitch16 - z->root16 + p[P_E6] * 16 + ((((int32_t)(r1 & 0xFFFFu) - 32768) * p[P_E5] / 127 * 2) >> 15);
    step = (pow2_q16(clamp(d16, -1536, 576)) >> 8) * (z->rate >> 8);
    step = step > GR_STEP_MAX ? GR_STEP_MAX : step < 256u ? 256u : step;
    span = (len * (step >> 8)) >> 8;                    /* source samples it reads */
    if (span + 3u > n) {
        span = n - 3u;
        len = (span << 8) / (step >> 8);
    }
    if (len < 64u)
        return;
    /* where: the playhead, scattered by SPRD over up to a whole grain */
    jit = (int32_t)gr_scale(span, (uint32_t)p[P_E5] * 516u);
    pos = head - (int32_t)(span >> 1) + (int32_t)gr_scale((uint32_t)jit, r2 & 0xFFFFu) - (jit >> 1);
    pos = clamp(pos, 0, (int32_t)(n - 3u - span));
    if (!live && (uint32_t)pos >> GR_SEG_LOG2 >= P->zdone[zl])
        return;                                     /* not indexed yet */
    if (!live) {                                    /* the decode load: a cap on the cost */
        for (i = 0, c = step; i < ST_NG; i++)
            if (P->g[i].owner && P->g[i].z)
                c += P->g[i].step;
        if (c > ST_LOAD_MAX)
            return;
    }
    g->step = step;
    g->frac = 0;
    g->wph = 0;
    g->winc = 0xFFFFFFFFu / len;
    g->left = len;
    c = (uint32_t)clamp(p[P_E4], 1, 8);
    g->gain = GR_NORM[c];
    g->owner = (uint8_t)(vi + 1u);
    if (live) {
        int16_t *rb = live_buf();
        g->z = 0;
        g->base = live_w;
        g->a = rb[(g->base + (uint32_t)pos) & (LIVE_LEN - 1u)];
        g->b = rb[(g->base + (uint32_t)pos + 1u) & (LIVE_LEN - 1u)];
        g->pos = (uint32_t)pos + 2u;
    } else {
        g->z = z;
        {
            uint32_t e = P->zbase[zl] + ((uint32_t)pos >> GR_SEG_LOG2), q;
            g->pred = P->ipred[e], g->idx = P->iidx[e];
            for (q = (uint32_t)pos & ~(GR_SEG - 1u); q < (uint32_t)pos; q++)
                gr_dec(z, q, &g->pred, &g->idx);
        }
        g->a = gr_dec(z, (uint32_t)pos, &g->pred, &g->idx);
        g->b = gr_dec(z, (uint32_t)pos + 1u, &g->pred, &g->idx);
        g->pos = (uint32_t)pos + 2u;
    }
}

/* one grain into acc; 0 when it ended. The window: a ramp over the block between its exact ends (as GRAIN's) */
static int st_run(st_grain_t *g, int32_t *acc, uint32_t n)
{
    uint32_t i, m = n < g->left ? n : g->left, step = g->step, pos = g->pos;
    int32_t a = g->a, b = g->b, frac = g->frac, w0, w1, wq, dq;
    w0 = mulq15(gr_win(g->wph), g->gain);
    g->wph += g->winc * m;
    g->left -= m;
    w1 = g->left ? mulq15(gr_win(g->wph), g->gain) : 0;
    wq = w0 * 1024;
    dq = m == CTL ? ((w1 - w0) * 1024) >> CTL_LOG2 : m ? (w1 - w0) * 1024 / (int32_t)m : 0;
    if (g->z) {
        const smp_zone_t *z = g->z;
        int32_t pred = g->pred, idx = g->idx;
        for (i = 0; i < m; i++) {
            int32_t s = a + (((b - a) * (frac >> 1)) >> 15);
            acc[i] += (s * (wq >> 10)) >> 15;
            wq += dq;
            frac += (int32_t)step;
            while (frac >= 65536) {
                frac -= 65536;
                a = b;
                b = pos < z->n ? gr_dec(z, pos++, &pred, &idx) : a;
            }
        }
        g->pred = pred, g->idx = idx;
    } else {
        const int16_t *rb = live_buf();
        for (i = 0; i < m; i++) {
            int32_t s = a + (((b - a) * (frac >> 1)) >> 15);
            acc[i] += (s * (wq >> 10)) >> 15;
            wq += dq;
            frac += (int32_t)step;
            while (frac >= 65536) {
                frac -= 65536;
                a = b;
                b = pos < LIVE_LEN ? rb[(g->base + pos++) & (LIVE_LEN - 1u)] : a;
            }
        }
    }
    g->pos = pos, g->a = a, g->b = b, g->frac = frac;
    if (!g->left)
        g->owner = 0;
    return g->left != 0;
}

static void stretch_note_on(track_t *t, voice_t *v)
{
    st_part_t *P = (st_part_t *)eng_arena_of(t, ENGI_STRETCH);
    uint32_t vi = (uint32_t)(v - t->v) % NVOICE, i, src = (uint32_t)t->p[P_E0] % ST_NSRC;
    uint32_t n;
    v->s[0] = src >= ST_LIVE_SRC ? 0 : gr_find(src, v->note);
    n = src >= ST_LIVE_SRC ? LIVE_LEN : v->s[0] >= 0 ? gr_zone(src, (uint32_t)v->s[0])->n : 0u;
    v->s[3] = (int32_t)(gr_scale(n, (uint32_t)t->p[P_E1] * 516u) << 12);   /* the playhead from POS */
    v->s[1] = 0;                                    /* the first grain at once */
    if (!v->env) {
        for (i = 0; i < ST_NG; i++)
            if (P->g[i].owner == vi + 1u)
                P->g[i].owner = 0;
        v->s[2] = 0;
    }
}

/* per part and block: the index follows SRC; LIVE: the part asks fx.c for the ring and holds it while it sounds */
static void stretch_block(track_t *t)
{
    st_part_t *P = (st_part_t *)eng_arena_of(t, ENGI_STRETCH);
    uint32_t src = (uint32_t)t->p[P_E0] % ST_NSRC, st = src < SMP_NALL ? gr_stamp(src) : 0u, i, pi = (uint32_t)(t - trk) % NPART;
    if (!P->rng)
        P->rng = 0x1F2E3D4C;
    if (P->src != src + 1u || P->stamp != st) {
        st_reset(P, src, st);
        for (i = 0; i < NVOICE; i++)
            if (t->v[i].active)
                t->v[i].s[0] = src >= ST_LIVE_SRC ? 0 : gr_find(src, t->v[i].note);
    }
    for (i = 0; i < ST_NG; i++)
        if (P->g[i].owner && !t->v[(P->g[i].owner - 1u) % NVOICE].active)
            P->g[i].owner = 0;
    if (src >= ST_LIVE_SRC) {
        uint32_t any = 0;
        for (i = 0; i < NVOICE; i++)
            any |= t->v[i].active;
        live_want[pi] = (uint8_t)(src - ST_LIVE_SRC + 1u);
        live_hold[pi] = (uint8_t)any;
    } else {
        st_build(P, t);
    }
}

static void stretch_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    st_part_t *P = (st_part_t *)eng_arena_of(t, ENGI_STRETCH);
    const int16_t *p = t->p;
    uint32_t vi = (uint32_t)(v - t->v) % NVOICE, i, mine = 0, nact = 0, src = (uint32_t)p[P_E0] % ST_NSRC, live = src >= ST_LIVE_SRC;
    uint32_t len = st_len(p), dens = (uint32_t)clamp(p[P_E4], 1, 8), iv, zn, sp;
    int32_t zl = v->s[0], acc[CTL], lp, y = v->s[2];
    const smp_zone_t *z;
    if (n > CTL)
        n = CTL;
    if (zl < 0 || P->src != src + 1u || (!live && zl >= P->nz) || (live && !live_on)) {
        if (zl < 0)
            v->active = 0;
        return;
    }
    z = live ? &ST_LIVE_ZONE : gr_zone(src, (uint32_t)zl);
    zn = z->n;
    for (i = 0; i < n; i++)
        acc[i] = 0;
    for (i = 0; i < ST_POLY; i++)
        nact += t->v[i].active;
    for (i = 0; i < ST_NG; i++)
        if (P->g[i].owner == vi + 1u)
            mine += st_run(&P->g[i], acc, n);
    /* the playhead: SPED times slower than the source (Q12 source samples); from POS again at the end */
    sp = ST_SPEED_DIV[(uint32_t)p[P_E2] & 15u];
    if (sp && zn > 64u) {
        uint32_t head = (uint32_t)v->s[3] + (uint32_t)(((uint64_t)z->rate * n << 12 >> 16) * 16u / sp);
        uint32_t end = (zn - 32u) << 12;
        if (head >= end)
            head = gr_scale(zn, (uint32_t)p[P_E1] * 516u) << 12;
        v->s[3] = (int32_t)head;
    }
    /* the next grain: DENS per grain length */
    iv = len / dens;
    v->s[1] -= (int32_t)n;
    if (v->s[1] <= 0) {
        v->s[1] += (int32_t)iv;
        if (v->s[1] < 0)
            v->s[1] = 0;
        if (mine < (nact > 1u ? ST_NG / nact : ST_NG))
            st_spawn(P, t, v, vi, len, m);
    }
    lp = 4000 + ((clamp((p[P_E7] << 8) + m->cutoff, 0, 127 << 8) * 28767) >> 15);
    for (i = 0; i < n; i++) {
        y += mulq15(clamp(clamp(acc[i], -65535, 65535) - y, -65535, 65535), lp);
        out[i] += mulq15(mulq15(y, amp_at(m, i)), VOICE_FS) << 1;
    }
    v->s[2] = y;
}

static const preset_t STRETCH_PRESETS[] = {
    /* name, {SRC, POS, SPED, SIZE, DENS, SPRD, PTCH, TONE}, {A D S R}, fenv, mono, sends. SRC: the SAMPLE sets (0 PIANO,
     * 2 VIBES, 3 HORNS, 4 STRGS, 5 FLUTE), USR1.. (SMP_NSETS), LIVE (ST_LIVE_SRC: track 1, 2, 3, the drums) */
    {"PNO STRETCH", {0, 16, 5, 100, 6, 30, 0, 100}, {80, 100, 127, 100}, 0, 0, FX(0, 30, 10, 90)},
    {"STR ETHER", {4, 8, 6, 110, 6, 40, 0, 90}, {90, 100, 127, 105}, 0, 0, FX(0, 35, 10, 95)},
    {"VIBE MIST", {2, 8, 4, 105, 5, 50, 12, 100}, {80, 100, 127, 105}, 0, 0, FX(0, 40, 15, 95)},
    {"FLUTE AIR", {5, 30, 5, 100, 6, 30, 0, 85}, {90, 100, 127, 100}, 0, 0, FX(0, 30, 10, 90)},
    {"HORN DRONE", {3, 40, 3, 115, 7, 25, -12, 70}, {100, 100, 127, 110}, 0, 0, FX(0, 25, 5, 85)},
    {"PNO FROZEN", {0, 45, 0, 110, 8, 20, 0, 100}, {90, 100, 127, 110}, 0, 0, FX(0, 30, 10, 95)},
    {"USR STRETCH", {SMP_NSETS, 0, 5, 100, 6, 30, 0, 110}, {80, 100, 127, 100}, 0, 0, FX(0, 30, 10, 90)},
    {"LIVE STRETCH", {ST_LIVE_SRC, 100, 5, 100, 6, 30, 0, 110}, {60, 100, 127, 100}, 0, 0, FX(0, 30, 10, 90)},
    {"LIVE FREEZE", {ST_LIVE_SRC + 3, 112, 0, 90, 8, 40, 0, 110}, {40, 100, 127, 100}, 0, 0, FX(0, 30, 10, 90)},
};

static const engine_t ENG_STRETCH = {
    "STRETCH", {"STRCH", "CLOUD"},
    {
        {"SRC", F_ENUM, 0, ST_NSRC - 1, 0, ST_SRC_NAMES, 0},
        {"POS", F_PCT, 0, 127, 16, 0, 0},
        {"SPED", F_ENUM, 0, 15, 5, ST_SPEED_NAMES, 0},
        {"SIZE", F_PCT, 0, 127, 100, 0, 0},
        {"DENS", F_INT, 1, 8, 6, 0, 0},
        {"SPRD", F_PCT, 0, 127, 30, 0, 0},
        {"PTCH", F_SEMI, -24, 24, 0, 0, 0},
        {"TONE", F_PCT, 0, 127, 110, 0, 0},
    },
    STRETCH_PRESETS, sizeof(STRETCH_PRESETS) / sizeof(STRETCH_PRESETS[0]), 1, stretch_note_on, stretch_render,
    0x5D7F, {P_E1, P_E2, P_E3, P_E5}, ST_POLY, 0, 0, stretch_block,
};

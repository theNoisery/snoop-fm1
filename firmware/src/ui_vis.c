/* SPDX-License-Identifier: GPL-3.0-only */
/* SLOOP 2.4: the full-screen visualiser. On HOME (the TRACKS screen), HOME tapped opens it; HOME again or
 * any page closes it; SELECT steps through the styles (the name shows a second); a layer held shows its
 * screen as ever, then the visualiser comes back. The keys, PLAY and REC work as always.
 * SNOOP 0.9: redrawn in the manner of the OP-1's screens: thin anti-aliased lines, a thin vector font, a
 * dark ground, four colours (the tracks: blue, green, white, red), nine styles:
 *   1 SCOPE      the master's wave between two poles, a heart monitor (the frame before in maroon), the tempo
 *   2 SPECTRUM   a staircase (green, the slow one behind in teal), the two highest peaks marked, the last note
 *   3 LISSAJOUS  L-R across, L+R up, violet
 *   4 METERS     the four tracks and the master
 *   5 HEXAGON    a hexagon growing with the master, turning a side a bar; a ball for each track playing,
 *                bounced by its notes
 *   6 TAPE       two reels (the tape runs while playing), the head, the position, the tracks' patterns below
 *   7 TRANSPORT  the tempo large; the bar and beat, the bar's sixteenths, the swing, playing / recording
 *   8 GEOMETRY   four figures (one a track): their lines multiply with the track's level
 *   9 TRACKS     each track's level (the needle: what it plays, the number: its LEVEL) and pan
 * It reads what the audio already leaves for the UI: the scope buffers (audio.c, the mix at 22 kHz, left and
 * right, as if MASTER were all the way up), the tracks' peaks (fx.c / drums.c, as the TRACKS meters read them),
 * the notes that started (voice.c vis_hit) and the transport clock. Everything is drawn in the main loop, every
 * other frame, in two bands of 120 rows (the canvas holds 124): no cost to the audio. */
/* VIS_N, vis_style: panel.c (kept with the settings); vis_on, vis_shown_last, vis_name_t: ui_draw.c */
static const char *const VIS_NAME[VIS_N] = {"SCOPE", "SPECTRUM", "LISSAJOUS", "METERS", "HEXAGON",
                                            "TAPE", "TRANSPORT", "GEOMETRY", "TRACKS"};
#define VIS_FFT 512u
#define VIS_NB 52u                                  /* the spectrum's bands (4 px each) */
static int16_t vis_re[VIS_FFT] __attribute__((section(".pool"))), vis_im[VIS_FFT] __attribute__((section(".pool")));
#include "scratch.h"                                /* sloop-lab: this frame's scope snapshot, in the shared scratch */
#define vis_l (main_scratch.vis[0])
#define vis_r (main_scratch.vis[1])
_Static_assert(sizeof main_scratch.vis[0] == VIS_FFT * sizeof(int16_t), "the scratch holds the snapshot");
static uint8_t vis_lj[3][256][2];                   /* Lissajous: the last three frames' points */
static struct {
    int32_t scale, ljscale, peak;                   /* auto-scales (smoothed peaks), this frame's peak */
    int16_t sb[VIS_NB], ss[VIS_NB];                 /* spectrum: the bands (fast), the slow line behind (0..1000) */
    int16_t vu[5], pk[5], pkt[5];                   /* METERS: tracks + master, peak holds */
    int16_t lvl[5];                                 /* the tracks' levels (0..1000) this frame, the master */
    int16_t sm[5];                                  /* the same, smoothed (fast up, slow down) */
    int16_t prev[208];                              /* SCOPE: the frame before's wave (y, 1/16 px) */
    int32_t bx[4], by[4], bvx[4], bvy[4];           /* HEXAGON: the balls (1/256 px), their speeds */
    uint8_t alive[4];                               /* .. shown while the track plays (frames left, fading) */
    uint8_t flash[4], last;                         /* frames since a note started (0 = none); the track last */
    uint32_t free_q8, last_ms, rnd;                 /* beats (1/256) when stopped; a random number */
    uint8_t lj_n, prev_ok;
    uint32_t bounces;                               /* HEXAGON: walls hit (tests/vis_test.c) */
} vs;
/* the spectrum's bands: bins of the 512-point FFT at 22 050 Hz (43 Hz each), 43 Hz .. 10 kHz, log spaced */
static const uint8_t VIS_EDGE[VIS_NB + 1u] = {1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 7, 7,
                                              8, 9, 10, 12, 13, 15, 17, 19, 21, 24, 27, 30, 34, 38, 43, 48, 54, 61,
                                              69, 77, 87, 98, 110, 124, 139, 157, 176, 198, 223, 232};

/* the OP-1's colours */
#define OP_BG RGB(0, 0, 0)                  /* (0.9.1: black, as the other screens: a grey looked faded on the FM-1) */
#define OP_BLUE RGB(84, 120, 246)
#define OP_GREEN RGB(22, 222, 140)
#define OP_WHITE RGB(236, 236, 242)
#define OP_RED RGB(246, 52, 84)
#define OP_LAV RGB(184, 184, 236)
#define OP_TEAL RGB(30, 98, 112)
#define OP_GREY RGB(124, 124, 140)
#define OP_DIM RGB(72, 72, 88)
#define OP_DARK RGB(44, 44, 54)
#define OP_MAROON RGB(140, 32, 56)
static const uint16_t OP_TRK[4] = {OP_BLUE, OP_GREEN, OP_WHITE, OP_RED};

static int vis_shown(void) { return vis_on && !ui.home && !ui.menu && cur_page()->scope == SC_TRK; }
static void vis_open(void)
{
    vis_on = 1;
    vis_name_t = 30;
    ui.force = 1;
}
static int32_t isin(uint32_t a) { return SINE[a & 1023u]; }          /* a: 1024 a turn; Q15 */
static int32_t icos(uint32_t a) { return SINE[(a + 256u) & 1023u]; }
static uint16_t mix565(uint16_t a, uint16_t b, int32_t k)              /* a -> b, k 0..256 */
{
    int32_t r = (a >> 11) + ((((b >> 11) - (a >> 11)) * k) >> 8);
    int32_t g = ((a >> 5) & 63) + (((((b >> 5) & 63) - ((a >> 5) & 63)) * k) >> 8);
    int32_t bl = (a & 31) + ((((b & 31) - (a & 31)) * k) >> 8);
    return (uint16_t)(r << 11 | g << 5 | bl);
}
/* |x| (Q15) -> 0..1000 over 48 dB (8 log2: 6 dB a step of 8) */
static int32_t vis_lvl(int32_t a)
{
    int32_t lg = 0, v;
    if (a < 0)
        a = -a;
    if (a < 16)
        return 0;
    while ((a >> lg) > 1)
        lg++;
    v = lg * 8 + (((a << 3) >> lg) & 7);           /* 120 = 0 dB */
    return clamp((v - 56) * 1000 / 64, 0, 1000);
}
static uint32_t vis_isqrt(uint32_t v)
{
    uint32_t r = 0, b = 1u << 30;
    while (b > v)
        b >>= 2;
    while (b) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }
        b >>= 2;
    }
    return r;
}
static uint32_t vis_rand(void)
{
    vs.rnd = vs.rnd * 1664525u + 1013904223u;
    return vs.rnd >> 16;
}

/* ---- drawing: anti-aliased, in 1/256 px (canvas coordinates; cv_oy picks the band) ---- */
static void op_px(int32_t x, int32_t y, uint16_t c, int32_t a)        /* a pixel covered a/256 */
{
    uint16_t *p;
    y += cv_oy;
    if ((uint32_t)x >= cv_w || (uint32_t)y >= cv_h || a <= 0)
        return;
    p = &cv_px[(uint32_t)y * cv_w + (uint32_t)x];
    *p = swap16(a >= 256 ? c : mix565(swap16(*p), c, a));
}
static int op_off_band(int32_t ylo, int32_t yhi)                    /* rows ylo..yhi (1/256 px) all off the band */
{
    return (yhi >> 8) + 2 < -cv_oy || (ylo >> 8) - 2 >= (int32_t)cv_h - cv_oy;
}
/* a line w wide (all 1/256 px): along its long axis a pixel at a time, across it each pixel as much as it is
 * covered */
static void op_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, uint16_t c)
{
    int32_t dx = x1 - x0, dy = y1 - y0, adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy, steep = ady > adx;
    int32_t u0, v0, u1, v1, du, slope, hw, i, i1;
    w = w * 5 / 4;                                  /* (0.9.1: a little bolder, crisper on the FM-1) */
    if (op_off_band(y0 < y1 ? y0 - w : y1 - w, y0 > y1 ? y0 + w : y1 + w))
        return;
    if (steep)
        u0 = y0, v0 = x0, u1 = y1, v1 = x1;
    else
        u0 = x0, v0 = y0, u1 = x1, v1 = y1;
    if (u1 < u0) {
        int32_t t = u0; u0 = u1; u1 = t;
        t = v0; v0 = v1; v1 = t;
    }
    du = u1 - u0;
    if (du < 1)
        du = 1;
    slope = (v1 - v0) * 4096 / du;                  /* |slope| <= 4096: the long axis is u */
    {                                               /* the width across u: w * length / du */
        uint32_t len = (uint32_t)(steep ? ady + adx * 3 / 8 : adx + ady * 3 / 8);   /* (~ the length, in 7 %) */
        hw = (int32_t)((uint32_t)w * (len > (uint32_t)du ? len : (uint32_t)du) / (uint32_t)du) / 2;
    }
    i1 = u1 >> 8;
    for (i = u0 >> 8; i <= i1; i++) {
        int32_t lo_u = i * 256 > u0 ? i * 256 : u0, hi_u = i * 256 + 256 < u1 ? i * 256 + 256 : u1;
        int32_t cu = hi_u - lo_u, uc = i * 256 + 128, v, lo, hi, p;
        if (cu <= 0)
            continue;
        if (steep && (i + cv_oy < 0 || i + cv_oy >= (int32_t)cv_h))
            continue;
        if (uc < u0) uc = u0;
        if (uc > u1) uc = u1;
        v = v0 + (((uc - u0) * slope) >> 12);
        lo = v - hw;
        hi = v + hw;
        for (p = lo >> 8; p <= (hi - 1) >> 8; p++) {
            int32_t a = (hi < p * 256 + 256 ? hi : p * 256 + 256) - (lo > p * 256 ? lo : p * 256);
            a = a * cu >> 8;
            if (steep)
                op_px(p, i, c, a);
            else
                op_px(i, p, c, a);
        }
    }
}
#define PX(v) ((int32_t)(v) * 256)                  /* whole pixels -> 1/256 */
/* a filled disc / a ring (radius r, ring width w; 1/256 px): each pixel by its distance from the centre */
static void op_round(int32_t cx, int32_t cy, int32_t r, int32_t w, uint16_t c)
{
    int32_t x, y, ro = r + (w ? w / 2 : 0) + 256, x0 = (cx - ro) >> 8, x1 = (cx + ro) >> 8, y0 = (cy - ro) >> 8,
                  y1 = (cy + ro) >> 8;
    if (op_off_band(cy - ro, cy + ro))
        return;
    int32_t out2, in2, ri;
    if (y0 < -cv_oy) y0 = -cv_oy;
    if (y1 >= (int32_t)cv_h - cv_oy) y1 = (int32_t)cv_h - cv_oy - 1;
    out2 = (ro >> 2) * (ro >> 2);                   /* (squared, in 1/64 px) */
    ri = (w ? r - w / 2 : r) - 256;
    in2 = ri > 0 ? (ri >> 2) * (ri >> 2) : 0;
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++) {
            int32_t ddx = (x * 256 + 128 - cx) >> 2, ddy = (y * 256 + 128 - cy) >> 2;   /* (1/64 px: no overflow) */
            int32_t d2 = ddx * ddx + ddy * ddy, d, a;
            if (d2 >= out2 || d2 < in2) {           /* wholly outside / inside (no root needed) */
                if (d2 < in2 && !w)
                    op_px(x, y, c, 256);
                continue;
            }
            d = (int32_t)vis_isqrt((uint32_t)d2) << 2;
            if (w) {
                a = w / 2 - (d > r ? d - r : r - d) + 128;
            } else {
                a = r - d + 128;
            }
            op_px(x, y, c, clamp(a, 0, 256));
        }
}
static void op_disc(int32_t cx, int32_t cy, int32_t r, uint16_t c) { op_round(cx, cy, r, 0, c); }
static void vis_ellipse(int32_t cx, int32_t cy, int32_t rx, int32_t ry, uint16_t c)   /* filled, whole pixels */
{
    int32_t y;
    if (rx < 1 || ry < 1)
        return;
    for (y = -ry; y <= ry; y++) {
        int32_t w = 0;
        while (w <= rx && (w * w) * (ry * ry) + (y * y) * (rx * rx) <= (rx * rx) * (ry * ry))
            w++;
        if (w)
            cv_rect(cx - w + 1, cy + y, 2 * w - 1, 1, c);
    }
}
/* an arc (or a circle: a0 .. a0 + 1024) of radius r, as straight pieces: a0, a1 in 1/1024 turn (0 = right,
 * 256 = down) */
static void op_arc(int32_t cx, int32_t cy, int32_t r, int32_t a0, int32_t a1, int32_t w, uint16_t c)
{
    int32_t n = (a1 - a0) * (r >> 8 > 30 ? 64 : r >> 8 > 10 ? 40 : 24) / 1024, k, px, py;
    if (n < 0) n = -n;
    if (n < 2) n = 2;
    if (op_off_band(cy - r - w, cy + r + w))
        return;
    px = cx + ((icos((uint32_t)a0) * (r >> 4)) >> 11);
    py = cy + ((isin((uint32_t)a0) * (r >> 4)) >> 11);
    for (k = 1; k <= n; k++) {
        uint32_t a = (uint32_t)(a0 + (a1 - a0) * k / n);
        int32_t x = cx + ((icos(a) * (r >> 4)) >> 11), y = cy + ((isin(a) * (r >> 4)) >> 11);
        op_line(px, py, x, y, w, c);
        px = x, py = y;
    }
}
static void op_circle(int32_t cx, int32_t cy, int32_t r, int32_t w, uint16_t c)
{
    if (r <= PX(14))
        op_round(cx, cy, r, w, c);                  /* (small: by distance, rounder) */
    else
        op_arc(cx, cy, r, 0, 1024, w, c);
}

/* ---- a thin vector font, after the OP-1's: glyphs drawn on a 12 x 20 grid by strokes:
 *   VM x y        move;           VL x y      a line to;       VN    the pen up
 *   VA cx cy rx ry a0 a1          an arc of an ellipse, from the pen if it is down (a in 1/32 turn, 0 = right,
 *                                 8 = down; a1 < a0: the other way round)
 *   VD x y        a dot;          VE          the end
 * the first number: the advance (the glyph and the gap after it) */
enum { VE = -100, VM = -101, VL = -102, VA = -103, VN = -104, VD = -105 };
#define G_(...) {__VA_ARGS__, VE}
static const int8_t VG_0[] = G_(17, VA, 6, 6, 6, 6, 16, 32, VL, 12, 14, VA, 6, 14, 6, 6, 0, 16, VL, 0, 6);
static const int8_t VG_1[] = G_(17, VM, 2, 4, VL, 7, 0, VL, 7, 20);
static const int8_t VG_2[] = G_(17, VA, 6, 6, 6, 6, 17, 36, VL, 0, 20, VL, 12, 20);
static const int8_t VG_3[] = G_(17, VA, 6, 5, 6, 5, 18, 40, VA, 6, 15, 6, 5, 24, 46);
static const int8_t VG_4[] = G_(17, VM, 9, 20, VL, 9, 0, VL, 0, 14, VL, 12, 14);
static const int8_t VG_5[] = G_(17, VM, 11, 0, VL, 1, 0, VL, 0, 9, VA, 6, 14, 6, 6, 21, 44);
static const int8_t VG_6[] = G_(17, VM, 9, 0, VL, 1, 10, VA, 6, 14, 6, 6, 18, 50);
static const int8_t VG_7[] = G_(17, VM, 0, 0, VL, 12, 0, VL, 3, 20);
static const int8_t VG_8[] = G_(17, VA, 6, 5, 5, 5, 0, 32, VN, VA, 6, 15, 6, 5, 24, 56);
static const int8_t VG_9[] = G_(17, VA, 6, 6, 6, 6, 2, 34, VL, 3, 20);
static const int8_t VG_COLON[] = G_(7, VD, 1, 6, VD, 1, 17);
static const int8_t VG_DOT[] = G_(7, VD, 1, 19);
static const int8_t VG_SLASH[] = G_(15, VM, 10, 0, VL, 0, 20);
static const int8_t VG_PCT[] = G_(17, VA, 3, 4, 3, 4, 0, 32, VN, VM, 12, 0, VL, 0, 20, VN, VA, 9, 16, 3, 4, 0, 32);
static const int8_t VG_MINUS[] = G_(15, VM, 1, 10, VL, 11, 10);
static const int8_t VG_PLUS[] = G_(15, VM, 1, 10, VL, 11, 10, VM, 6, 5, VL, 6, 15);
static const int8_t VG_HASH[] = G_(16, VM, 4, 2, VL, 3, 18, VM, 9, 2, VL, 8, 18, VM, 1, 7, VL, 12, 7, VM, 0, 13, VL, 11, 13);
static const int8_t VG_SP[] = G_(10);
static const int8_t VG_A[] = G_(16, VM, 0, 20, VL, 6, 0, VL, 12, 20, VM, 2, 13, VL, 10, 13);
static const int8_t VG_B[] = G_(16, VM, 0, 20, VL, 0, 0, VL, 6, 0, VA, 6, 5, 5, 5, 24, 40, VL, 0, 10, VM, 6, 10,
                                VA, 6, 15, 6, 5, 24, 40, VL, 0, 20);
static const int8_t VG_C[] = G_(16, VA, 6, 10, 6, 10, 28, 4);
static const int8_t VG_D[] = G_(16, VM, 0, 0, VL, 0, 20, VL, 4, 20, VA, 4, 10, 8, 10, 8, -8, VL, 0, 0);
static const int8_t VG_E[] = G_(16, VM, 12, 0, VL, 0, 0, VL, 0, 20, VL, 12, 20, VM, 0, 10, VL, 9, 10);
static const int8_t VG_F[] = G_(16, VM, 12, 0, VL, 0, 0, VL, 0, 20, VM, 0, 10, VL, 9, 10);
static const int8_t VG_G[] = G_(16, VA, 6, 10, 6, 10, 28, 0, VL, 7, 10);
static const int8_t VG_H[] = G_(16, VM, 0, 0, VL, 0, 20, VM, 12, 0, VL, 12, 20, VM, 0, 10, VL, 12, 10);
static const int8_t VG_I[] = G_(6, VM, 1, 0, VL, 1, 20);
static const int8_t VG_J[] = G_(16, VM, 10, 0, VL, 10, 14, VA, 5, 14, 5, 6, 0, 16);
static const int8_t VG_K[] = G_(16, VM, 0, 0, VL, 0, 20, VM, 12, 0, VL, 0, 12, VM, 4, 8, VL, 12, 20);
static const int8_t VG_L[] = G_(15, VM, 0, 0, VL, 0, 20, VL, 11, 20);
static const int8_t VG_M[] = G_(17, VM, 0, 20, VL, 0, 0, VL, 6, 12, VL, 12, 0, VL, 12, 20);
static const int8_t VG_N[] = G_(16, VM, 0, 20, VL, 0, 0, VL, 12, 20, VL, 12, 0);
static const int8_t VG_O[] = G_(17, VA, 6, 10, 6, 10, 0, 32);
static const int8_t VG_P[] = G_(16, VM, 0, 20, VL, 0, 0, VL, 6, 0, VA, 6, 5, 6, 5, 24, 40, VL, 0, 10);
static const int8_t VG_Q[] = G_(17, VA, 6, 10, 6, 10, 0, 32, VN, VM, 7, 14, VL, 12, 20);
static const int8_t VG_R[] = G_(16, VM, 0, 20, VL, 0, 0, VL, 6, 0, VA, 6, 5, 6, 5, 24, 40, VL, 0, 10, VM, 6, 10, VL, 12, 20);
static const int8_t VG_S[] = G_(16, VA, 6, 5, 6, 5, 29, 8, VA, 6, 15, 6, 5, 24, 43);
static const int8_t VG_T[] = G_(16, VM, 0, 0, VL, 12, 0, VM, 6, 0, VL, 6, 20);
static const int8_t VG_U[] = G_(16, VM, 0, 0, VL, 0, 14, VA, 6, 14, 6, 6, 16, 0, VL, 12, 0);
static const int8_t VG_V[] = G_(16, VM, 0, 0, VL, 6, 20, VL, 12, 0);
static const int8_t VG_W[] = G_(17, VM, 0, 0, VL, 3, 20, VL, 6, 6, VL, 9, 20, VL, 12, 0);
static const int8_t VG_X[] = G_(16, VM, 0, 0, VL, 12, 20, VM, 12, 0, VL, 0, 20);
static const int8_t VG_Y[] = G_(16, VM, 0, 0, VL, 6, 10, VL, 12, 0, VM, 6, 10, VL, 6, 20);
static const int8_t VG_Z[] = G_(16, VM, 0, 0, VL, 12, 0, VL, 0, 20, VL, 12, 20);
#undef G_
static const int8_t *const VG_AZ[26] = {VG_A, VG_B, VG_C, VG_D, VG_E, VG_F, VG_G, VG_H, VG_I, VG_J, VG_K, VG_L, VG_M,
                                        VG_N, VG_O, VG_P, VG_Q, VG_R, VG_S, VG_T, VG_U, VG_V, VG_W, VG_X, VG_Y, VG_Z};
static const int8_t *const VG_09[10] = {VG_0, VG_1, VG_2, VG_3, VG_4, VG_5, VG_6, VG_7, VG_8, VG_9};
static const int8_t *vglyph(uint32_t ch)
{
    if (ch >= 'a' && ch <= 'z')
        ch -= 32u;
    if (ch >= 'A' && ch <= 'Z') return VG_AZ[ch - 'A'];
    if (ch >= '0' && ch <= '9') return VG_09[ch - '0'];
    switch (ch) {
    case ':': return VG_COLON;
    case '.': return VG_DOT;
    case '/': return VG_SLASH;
    case '%': return VG_PCT;
    case '-': return VG_MINUS;
    case '+': return VG_PLUS;
    case '#': return VG_HASH;
    default: return VG_SP;
    }
}
/* text, its top left at x, y (px), h px high, wide sx/sy of the grid's (256 = as high), strokes w (1/256 px);
 * returns its width (px) */
static int32_t vtext_sx(int32_t x, int32_t y, int32_t h, int32_t sx, const char *s, int32_t w, uint16_t c)
{
    int32_t ky = h * 256 / 20, kx = ky * sx / 256, ox = PX(x), oy = PX(y);
    for (; *s; s++) {
        const int8_t *g = vglyph((uint8_t)*s);
        int32_t adv = g[0], px = 0, py = 0, down = 0;
        g++;
        if (!op_off_band(oy - w, oy + PX(h) + w))
            while (*g != VE) {
                int32_t op = *g++;
                if (op == VM || op == VL) {
                    int32_t nx = ox + g[0] * kx, ny = oy + g[1] * ky;
                    g += 2;
                    if (op == VL && down)
                        op_line(px, py, nx, ny, w, c);
                    px = nx, py = ny, down = 1;
                } else if (op == VA) {
                    int32_t cx = ox + g[0] * kx, cy = oy + g[1] * ky, rx = g[2] * kx, ry = g[3] * ky;
                    int32_t a0 = g[4] * 32, a1 = g[5] * 32, n = (a1 - a0) / (h > 40 ? 16 : h > 16 ? 24 : 32), k;
                    g += 6;
                    if (n < 0) n = -n;
                    if (n < 2) n = 2;
                    for (k = 0; k <= n; k++) {
                        uint32_t a = (uint32_t)(a0 + (a1 - a0) * k / n);
                        int32_t nx = cx + ((icos(a) * (rx >> 4)) >> 11), ny = cy + ((isin(a) * (ry >> 4)) >> 11);
                        if (down)
                            op_line(px, py, nx, ny, w, c);
                        px = nx, py = ny, down = 1;
                    }
                } else if (op == VN) {
                    down = 0;
                } else if (op == VD) {
                    op_disc(ox + g[0] * kx, oy + g[1] * ky, w * 3 / 4 + 64, c);
                    g += 2;
                }
            }
        ox += adv * kx;
    }
    return (ox - PX(x)) >> 8;
}
static int32_t vtext(int32_t x, int32_t y, int32_t h, const char *s, int32_t w, uint16_t c)
{
    return vtext_sx(x, y, h, 256, s, w, c);
}
static int32_t vtext_w(int32_t h, int32_t sx, const char *s)              /* its width, the last gap left out */
{
    int32_t n = 0, last = 0, ky = h * 256 / 20;
    for (; *s; s++) {
        const int8_t *g = vglyph((uint8_t)*s);
        n += g[0];
        last = g[0] >= 16 ? g[0] - 12 : g[0] >= 15 ? g[0] - 11 : 5;
    }
    return ((n - last) * (ky * sx / 256)) >> 8;
}
static void vtext_c(int32_t cx, int32_t y, int32_t h, const char *s, int32_t w, uint16_t c)   /* centred on cx */
{
    vtext(cx - vtext_w(h, 256, s) / 2, y, h, s, w, c);
}
static void vtext_r(int32_t rx, int32_t y, int32_t h, const char *s, int32_t w, uint16_t c)   /* ending at rx */
{
    vtext(rx - vtext_w(h, 256, s), y, h, s, w, c);
}

/* ---- the frame's data ---- */
/* beats since PLAY in 1/256 (stopped: a free count at the tempo, for what moves anyway) */
static uint32_t vis_beat_q8(void)
{
    if (song.playing)
        return clk_beat * 256u + clk_pos / (BEAT_U / 256u);
    return vs.free_q8;
}
static void vis_fft(void)
{
    uint32_t i, j = 0, bit, len;
    for (i = 0; i < VIS_FFT; i++) {                 /* Hann window */
        int32_t w = (32768 - icos(i * 2u)) >> 1;
        vis_re[i] = (int16_t)((vis_l[i] * w) >> 15);
        vis_im[i] = 0;
    }
    for (i = 1; i < VIS_FFT; i++) {                 /* bit reversal */
        for (bit = VIS_FFT >> 1; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            int16_t t = vis_re[i]; vis_re[i] = vis_re[j]; vis_re[j] = t;
        }
    }
    for (len = 2; len <= VIS_FFT; len <<= 1) {      /* radix 2, halved each stage (no overflow) */
        uint32_t half = len >> 1, k;
        for (i = 0; i < VIS_FFT; i += len)
            for (k = 0; k < half; k++) {
                uint32_t a = i + k, b = a + half, ix = k * (1024u / len);
                int32_t wr = icos(ix), wi = -isin(ix);
                int32_t tr = (vis_re[b] * wr - vis_im[b] * wi) >> 15, ti = (vis_re[b] * wi + vis_im[b] * wr) >> 15;
                int32_t ar = vis_re[a], ai = vis_im[a];
                vis_re[b] = (int16_t)((ar - tr) >> 1);
                vis_im[b] = (int16_t)((ai - ti) >> 1);
                vis_re[a] = (int16_t)((ar + tr) >> 1);
                vis_im[a] = (int16_t)((ai + ti) >> 1);
            }
    }
}
/* the bands of the last FFT, 0..1000 (60 dB under a full-scale sine) */
static void vis_bands(int16_t *out)
{
    uint32_t k, i;
    for (k = 0; k < VIS_NB; k++) {
        int32_t m = 0, lg = 0, v;
        uint32_t e1 = VIS_EDGE[k + 1u] > VIS_EDGE[k] ? VIS_EDGE[k + 1u] : VIS_EDGE[k] + 1u;
        for (i = VIS_EDGE[k]; i < e1 && i < VIS_FFT / 2u; i++) {
            int32_t a = vis_re[i] < 0 ? -vis_re[i] : vis_re[i], b = vis_im[i] < 0 ? -vis_im[i] : vis_im[i];
            int32_t mag = a > b ? a + b / 2 : b + a / 2;
            if (mag > m)
                m = mag;
        }
        if (m < 2) {
            out[k] = 0;
            continue;
        }
        while ((m >> lg) > 1)
            lg++;
        v = lg * 8 + (((m << 3) >> lg) & 7);        /* a full-scale sine: ~104 */
        out[k] = (int16_t)clamp((v - 24) * 1000 / 80, 0, 1000);
    }
}

/* HEXAGON: its centre, radius (1/256 px) and turn (1/1024) this frame */
#define HX_CX PX(120)
#define HX_CY PX(128)
static int32_t hx_r(void) { return PX(52) + vs.sm[4] * PX(32) / 1000; }
static uint32_t hx_turn(void) { return vis_beat_q8() / 3u; }          /* a side (1/6 turn) every two beats */
static void hx_balls(uint32_t hit)
{
    uint32_t i, k, turn = hx_turn();
    int32_t ap = hx_r() * 887 / 1024 - PX(5);       /* the apothem (the edges' distance), less a ball and the line */
    for (i = 0; i < 4u; i++) {
        if ((hit >> i) & 1u || vs.lvl[i] > 0) {
            if (!vs.alive[i]) {                     /* comes in from the middle */
                vs.bx[i] = HX_CX + (int32_t)(vis_rand() % 41u) * 256 - PX(20);
                vs.by[i] = HX_CY - PX(10);
                vs.bvx[i] = vs.bvy[i] = 0;
            }
            vs.alive[i] = 60;                       /* (two seconds after the track stops) */
        } else if (vs.alive[i]) {
            vs.alive[i]--;
        }
        {                                           /* free flight, no gravity: they ricochet off the walls */
            int32_t sp = (int32_t)vis_isqrt((uint32_t)((vs.bvx[i] >> 2) * (vs.bvx[i] >> 2) + (vs.bvy[i] >> 2) * (vs.bvy[i] >> 2))) << 2;
            if ((hit >> i) & 1u) {                  /* a note: faster (up to 7 px a frame), a little turned */
                if (sp < PX(1)) {
                    uint32_t a = vis_rand() & 1023u;
                    vs.bvx[i] = icos(a) / 16, vs.bvy[i] = isin(a) / 16;   /* 2 px a frame, any way */
                } else {
                    int32_t k2 = sp < PX(4) ? 400 : sp < PX(7) ? 300 : 256, t = (int32_t)(vis_rand() % 129u) - 64;
                    int32_t vx = vs.bvx[i] * k2 / 256, vy = vs.bvy[i] * k2 / 256;
                    vs.bvx[i] = vx - vy * t / 512, vs.bvy[i] = vy + vx * t / 512;
                }
            } else if (sp > PX(1) + 128) {          /* slowing down between notes, never to a stop */
                vs.bvx[i] = vs.bvx[i] * 250 / 256, vs.bvy[i] = vs.bvy[i] * 250 / 256;
            }
        }
        vs.bx[i] += vs.bvx[i];
        vs.by[i] += vs.bvy[i];
        for (k = 0; k < 6u; k++) {                  /* the six edges: back in, and bounced */
            uint32_t a = turn + 85u + k * 1024u / 6u;   /* (the edges' middles: 30 degrees past the corners) */
            int32_t nx = icos(a), ny = isin(a);     /* Q15 */
            int32_t d = (int32_t)(((int64_t)(vs.bx[i] - HX_CX) * nx + (int64_t)(vs.by[i] - HX_CY) * ny) >> 15);
            if (d > ap) {
                int32_t vn = (int32_t)(((int64_t)vs.bvx[i] * nx + (int64_t)vs.bvy[i] * ny) >> 15);
                vs.bx[i] -= (int32_t)(((int64_t)(d - ap) * nx) >> 15);
                vs.by[i] -= (int32_t)(((int64_t)(d - ap) * ny) >> 15);
                if (vn > 0) {                       /* a mirror: bounced off the wall */
                    vs.bounces++;
                    vs.bvx[i] -= (int32_t)(((int64_t)vn * nx) >> 15) * 2;
                    vs.bvy[i] -= (int32_t)(((int64_t)vn * ny) >> 15) * 2;
                }
            }
        }
    }
}

static void vis_update(void)
{
    uint32_t i, w, hit, now = fm1_ms;
    int32_t pk[4], mpk = 0;
    /* the scope: the last 512 frames (left, right) */
    w = scope_w;
    for (i = 0; i < VIS_FFT; i++) {
        vis_l[i] = scope_buf[(w + i) & (SCOPE_N - 1u)];
        vis_r[i] = scope_bufr[(w + i) & (SCOPE_N - 1u)];
        if (vis_l[i] > mpk) mpk = vis_l[i];
        if (-vis_l[i] > mpk) mpk = -vis_l[i];
    }
    /* the tracks' peaks since the last look (as the TRACKS meters take them) and the notes that started */
    fm1_irq_off();
    for (i = 0; i < NPART; i++) {
        pk[i] = trk[i].peak;
        trk[i].peak = 0;
    }
    if (is_drum(TDRUM)) {                           /* (SNOOP 0.10: track 4 a synth: its own peak, above) */
        pk[3] = drums.peak;
        drums.peak = 0;
    }
    hit = vis_hit;
    vis_hit = 0;
    vis_kick_hit = 0;
    fm1_irq_on();
    for (i = 0; i < 4u; i++) {
        int32_t v = trk_level(i) && !trk[i].p[P_MUTE] ? vis_lvl(pk[i]) : 0;   /* (as the TRACKS meters) */
        vs.lvl[i] = (int16_t)v;
        if ((hit >> i) & 1u) {
            vs.flash[i] = 8;
            vs.last = (uint8_t)i;
        } else if (vs.flash[i]) {
            vs.flash[i]--;
        }
    }
    vs.lvl[4] = (int16_t)vis_lvl(mpk);
    for (i = 0; i < 5u; i++)                        /* smoothed: up at once, down ~0.6 s */
        vs.sm[i] = (int16_t)(vs.lvl[i] > vs.sm[i] ? vs.lvl[i] : vs.sm[i] - (vs.sm[i] - vs.lvl[i]) / 10 - 2);
    for (i = 0; i < 5u; i++)
        if (vs.sm[i] < 0) vs.sm[i] = 0;
    /* the free beat count (stopped) */
    {
        uint32_t dt = now - vs.last_ms;
        vs.last_ms = now;
        if (dt > 200u) dt = 200u;
        vs.free_q8 += dt * (uint32_t)song.g[G_BPM] * 256u / 60000u;
    }
    vs.peak = mpk;
    {                                               /* the wave's scale follows the peak */
        int32_t target = mpk < 1600 ? 1600 : mpk;
        vs.scale += (target - vs.scale) / 5;
    }
    switch (vis_style % VIS_N) {
    case 1: {
        int16_t b[VIS_NB];
        vis_fft();
        vis_bands(b);
        for (i = 0; i < VIS_NB; i++) {
            int32_t f = vs.sb[i] - vs.sb[i] * 18 / 100;
            vs.sb[i] = (int16_t)(b[i] > f ? b[i] : f);
            vs.ss[i] = (int16_t)(vs.sb[i] > vs.ss[i] - 12 ? vs.sb[i] : vs.ss[i] - 12);
        }
        break;
    }
    case 2: {                                       /* Lissajous: L-R across, L+R up; three frames kept */
        int32_t m = 1;
        uint8_t (*p)[2];
        for (i = 0; i < VIS_FFT; i += 2u) {
            int32_t s = vis_l[i] + vis_r[i];
            if (s < 0) s = -s;
            if (s > m) m = s;
        }
        if (m < 3000) m = 3000;
        vs.ljscale += (m - vs.ljscale) / 5;
        if (vs.ljscale < 1) vs.ljscale = 1;
        vs.lj_n = (uint8_t)((vs.lj_n + 1u) % 3u);
        p = vis_lj[vs.lj_n];
        for (i = 0; i < 256u; i++) {
            int32_t l = vis_l[i * 2u], r = vis_r[i * 2u];
            p[i][0] = (uint8_t)clamp(120 + (l - r) * 170 / vs.ljscale, 0, 239);
            p[i][1] = (uint8_t)clamp(124 - (l + r) * 95 / vs.ljscale, 0, 239);
        }
        break;
    }
    case 3:                                         /* METERS: fall 3 % a frame, peaks held a second */
        for (i = 0; i < 5u; i++) {
            int32_t v = vs.lvl[i];
            vs.vu[i] = (int16_t)(v > vs.vu[i] - 30 ? v : vs.vu[i] - 30);
            if (v >= vs.pk[i]) {
                vs.pk[i] = (int16_t)v;
                vs.pkt[i] = 0;
            } else if (++vs.pkt[i] > 30) {
                vs.pk[i] = (int16_t)(vs.pk[i] > 20 ? vs.pk[i] - 20 : 0);
            }
        }
        break;
    case 4:
        hx_balls(hit);
        break;
    default:
        break;
    }
}

/* ---- the styles (each draws the whole screen; called once per band) ---- */
static uint32_t vis_pass;                           /* the band being drawn (0 top, 1 bottom) */
static int pass_last_band(void) { return vis_pass == 1u; }
/* the tempo, small, top left: "120" blue, "BPM" grey */
static void op_bpm_tl(void)
{
    char b[8];
    int32_t x;
    fmt_int(b, song.g[G_BPM]);
    x = vtext(16, 16, 16, b, 330, OP_BLUE);
    vtext(16 + x + 6, 22, 10, "BPM", 260, OP_GREY);
}
/* playing / recording / stopped: a red triangle, dot, square, its right edge at rx */
static void op_state(int32_t rx, int32_t y, uint16_t c)
{
    if (song.rec && song.playing) {
        op_disc(PX(rx - 7), PX(y + 7), PX(6), OP_RED);
    } else if (song.playing) {
        op_line(PX(rx - 13), PX(y), PX(rx - 13), PX(y + 14), 330, c);
        op_line(PX(rx - 13), PX(y), PX(rx), PX(y + 7), 330, c);
        op_line(PX(rx), PX(y + 7), PX(rx - 13), PX(y + 14), 330, c);
    } else {
        op_line(PX(rx - 12), PX(y + 1), PX(rx), PX(y + 1), 330, c);
        op_line(PX(rx), PX(y + 1), PX(rx), PX(y + 13), 330, c);
        op_line(PX(rx), PX(y + 13), PX(rx - 12), PX(y + 13), 330, c);
        op_line(PX(rx - 12), PX(y + 13), PX(rx - 12), PX(y + 1), 330, c);
    }
}
/* 1 SCOPE: a heart monitor: the wave between two poles (flat at them), the frame before behind it */
static void vis_scope(void)
{
    static const int32_t X0 = 24, X1 = 216, Y = 132;
    uint32_t i0 = 0, i, n = (uint32_t)(X1 - X0);
    int32_t px = PX(X0), py = PX(Y), sc = vs.scale ? vs.scale : 1;
    op_bpm_tl();
    op_state(224, 17, OP_RED);
    for (i = 1; i < 100u; i++)                      /* a rising zero crossing: the wave stands still */
        if (vis_l[i - 1u] < 0 && vis_l[i] >= 0) {
            i0 = i;
            break;
        }
    if (vs.prev_ok)                                 /* the frame before, in maroon */
        for (i = 1; i <= n; i++)
            op_line(PX(X0 + (int32_t)i - 1), vs.prev[i - 1u] * 16, PX(X0 + (int32_t)i), vs.prev[i] * 16, 300, OP_MAROON);
    vs.prev[0] = (int16_t)(Y * 16);
    for (i = 1; i <= n; i++) {
        int32_t env = isin(i * 512u / n), s = vis_l[i0 + i * 2u];   /* (flat at the poles: sin) */
        int32_t y = PX(Y) - (int32_t)(((int64_t)s * env >> 15) * PX(64) / sc);
        y = clamp(y, PX(40), PX(228));
        op_line(px, py, PX(X0 + (int32_t)i), y, 360, OP_WHITE);
        px = PX(X0 + (int32_t)i), py = y;
        if (pass_last_band())
            vs.prev[i] = (int16_t)(y / 16);
    }
    if (pass_last_band())
        vs.prev_ok = 1;
    op_disc(PX(X0 - 4), PX(Y), PX(4), OP_GREEN);    /* the poles, + and - */
    op_disc(PX(X1 + 4), PX(Y), PX(4), OP_GREEN);
    op_line(PX(X0 - 4), PX(Y), PX(X0), PX(Y), 360, OP_WHITE);
    op_line(PX(X1), PX(Y), PX(X1 + 4), PX(Y), 360, OP_WHITE);
    vtext_c(X0 - 4, 92, 18, "+", 380, OP_GREEN);
    vtext_c(X1 + 4, 92, 18, "-", 380, OP_GREEN);
}
/* 2 SPECTRUM: a staircase of the bands (the slow one behind it), the two highest peaks marked; the last note
 * played (its track's colour) and the wave, small, above */
static void vis_spectrum(void)
{
    static const int32_t X0 = 16, BW = 4, BASE = 216, H = 150;
    uint32_t k, pass;
    int32_t top[2] = {-1, -1};
    for (pass = 0; pass < 2u; pass++) {             /* the slow line, then the fast one */
        const int16_t *b = pass ? vs.sb : vs.ss;
        int32_t py = BASE * 256;
        for (k = 0; k < VIS_NB; k++) {
            int32_t x = X0 + (int32_t)k * BW, y = PX(BASE) - b[k] * PX(H) / 1000;
            op_line(PX(x), py, PX(x), y, pass ? 380 : 330, pass ? OP_GREEN : OP_TEAL);
            op_line(PX(x), y, PX(x + BW), y, pass ? 380 : 330, pass ? OP_GREEN : OP_TEAL);
            py = y;
        }
        op_line(PX(X0 + (int32_t)VIS_NB * BW), py, PX(X0 + (int32_t)VIS_NB * BW), PX(BASE), pass ? 380 : 330,
                pass ? OP_GREEN : OP_TEAL);
    }
    op_line(PX(X0 - 6), PX(BASE), PX(X0 + (int32_t)VIS_NB * BW + 6), PX(BASE), 300, OP_TEAL);
    for (k = 1; k + 1u < VIS_NB; k++) {             /* the two highest peaks (apart) */
        int32_t v = vs.sb[k];
        if (v < 120 || v < vs.sb[k - 1u] || v < vs.sb[k + 1u])
            continue;
        if (top[0] < 0 || v > vs.sb[top[0]]) {
            if (top[0] >= 0 && (int32_t)k - top[0] > 5) top[1] = top[0];
            top[0] = (int32_t)k;
        } else if ((int32_t)k - top[0] > 5 && (top[1] < 0 || v > vs.sb[top[1]])) {
            top[1] = (int32_t)k;
        }
    }
    if (top[1] >= 0 && top[1] - top[0] <= 5 && top[0] - top[1] <= 5)
        top[1] = -1;
    for (k = 0; k < 2u; k++)
        if (top[k] >= 0) {
            int32_t x = X0 + top[k] * BW + BW / 2, y = BASE - vs.sb[top[k]] * H / 1000, d;
            uint16_t c = k ? OP_BLUE : OP_WHITE;
            for (d = y + 8; d < BASE; d += 6)       /* dashed down to the base */
                op_line(PX(x), PX(d), PX(x), PX(d + 3), 260, OP_TEAL);
            op_circle(PX(x), PX(y), PX(6), 320, c);
            op_disc(PX(x), PX(y), PX(2) + 64, c);
        }
    {                                               /* the note last played, and the wave */
        char b[8];
        uint32_t t = vs.last % 4u, i;
        int32_t px = 0, py = 0;
        if (vis_note[t] || vs.flash[t])
            note_name(b, vis_note[t]);
        else
            str_cpy(b, "--", 4);
        vtext(16, 16, 20, b, 380, OP_WHITE);
        {
            char tn[3] = {(char)('1' + t), 0, 0};
            vtext(70, 24, 12, tn, 300, OP_TRK[t]);
        }
        for (i = 0; i < 120u; i++) {
            int32_t m = 0, j;
            for (j = 0; j < 4; j++) {
                int32_t a = vis_l[i * 4u + (uint32_t)j];
                if (a < 0) a = -a;
                if (a > m) m = a;
            }
            {
                int32_t x = PX(96) + (int32_t)i * 256, y = PX(34) - m * PX(10) / (vs.scale > 1 ? vs.scale : 1);
                if (i)
                    op_line(px, py, x, y, 300, OP_GREEN);
                px = x, py = y;
            }
        }
    }
}
/* 3 LISSAJOUS: violet points, the oldest frames dimmest */
static void vis_lissajous(void)
{
    uint32_t f, i;
    static const uint16_t COLS[3] = {RGB(58, 34, 96), RGB(112, 70, 186), RGB(176, 124, 255)};
    op_line(PX(120), PX(36), PX(120), PX(212), 256, OP_DARK);
    op_line(PX(32), PX(124), PX(208), PX(124), 256, OP_DARK);
    for (f = 0; f < 3u; f++) {                      /* the oldest first, dimmest */
        const uint8_t (*p)[2] = vis_lj[(vs.lj_n + 1u + f) % 3u];
        for (i = 0; i < 256u; i++) {
            cv_pset(p[i][0], p[i][1], COLS[f]);
            if (f == 2u)
                cv_pset(p[i][0] + 1, p[i][1], COLS[1]);
        }
    }
    vtext_c(120, 14, 12, "M", 300, OP_GREY);
    vtext_c(18, 118, 12, "L", 300, OP_GREY);
    vtext_c(222, 118, 12, "R", 300, OP_GREY);
}
/* 4 METERS: thin bars, the tracks' colours, red at the top; the peak held white */
static void vis_meters(void)
{
    uint32_t i, k;
    for (i = 0; i < 5u; i++) {
        int32_t x0 = 16 + (int32_t)i * 44, on = vs.vu[i] * 30 / 1000, pk = vs.pk[i] * 30 / 1000;
        uint16_t c = i < 4u ? OP_TRK[i] : OP_LAV;
        char lab[2] = {(char)(i < 4u ? '1' + i : 'M'), 0};
        for (k = 0; k < 30u; k++) {
            int32_t y = 196 - (int32_t)k * 5;
            uint16_t col = (int32_t)k < on ? (k < 26u ? c : OP_RED) : OP_DARK;
            if ((int32_t)k == pk && pk > 0)
                col = OP_WHITE;
            cv_rect(x0, y, 32, 2, col);
        }
        vtext_c(x0 + 16, 210, 14, lab, 330, c);
    }
}
/* 5 HEXAGON: it grows with the master, turns a side a bar; the balls are the tracks playing */
static void vis_hexagon(void)
{
    uint32_t k, turn = hx_turn();
    int32_t r = hx_r(), x[7], y[7];
    char b[8];
    for (k = 0; k < 7u; k++) {
        uint32_t a = turn + k * 1024u / 6u;
        x[k] = HX_CX + ((icos(a) * (r >> 4)) >> 11);
        y[k] = HX_CY + ((isin(a) * (r >> 4)) >> 11);
    }
    for (k = 0; k < 6u; k++)
        op_line(x[k], y[k], x[k + 1u], y[k + 1u], 400, OP_WHITE);
    for (k = 0; k < 4u; k++)
        if (vs.alive[k]) {
            uint16_t c = vs.alive[k] >= 30u ? OP_TRK[k] : mix565(OP_BG, OP_TRK[k], vs.alive[k] * 256 / 30);
            op_disc(vs.bx[k], vs.by[k], PX(4) + (vs.flash[k] ? vs.flash[k] * 24 : 0), c);
        }
    /* the corners: the tempo; the tracks playing; the master */
    vtext(16, 16, 22, "B", 380, OP_BLUE);
    fmt_int(b, song.g[G_BPM]);
    vtext(36, 16, 22, b, 380, OP_WHITE);
    {
        uint32_t n = 0;
        for (k = 0; k < 4u; k++)
            n += vs.alive[k] != 0;
        b[0] = (char)('0' + n), b[1] = 0;
        vtext(178, 16, 22, "T", 380, OP_GREEN);
        op_circle(PX(214), PX(27), PX(10), 330, OP_TEAL);
        vtext_c(214, 21, 12, b, 300, n ? OP_GREEN : OP_GREY);
    }
    vtext(178, 202, 22, "M", 380, OP_RED);
    {
        int32_t m = vs.sm[4] * 26 / 1000;
        op_line(PX(198), PX(220), PX(226), PX(220), 330, OP_BLUE);
        op_disc(PX(198), PX(220), PX(2) + 64, OP_BLUE);
        op_disc(PX(226), PX(220), PX(2) + 64, OP_BLUE);
        op_line(PX(199 + m), PX(206), PX(199 + m), PX(220), 330, OP_LAV);
    }
}
/* the loop sign (centred on cx, cy) */
static void op_loop(int32_t cx, int32_t cy, uint16_t c)
{
    op_line(PX(cx - 10), PX(cy - 6), PX(cx + 6), PX(cy - 6), 330, c);
    op_arc(PX(cx + 6), PX(cy), PX(6), 768, 1280, 330, c);
    op_line(PX(cx + 6), PX(cy + 6), PX(cx - 10), PX(cy + 6), 330, c);
    op_line(PX(cx - 10), PX(cy + 6), PX(cx - 5), PX(cy + 1), 330, c);
    op_line(PX(cx - 10), PX(cy + 6), PX(cx - 5), PX(cy + 11), 330, c);
}
/* a step of track i has something to play */
static int vis_step_on(uint32_t i, uint32_t s)
{
    if (is_drum(&trk[i]))
        return trk[i].dstep[s].on[0] | trk[i].dstep[s].on[1];
    return trk[i].step[s].n != 0;
}
/* 6 TAPE: the reels, the head, the position; the tracks' patterns below, the playhead */
static void vis_tape(void)
{
    uint32_t bq = vis_beat_q8(), s, i;
    int32_t prog = (int32_t)((song.playing ? clk_beat * 256u + clk_pos / (BEAT_U / 256u) : 0u) % (128u * 256u)) * 1000 /
                   (128 * 256);                     /* the tape runs one reel to the other in 32 bars */
    char b[16];
    /* the track box, the position */
    op_line(PX(12), PX(12), PX(34), PX(12), 330, OP_WHITE);
    op_line(PX(34), PX(12), PX(34), PX(34), 330, OP_WHITE);
    op_line(PX(34), PX(34), PX(12), PX(34), 330, OP_WHITE);
    op_line(PX(12), PX(34), PX(12), PX(12), 330, OP_WHITE);
    b[0] = (char)('1' + song.sel % 4u), b[1] = 0;
    vtext_c(23, 15, 16, b, 330, OP_WHITE);
    s = song.playing ? clk_beat : 0u;
    fmt_int(b, (int32_t)(s / 4u + 1u));
    i = str_len(b);
    b[i] = ':', b[i + 1u] = (char)('1' + s % 4u), b[i + 2u] = ':';
    b[i + 3u] = (char)('1' + (song.playing ? clk_pos * 4u / BEAT_U : 0u) % 4u), b[i + 4u] = 0;
    vtext_c(120, 10, 22, b, 380, OP_WHITE);
    for (i = 0; i < 9u; i++)                        /* the scale under it */
        op_line(PX(96 + (int32_t)i * 6), PX(i == 4u ? 38 : 40), PX(96 + (int32_t)i * 6), PX(44), 256, OP_GREY);
    /* the tape's path (behind) */
    op_line(PX(23), PX(98), PX(24), PX(150), 256, OP_GREY);
    op_line(PX(24), PX(150), PX(70), PX(162), 256, OP_GREY);
    op_line(PX(70), PX(162), PX(170), PX(162), 256, OP_GREY);
    op_line(PX(170), PX(162), PX(216), PX(150), 256, OP_GREY);
    op_line(PX(216), PX(150), PX(217), PX(98), 256, OP_GREY);
    /* the reels: the tape on each, the hub, three spokes turning (playing) */
    for (s = 0; s < 2u; s++) {
        int32_t cx = s ? 176 : 64, cy = 98, fill = s ? prog : 1000 - prog, rp = 15 + fill * 22 / 1000, k;
        vis_ellipse(cx, cy, rp, rp, OP_DARK);
        op_circle(PX(cx), PX(cy), PX(rp), 256, OP_DIM);
        op_circle(PX(cx), PX(cy), PX(41), 340, OP_WHITE);
        op_circle(PX(cx), PX(cy), PX(10), 330, OP_WHITE);
        op_circle(PX(cx), PX(cy), PX(4), 330, OP_WHITE);
        for (k = 0; k < 3; k++) {
            uint32_t a = (song.playing ? bq * 2u : 0u) + (uint32_t)k * 341u + 85u;
            op_line(PX(cx) + icos(a) * 14 / 128, PX(cy) + isin(a) * 14 / 128, PX(cx) + icos(a) * 32 / 128,
                    PX(cy) + isin(a) * 32 / 128, 360, OP_WHITE);
        }
    }
    op_loop(120, 98, song.playing ? OP_WHITE : OP_GREY);
    /* the rollers, guides and the head */
    op_circle(PX(24), PX(150), PX(4), 330, OP_WHITE);
    op_circle(PX(216), PX(150), PX(4), 330, OP_WHITE);
    op_circle(PX(70), PX(162), PX(6), 330, OP_WHITE);
    op_disc(PX(70), PX(162), PX(2), OP_WHITE);
    op_circle(PX(170), PX(162), PX(6), 330, OP_WHITE);
    op_line(PX(166), PX(166), PX(174), PX(158), 330, OP_WHITE);
    vis_ellipse(120, 162, 7, 7, OP_BG);
    op_line(PX(113), PX(155), PX(127), PX(155), 340, OP_WHITE);
    op_line(PX(127), PX(155), PX(127), PX(169), 340, OP_WHITE);
    op_line(PX(127), PX(169), PX(113), PX(169), 340, OP_WHITE);
    op_line(PX(113), PX(169), PX(113), PX(155), 340, OP_WHITE);
    /* the MASTER knob, on the right (0.9.1: where the knob is, not what plays; main.c: master_q12 = k10^2 / 256) */
    {
        int32_t k = (int32_t)vis_isqrt(song.master_q12 << 8), y = 140 - clamp(k, 0, 1023) * 114 / 1023;
        op_line(PX(232), PX(26), PX(232), PX(y), 256, OP_GREY);
        op_line(PX(232), PX(y), PX(232), PX(142), 330, OP_RED);
        op_disc(PX(232), PX(y), PX(4), OP_RED);
    }
    /* the tracks' patterns, a lane each; the playheads */
    op_line(PX(20), PX(184), PX(220), PX(184), 300, song.playing ? OP_GREEN : OP_TEAL);
    op_disc(PX(20), PX(184), PX(2) + 64, song.playing ? OP_GREEN : OP_TEAL);
    op_disc(PX(220), PX(184), PX(2) + 64, song.playing ? OP_GREEN : OP_TEAL);
    for (i = 0; i < 5u; i++)
        op_line(PX(20 + (int32_t)i * 50), PX(178), PX(20 + (int32_t)i * 50), PX(182), 256, OP_GREY);
    for (i = 0; i < 4u; i++) {
        uint32_t len = trk[i].p[P_SLEN] > 0 ? (uint32_t)trk[i].p[P_SLEN] : 1u, st, at = trk[i].seq_idx % len;
        int32_t y = 194 + (int32_t)i * 9;
        if (len > NSTEP) len = NSTEP;
        for (st = 0; st < len; st++) {
            int32_t xa = 20 + (int32_t)(st * 200u / len), xb = 20 + (int32_t)((st + 1u) * 200u / len) - 1;
            if (vis_step_on(i, st))
                op_line(PX(xa), PX(y), PX(xb), PX(y), 420, OP_TRK[i]);
            else
                op_line(PX(xa), PX(y), PX(xb), PX(y), 256, OP_DARK);
        }
        if (song.playing && trk[i].seq_active) {
            int32_t x = 20 + (int32_t)((at * 2u + 1u) * 100u / len);
            if (i == song.sel % 4u)
                op_line(PX(x), PX(188), PX(x), PX(226), 300, OP_WHITE);
            else
                op_line(PX(x), PX(y - 3), PX(x), PX(y + 3), 300, OP_WHITE);
        }
    }
}
/* 7 TRANSPORT: the tempo large; the bar and beat, the bar's sixteenths; the swing, playing / recording */
static void vis_transport(void)
{
    char b[12];
    uint32_t i, beat = song.playing ? clk_beat : 0u, six = song.playing ? beat % 4u * 4u + clk_pos * 4u / BEAT_U : 99u;
    fmt_int(b, song.g[G_BPM]);
    vtext_sx(120 - vtext_w(92, 256 * 3 / 4, b) / 2, 70, 92, 256 * 3 / 4, b, 600, OP_LAV);
    vtext_c(120, 176, 10, "BPM", 260, OP_GREY);
    fmt_int(b, (int32_t)(beat / 4u + 1u));
    i = str_len(b);
    b[i] = '.', b[i + 1u] = (char)('1' + beat % 4u), b[i + 2u] = 0;
    vtext(16, 16, 22, b, 380, OP_BLUE);
    for (i = 0; i < 16u; i++)                       /* the bar's sixteenths: past grey, now white */
        op_disc(PX(18 + (int32_t)(i % 4u) * 7), PX(50 + (int32_t)(i / 4u) * 7), PX(1) + 160,
                i == six ? OP_WHITE : i < six && six != 99u ? OP_GREY : OP_DARK);
    fmt_int(b, song.g[G_SWING]);
    str_cpy(b + str_len(b), "%", 2);
    vtext_r(226, 16, 22, b, 380, OP_GREEN);
    if (song.playing && !song.rec)
        op_loop(214, 55, OP_RED);
    else
        op_state(224, 48, OP_RED);
    /* a bar's progress, under it all */
    {
        int32_t p = song.playing ? (int32_t)((beat % 4u) * 256u + clk_pos / (BEAT_U / 256u)) * 200 / 1024 : 0;
        op_line(PX(20), PX(212), PX(220), PX(212), 256, OP_DARK);
        if (p)
            op_line(PX(20), PX(212), PX(20 + p), PX(212), 330, OP_BLUE);
        for (i = 0; i < 5u; i++)
            op_line(PX(20 + (int32_t)i * 50), PX(208), PX(20 + (int32_t)i * 50), PX(216), 256, OP_GREY);
    }
}
/* 8 GEOMETRY: a figure a track (crossed diamonds, nested chevrons), its lines multiplying with the level */
static void vis_geometry(void)
{
    uint32_t i;
    for (i = 0; i < 4u; i++) {
        int32_t cx = PX(60 + (int32_t)(i % 2u) * 120), cy = PX(66 + (int32_t)(i / 2u) * 110), lv = vs.sm[i];
        int32_t H = PX(36) + vs.sm[4] * PX(8) / 1000 + lv * PX(6) / 1000, k, n = 1 + lv * 4 / 1000, w = 330;
        uint16_t c = lv > 0 ? OP_LAV : OP_DIM;
        if (vs.flash[i])
            c = mix565(c, OP_WHITE, vs.flash[i] * 28);
        if ((i == 0u) || (i == 3u)) {               /* the X, a diamond and its tips; more inside, louder */
            int32_t d = H * 11 / 20;
            op_line(cx - H, cy - H, cx + H, cy + H, w, c);
            op_line(cx + H, cy - H, cx - H, cy + H, w, c);
            for (k = 0; k < n; k++) {
                int32_t r = d - k * d / 4;
                op_line(cx, cy - r, cx + r, cy, w, c), op_line(cx + r, cy, cx, cy + r, w, c);
                op_line(cx, cy + r, cx - r, cy, w, c), op_line(cx - r, cy, cx, cy - r, w, c);
            }
            for (k = -1; k <= 1; k += 2) {
                int32_t ty = cy + k * d, r = H / 5;
                op_line(cx, ty - r, cx + r, ty, w, c), op_line(cx + r, ty, cx, ty + r, w, c);
                op_line(cx, ty + r, cx - r, ty, w, c), op_line(cx - r, ty, cx, ty - r, w, c);
            }
        } else {                                    /* chevrons from the four sides, nested */
            int32_t g = H / 5;
            for (k = 0; k < n; k++) {
                int32_t s = H - k * g;
                op_line(cx - s, cy - H, cx, cy - k * g, w, c), op_line(cx, cy - k * g, cx + s, cy - H, w, c);
                op_line(cx - s, cy + H, cx, cy + k * g, w, c), op_line(cx, cy + k * g, cx + s, cy + H, w, c);
                op_line(cx - H, cy - s, cx - k * g, cy, w, c), op_line(cx - k * g, cy, cx - H, cy + s, w, c);
                op_line(cx + H, cy - s, cx + k * g, cy, w, c), op_line(cx + k * g, cy, cx + H, cy + s, w, c);
            }
        }
    }
}
/* 9 TRACKS: a gauge (what the track plays), its pan, its LEVEL large */
static void vis_tracks(void)
{
    uint32_t i;
    for (i = 0; i < 4u; i++) {
        int32_t cx = 30 + (int32_t)i * 60, gy = 64, lv = (int32_t)trk_level(i) * 99 / 127;
        int32_t pan = trk[i].p[P_PAN], nd = trk[i].p[P_MUTE] ? 0 : vs.sm[i];
        uint32_t a = 512u + (uint32_t)(nd * 512 / 1000);
        char b[4];
        b[0] = (char)('1' + i), b[1] = 0;
        vtext_c(cx, gy - 28, 30, b, 330, OP_DIM);   /* the number, faint, behind */
        op_arc(PX(cx), PX(gy), PX(22), 512, 896, 300, OP_GREY);
        op_arc(PX(cx), PX(gy), PX(22), 896, 1024, 330, OP_RED);
        op_line(PX(cx), PX(gy - 26), PX(cx), PX(gy - 19), 300, OP_GREY);
        op_line(PX(cx), PX(gy), PX(cx) + icos(a) * 25 / 128, PX(gy) + isin(a) * 25 / 128, 400,
                nd > 880 ? OP_RED : OP_WHITE);
        op_disc(PX(cx), PX(gy), PX(3), nd > 880 ? OP_RED : OP_WHITE);
        for (a = 0; a < 3u; a++)                    /* dotted down to the pan */
            op_disc(PX(cx), PX(gy + 9 + (int32_t)a * 5), 230, OP_GREY);
        {
            uint32_t pa = (uint32_t)(768 + pan * 384 / 64) & 1023u;
            op_circle(PX(cx), PX(gy + 34), PX(7), 300, OP_GREY);
            op_line(PX(cx), PX(gy + 34), PX(cx) + icos(pa) * 8 / 128, PX(gy + 34) + isin(pa) * 8 / 128, 330, OP_WHITE);
            vtext_c(cx - 17, gy + 28, 11, "L", 260, OP_GREY);
            vtext_c(cx + 17, gy + 28, 11, "R", 260, OP_GREY);
        }
        b[0] = (char)('0' + lv / 10), b[1] = (char)('0' + lv % 10), b[2] = 0;
        vtext_sx(cx - vtext_w(66, 112, b) / 2, 126, 66, 112, b, 440, OP_TRK[i]);
    }
}

/* the visualiser, every other frame: the update, then both bands */
static void vis_draw(void)
{
    if (!vis_shown_last || ui.force) {
        vis_shown_last = 1;
        lcd_fill(0, 0, 240, 240, OP_BG);
    } else if (ui.frame & 1u) {
        return;
    }
    vis_update();
    for (vis_pass = 0; vis_pass < 2u; vis_pass++) {
        cv_begin(240, 120, OP_BG);
        cv_oy = -(int32_t)(vis_pass * 120u);
        switch (vis_style % VIS_N) {
        case 0: vis_scope(); break;
        case 1: vis_spectrum(); break;
        case 2: vis_lissajous(); break;
        case 3: vis_meters(); break;
        case 4: vis_hexagon(); break;
        case 5: vis_tape(); break;
        case 6: vis_transport(); break;
        case 7: vis_geometry(); break;
        default: vis_tracks(); break;
        }
        if (!vis_pass && (vis_name_t || ui.msg_t)) {   /* the style's name a second (or a message) */
            char n[8];
            const char *s = ui.msg_t ? ui.msg : VIS_NAME[vis_style % VIS_N];
            cv_rect(0, 0, 240, 44, OP_BG);
            if (ui.msg_t) {
                cv_text(120 - text_w(&FONT_S, s) / 2, 14, &FONT_S, s, C_WHITE);
            } else {
                vtext_c(120, 16, 14, s, 330, OP_WHITE);
                fmt_int(n, (int32_t)(vis_style % VIS_N) + 1);
                str_cpy(n + str_len(n), "/9", 3);
                vtext(8, 4, 10, n, 260, OP_GREY);
            }
        }
        cv_oy = 0;
        cv_blit(0, vis_pass * 120u);
    }
    if (vis_name_t)
        vis_name_t--;
}
/* SELECT on the visualiser: the next / previous style (round) */
static void vis_select(int32_t s)
{
    vis_style = (uint8_t)((vis_style + (s > 0 ? 1u : VIS_N - 1u)) % VIS_N);
    vis_name_t = 30;
    vs.prev_ok = 0;
    ui.force = 1;
    settings_later = 1;                                 /* (kept with the settings, once stopped) */
}

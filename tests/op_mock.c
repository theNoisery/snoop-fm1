/* SPDX-License-Identifier: GPL-3.0-only */
/* SNOOP 0.12 (draft): mockups of the OP look for the main screens, drawn by the firmware's own drawing code (ui_vis.c:
 * anti-aliased lines, the thin vector font; gfx.c: FONT_N, Terminus regular) from the firmware's own state (a groove
 * playing, the default sounds). Not wired into the UI yet: pictures to decide on. Screens: DIR/op-*.ppm */
#define UI_PAGES_HARNESS_ONLY 1
#include "ui_pages_test.c"

#define OPW(w) ((int32_t)((w) * 256))                   /* a width in px, 1/256 */
static uint16_t PC[4] = {OP_BLUE, OP_GREEN, OP_WHITE, OP_RED};   /* the scheme: track i's colour, knob k's colour */
#define KCOL PC
#define REC_RED RGB(246, 52, 84)

static void ntext(int32_t x, int32_t y, const char *s, uint16_t c) { cv_text(x, y, &FONT_N, s, c); }
static int32_t ntext_w(const char *s) { return text_w(&FONT_N, s); }
static void ntext_r(int32_t rx, int32_t y, const char *s, uint16_t c) { ntext(rx - ntext_w(s), y, s, c); }
static void ntext_c(int32_t cx, int32_t y, const char *s, uint16_t c) { ntext(cx - ntext_w(s) / 2, y, s, c); }

/* a rounded rectangle, outlined (fill 0) or filled */
static void op_rrect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, int32_t lw, uint16_t c, int fill)
{
    if (fill) {
        int32_t yy;
        for (yy = 0; yy < h; yy++) {
            int32_t in = 0;
            if (yy < r) in = r - (int32_t)vis_isqrt((uint32_t)(r * r - (r - yy) * (r - yy)));
            if (yy >= h - r) in = r - (int32_t)vis_isqrt((uint32_t)(r * r - (yy - (h - r - 1)) * (yy - (h - r - 1))));
            cv_rect(x + in, y + yy, w - 2 * in, 1, c);
        }
        return;
    }
    op_line(PX(x + r), PX(y), PX(x + w - r), PX(y), lw, c);
    op_line(PX(x + r), PX(y + h), PX(x + w - r), PX(y + h), lw, c);
    op_line(PX(x), PX(y + r), PX(x), PX(y + h - r), lw, c);
    op_line(PX(x + w), PX(y + r), PX(x + w), PX(y + h - r), lw, c);
    op_arc(PX(x + r), PX(y + r), PX(r), 512, 768, lw, c);
    op_arc(PX(x + w - r), PX(y + r), PX(r), 768, 1024, lw, c);
    op_arc(PX(x + w - r), PX(y + h - r), PX(r), 0, 256, lw, c);
    op_arc(PX(x + r), PX(y + h - r), PX(r), 256, 512, lw, c);
}
/* an OP-1 dial: a grey track of 270 degrees, the value along it in colour, a dot at its end */
static void op_dial(int32_t cx, int32_t cy, int32_t r, int32_t v, uint16_t c)
{
    int32_t a0 = 384, a1 = 384 + 768 * clamp(v, 0, 1000) / 1000;
    op_arc(PX(cx), PX(cy), PX(r), 384, 1152, OPW(1.4), OP_DARK);
    if (a1 > a0 + 4)
        op_arc(PX(cx), PX(cy), PX(r), a0, a1, OPW(2.2), c);
    op_disc(PX(cx) + icos((uint32_t)a1) * r / 128, PX(cy) + isin((uint32_t)a1) * r / 128, PX(2) + 64, OP_WHITE);
}
/* a dashed vertical line */
static void op_dash(int32_t x, int32_t y0, int32_t y1, uint16_t c)
{
    int32_t y;
    for (y = y0; y < y1; y += 6)
        op_line(PX(x), PX(y), PX(x), PX(y + 3 < y1 ? y + 3 : y1), OPW(1), c);
}
/* the track's number in a box: filled when it is the selected one */
static void op_badge(int32_t x, int32_t y, uint32_t i, int sel)
{
    char b[2] = {(char)('1' + i), 0};
    uint16_t c = PC[i];
    if (sel) {
        op_rrect(x, y, 20, 20, 3, 0, c, 1);
        vtext_c(x + 10, y + 4, 12, b, OPW(1.5), OP_BG);
    } else {
        op_rrect(x, y, 20, 20, 3, OPW(1.2), c, 0);
        vtext_c(x + 10, y + 4, 12, b, OPW(1.3), c);
    }
}
/* the top bar of a page: its title (thin, large), the track and the tempo */
static void op_top(const char *title, uint32_t trk_i)
{
    char b[8];
    vtext(10, 9, 15, title, OPW(1.5), OP_WHITE);
    op_badge(214, 6, trk_i, 1);
    fmt_int(b, song.g[G_BPM]);
    vtext_r(204, 10, 12, b, OPW(1.3), OP_LAV);
    op_line(PX(10), PX(32), PX(230), PX(32), OPW(1), OP_DARK);
}
/* the bottom line: the sound, the page */
static void op_foot(uint32_t trk_i, const char *page)
{
    const engine_t *e = ENGINES[trk[trk_i].engine];
    op_line(PX(10), PX(216), PX(230), PX(216), OPW(1), OP_DARK);
    ntext(10, 221, e->presets[trk[trk_i].preset % e->npresets].name, OP_WHITE);
    ntext_r(230, 221, page, OP_GREY);
}
/* four knobs, a column each (58 px): the label (small, grey), the value (thin, large) and its unit, and under them a
 * thin bar in the knob's colour (the OP-1's four: blue, green, white, red) with a dot where it stands */
static void op_knob1(int32_t x, int32_t y, const char *lab, const char *val, const char *unit, int32_t rat, uint16_t c)
{
    int32_t w, xb = x + 2 + 50 * clamp(rat, 0, 1000) / 1000;
    ntext(x + 1, y, lab, OP_GREY);
    w = vtext(x + 2, y + 18, 14, val, OPW(1.4), OP_WHITE);
    if (unit[0] && w + 2 + ntext_w(unit) <= 56)
        ntext(x + 4 + w, y + 17, unit, OP_GREY);
    op_line(PX(x + 2), PX(y + 38), PX(x + 52), PX(y + 38), OPW(1.2), OP_DARK);
    if (xb > x + 2)
        op_line(PX(x + 2), PX(y + 38), PX(xb), PX(y + 38), OPW(2.2), c);
    op_disc(PX(xb), PX(y + 38), PX(2) + 64, OP_WHITE);
}
static void op_knobs(int32_t y, const char *const lab[4], const char *const val[4], const char *const unit[4],
                     const int32_t rat[4])
{
    uint32_t k;
    for (k = 0; k < 4u; k++)
        if (lab[k][0])
            op_knob1(4 + (int32_t)k * 59, y, lab[k], val[k], unit[k], rat[k], KCOL[k]);
}

/* ------------------------------------------------------------------------------------------------ the screens */
static void scr_tracks(void)
{
    char b[16];
    uint32_t i, s;
    fmt_int(b, song.g[G_BPM]);
    {
        int32_t w = vtext(10, 6, 24, b, OPW(1.8), OP_WHITE);
        ntext(14 + w, 13, "bpm", OP_GREY);
    }
    op_line(PX(150), PX(10), PX(150), PX(24), OPW(1.6), PC[1]);   /* playing */
    op_line(PX(150), PX(10), PX(162), PX(17), OPW(1.6), PC[1]);
    op_line(PX(162), PX(17), PX(150), PX(24), OPW(1.6), PC[1]);
    vtext(170, 10, 14, "1.1", OPW(1.4), OP_WHITE);
    op_disc(PX(222), PX(17), PX(6), REC_RED);
    for (i = 0; i < NTRK; i++) {
        int32_t y = 38 + (int32_t)i * 38, len = trk[i].p[P_SLEN] > 0 ? trk[i].p[P_SLEN] : 16, at = (int32_t)(trk[i].seq_idx % (uint32_t)len);
        const engine_t *e = ENGINES[trk[i].engine];
        op_badge(8, y + 2, i, i == song.sel);
        if (is_drum(&trk[i]))
            str_cpy(b, DRUM_KIT_NAMES[drum_kit()], sizeof b);
        else
            str_cpy(b, e->presets[trk[i].preset % e->npresets].name, sizeof b);
        ntext(36, y, b, OP_WHITE);
        {
            char en[12]; uint32_t q;
            str_cpy(en, is_drum(&trk[i]) ? "drums" : e->name, sizeof en);
            for (q = 0; en[q]; q++) if (en[q] >= 'A' && en[q] <= 'Z') en[q] = (char)(en[q] + 32);
            ntext(44 + ntext_w(b), y, en, OP_GREY);
        }
        for (s = 0; s < 16u; s++) {                     /* the steps: a lane */
            int32_t xa = 36 + (int32_t)s * 11, xb = xa + 8;
            int on = vis_step_on(i, s % (uint32_t)len);
            if (s >= (uint32_t)len)
                continue;
            op_line(PX(xa), PX(y + 25), PX(xb), PX(y + 25), on ? OPW(3) : OPW(1.2), on ? PC[i] : OP_DARK);
            if ((int32_t)s == at)
                op_line(PX(xa + 4), PX(y + 19), PX(xa + 4), PX(y + 31), OPW(1.2), OP_WHITE);
        }
        {                                               /* its level: a small gauge */
            int32_t lv = (int32_t)trk_level(i) * 1000 / 127;
            uint32_t a = 512u + (uint32_t)(lv * 512 / 1000);
            op_arc(PX(222), PX(y + 26), PX(10), 512, 1024, OPW(1.2), OP_DARK);
            op_arc(PX(222), PX(y + 26), PX(10), 512, (int32_t)a, OPW(1.6), PC[i]);
            op_line(PX(222), PX(y + 26), PX(222) + icos(a) * 12 / 128, PX(y + 26) + isin(a) * 12 / 128, OPW(1.4), OP_WHITE);
        }
    }
    {
        static const char *const L[4] = {"swing", "level", "steps", "pan"};   /* (lower case: FONT_N) */
        char v[4][8];
        const char *vp[4] = {v[0], v[1], v[2], v[3]}, *u[4] = {"%", "", "", ""};
        int32_t r[4];
        fmt_int(v[0], song.g[G_SWING]), r[0] = song.g[G_SWING] * 10;
        fmt_int(v[1], (int32_t)trk_level(song.sel)), r[1] = (int32_t)trk_level(song.sel) * 1000 / 127;
        fmt_int(v[2], trk[song.sel].p[P_SLEN]), r[2] = trk[song.sel].p[P_SLEN] * 1000 / 64;
        fmt_int(v[3], trk[song.sel].p[P_PAN]), r[3] = 500 + trk[song.sel].p[P_PAN] * 500 / 64;
        op_line(PX(10), PX(190), PX(230), PX(190), OPW(1), OP_DARK);
        op_knobs(194, L, vp, u, r);
    }
}
static void scr_env(void)
{
    static const char *const L[4] = {"attack", "decay", "sustain", "release"};
    static const char *const V[4] = {"1.0", "3.36", "0", "43"}, *U[4] = {"ms", "s", "%", "ms"};
    static const int32_t R[4] = {20, 640, 0, 340};
    /* the envelope: a segment a knob, in its colour, a dot at each turn */
    int32_t x0 = 14, base = 200, top = 104, xa = 22, xd = 120, xs = 170, xr = 226, ys = base - (top - base) * 0 / 100;
    op_top("ENVELOPE", 0);
    op_knobs(38, L, V, U, R);
    op_line(PX(x0), PX(base), PX(228), PX(base), OPW(1), OP_DARK);
    op_dash(xa, top + 6, base, OP_TEAL);
    op_dash(xd, top + 6, base, OP_TEAL);
    op_dash(xs, top + 6, base, OP_TEAL);
    op_line(PX(x0), PX(base), PX(xa), PX(top), OPW(1.8), PC[0]);
    {                                                   /* the decay: exponential */
        int32_t k, px = PX(xa), py = PX(top);
        for (k = 1; k <= 40; k++) {
            double f = k / 40.0, y = top + (base - top) * (1.0 - exp(-4.0 * f)) / (1.0 - exp(-4.0));
            int32_t x = PX(xa) + (int32_t)((xd - xa) * f * 256), yy = (int32_t)(y * 256);
            op_line(px, py, x, yy, OPW(1.8), PC[1]);
            px = x, py = yy;
        }
    }
    op_line(PX(xd), PX(ys), PX(xs), PX(ys), OPW(1.8), PC[2]);
    op_line(PX(xs), PX(ys), PX(xr), PX(base), OPW(1.8), PC[3]);
    op_circle(PX(xa), PX(top), PX(5), OPW(1.4), PC[0]);
    op_disc(PX(xa), PX(top), PX(2), PC[0]);
    op_circle(PX(xd), PX(ys), PX(5), OPW(1.4), OP_WHITE);
    op_disc(PX(xd), PX(ys), PX(2), OP_WHITE);
    op_foot(0, "env 1/2");
}
static void scr_filter(void)
{
    static const char *const L[4] = {"type", "cutoff", "reso", "keytrk"};
    static const char *const V[4] = {"LP", "2.4", "62", "0"}, *U[4] = {"", "khz", "%", "%"};
    static const int32_t R[4] = {0, 560, 620, 0};
    int32_t k, px = 0, py = 0, base = 200, x0 = 14, x1 = 228, fc = 0;
    op_top("FILTER", 0);
    op_knobs(38, L, V, U, R);
    op_line(PX(x0), PX(base), PX(x1), PX(base), OPW(1), OP_DARK);
    for (k = 0; k <= 107; k++) {                        /* a low-pass's response, the resonance's peak */
        double f = 20.0 * pow(1000.0, k / 107.0), c = 2400.0, q = 3.2, w = f / c;
        double mag = 1.0 / sqrt((1 - w * w) * (1 - w * w) + (w / q) * (w / q));
        double db = 20 * log10(mag), y = 150 - db * 2.6;
        int32_t x = PX(x0) + k * 512, yy = (int32_t)((y < base ? y : base) * 256);
        if (k) op_line(px, py, x, yy, OPW(1.8), PC[1]);
        px = x, py = yy;
        if (!fc && f >= c) fc = x;
    }
    op_dash(fc >> 8, 120, base, OP_TEAL);
    op_circle(fc, PX(108), PX(5), OPW(1.4), OP_WHITE);
    op_disc(fc, PX(108), PX(2), OP_WHITE);
    op_foot(0, "filter 3/5");
}
static void scr_notes(void)
{
    static const char *const L[4] = {"note", "pitch", "length", "force"};
    static const char *const V[4] = {"1/16", "C3", "2", "NORM"}, *U[4] = {"", "", "step", ""};
    static const int32_t R[4] = {250, 400, 120, 0};
    track_t *t = &trk[1];
    int32_t lo = 60, hi = 79, x0 = 30, x1 = 230, y0 = 86, y1 = 206, rows = hi - lo + 1, s;
    op_top("NOTES", 1);
    op_knobs(38, L, V, U, R);
    for (s = 0; s < rows; s++) {                        /* the rows: the black keys darker */
        int32_t y = y1 - (s + 1) * (y1 - y0) / rows, n = (lo + s) % 12;
        if (n == 1 || n == 3 || n == 6 || n == 8 || n == 10)
            cv_rect(x0, y, x1 - x0, (y1 - y0) / rows, RGB(14, 14, 18));
        if (n == 0) {
            char b[6];
            note_name(b, (uint32_t)(lo + s));
            ntext(4, y - 6, b, OP_GREY);
            op_line(PX(x0), PX(y + (y1 - y0) / rows), PX(x1), PX(y + (y1 - y0) / rows), OPW(1), OP_DARK);
        }
    }
    for (s = 0; s <= 16; s += 4)
        op_dash(x0 + s * (x1 - x0) / 16, y0, y1, s % 16 ? OP_DARK : OP_DIM);
    for (s = 0; s < 16; s++) {                          /* the notes: bars in the track's colour */
        const step_t *st = &t->step[s];
        uint32_t k;
        for (k = 0; k < st->n && k < 4u; k++) {
            int32_t n = st->note[k];
            if (n < lo || n > hi) continue;
            {
                int32_t y = y1 - (n - lo) * (y1 - y0) / rows - (y1 - y0) / rows / 2;
                int32_t xa = x0 + s * (x1 - x0) / 16 + 2, xb = x0 + (s + 2) * (x1 - x0) / 16 - 2;
                op_line(PX(xa), PX(y), PX(xb), PX(y), OPW(4), PC[1]);
                op_disc(PX(xa), PX(y), PX(2) + 128, OP_WHITE);
            }
        }
    }
    {
        int32_t x = x0 + (int32_t)(t->seq_idx % 16u) * (x1 - x0) / 16;
        op_line(PX(x), PX(y0), PX(x), PX(y1), OPW(1.2), OP_WHITE);
    }
    op_foot(1, "notes 1/2");
}
static void scr_drums(void)
{
    static const char *const ROW[8] = {"kick", "snare", "clap", "hat", "open", "tom", "rim", "crash"};
    static const uint8_t SND[8] = {0, 2, 3, 4, 5, 9, 7, 11};
    char b[16];
    uint32_t r, s, at = TDRUM->seq_idx % 16u;
    str_cpy(b, DRUM_KIT_NAMES[drum_kit()], sizeof b);
    vtext(10, 8, 20, b, OPW(1.6), OP_WHITE);
    ntext(14 + vtext_w(20, 256, b), 13, "kit", OP_GREY);
    op_badge(214, 6, 3, 1);
    op_line(PX(10), PX(36), PX(230), PX(36), OPW(1), OP_DARK);
    for (r = 0; r < 8u; r++) {
        int32_t y = 48 + (int32_t)r * 17;
        ntext(8, y - 8, ROW[r], r == 3u ? OP_WHITE : OP_GREY);
        for (s = 0; s < 16u; s++) {
            int32_t x = 62 + (int32_t)s * 11;
            const dstep_t *d = &TDRUM->dstep[s];
            if (dstep_has(d, SND[r])) {
                uint32_t lv = dstep_lvl(d, SND[r]);
                op_disc(PX(x), PX(y), lv == LV_SOFT ? PX(2) : PX(3) + 160, s == at ? OP_WHITE : PC[3]);
                if (lv == LV_HARD) op_circle(PX(x), PX(y), PX(6), OPW(1.2), PC[3]);
            } else {
                op_disc(PX(x), PX(y), 200, s % 4u ? OP_DARK : OP_DIM);
            }
        }
    }
    op_line(PX(62 + (int32_t)at * 11), PX(40), PX(62 + (int32_t)at * 11), PX(184), OPW(1), OP_GREY);
    {
        static const char *const L[4] = {"sound", "step", "hit", "level"};
        static const char *const V[4] = {"HAT", "7", "ON", "SOFT"}, *U[4] = {"", "", "", ""};
        static const int32_t R[4] = {280, 420, 1000, 330};
        op_line(PX(10), PX(190), PX(230), PX(190), OPW(1), OP_DARK);
        op_knobs(194, L, V, U, R);
    }
}
static void scr_roll(void)
{
    static const char *const P[16] = {"kick", "kick2", "snare", "clap", "hat", "open", "pedal", "rim",
                                      "snr2", "tom l", "tom h", "crash", "ride", "shake", "conga", "bell"};
    uint32_t i;
    vtext(10, 8, 20, "ROLL", OPW(1.7), PC[3]);
    ntext(80, 13, "+ key: roll", OP_GREY);
    op_line(PX(10), PX(36), PX(230), PX(36), OPW(1), OP_DARK);
    for (i = 0; i < 16u; i++) {
        int32_t x = 10 + (int32_t)(i % 4u) * 56, y = 44 + (int32_t)(i / 4u) * 36;
        if (i == 4u) {
            op_rrect(x, y, 50, 30, 6, 0, OP_WHITE, 1);
            ntext_c(x + 25, y + 7, P[i], OP_BG);
        } else {
            op_rrect(x, y, 50, 30, 6, OPW(1.2), OP_DIM, 0);
            ntext_c(x + 25, y + 7, P[i], OP_WHITE);
        }
    }
    {
        static const char *const L[4] = {"rate", "mutate", "", ""};
        static const char *const V[4] = {"1/32", "31", "", ""}, *U[4] = {"", "%", "", ""};
        static const int32_t R[4] = {750, 310, 0, 0};
        op_line(PX(10), PX(190), PX(230), PX(190), OPW(1), OP_DARK);
        op_knobs(194, L, V, U, R);
    }
}

static void shot(const char *name, void (*f)(void))
{
    uint32_t pass;
    for (pass = 0; pass < 2u; pass++) {
        cv_begin(240, 120, OP_BG);
        cv_oy = -(int32_t)(pass * 120u);
        f();
        cv_oy = 0;
        cv_blit(0, pass * 120u);
    }
    ppm(name);
}

int main(int argc, char **argv)
{
    uint32_t i, j;
    outdir = argc > 1 ? argv[1] : "build/host";
    panel = PANEL_DEFAULT;
    layers_init();
    host_tracks_init();
    for (i = 0; i < NSYN; i++) { set_engine_of(&trk[i], TRK_DEF[i][0]); apply_preset_to(&trk[i], TRK_DEF[i][1]); trk[i].engine = trk[i].eng_req; }
    TDRUM->p[P_E0] = DRUM_DEFAULT_KIT;
    for (j = 0; j < NTRK; j++) { steps_clear(&trk[j]); trk[j].p[P_SLEN] = 16; trk[j].seq_active = 1; }
    for (j = 0; j < 16u; j++) {
        step_t *sp;
        if (!(j & 1u)) { sp = &trk[0].step[j]; sp->n = 1; sp->note[0] = 36; sp->time = ST_NOTE; sp->vel = 100; }
        if ((j % 4u) == 2u) { sp = &trk[1].step[j]; sp->n = 3; sp->note[0] = 65; sp->note[1] = 69; sp->note[2] = 72; sp->time = ST_NOTE; sp->vel = 100; }
        if (!(j % 8u)) { sp = &trk[2].step[j]; sp->n = 2; sp->note[0] = 62; sp->note[1] = 69; sp->time = ST_NOTE; sp->vel = 100; }
        memset(&TDRUM->dstep[j], 0, sizeof(dstep_t));
        if (!(j % 4u)) dstep_set(&TDRUM->dstep[j], 0, j == 0u ? LV_HARD : LV_NORM, 0);
        if ((j % 8u) == 4u) dstep_set(&TDRUM->dstep[j], 2, LV_NORM, 0);
        if (j & 1u) dstep_set(&TDRUM->dstep[j], 4, j % 4u == 3u ? LV_NORM : LV_SOFT, 0);
        if (j == 14u) dstep_set(&TDRUM->dstep[j], 5, LV_NORM, 0);
    }
    trk[1].p[P_PAN] = -12, trk[2].p[P_LEVEL] = 80;
    song.g[G_BPM] = 101;
    song.sel = 1;
    go_home(); frames(4);
    transport_req = 1; frames(37);
    {   /* the schemes, on black: tracks / knobs 1..4 */
        static const struct { const char *n; uint16_t c[4]; } SCH[] = {
            {"op1", {RGB(84, 120, 246), RGB(22, 222, 140), RGB(236, 236, 242), RGB(246, 52, 84)}},
            {"ocean", {RGB(80, 124, 255), RGB(60, 206, 232), RGB(196, 182, 255), RGB(255, 138, 92)}},
            {"sunset", {RGB(255, 132, 56), RGB(255, 200, 70), RGB(255, 92, 140), RGB(166, 120, 255)}},
            {"synthwave", {RGB(176, 106, 255), RGB(255, 78, 174), RGB(62, 216, 240), RGB(255, 210, 74)}},
            {"ice", {RGB(236, 236, 242), RGB(150, 190, 255), RGB(110, 136, 255), RGB(255, 122, 46)}},
        };
        uint32_t q;
        char nm[40];
        for (q = 0; q < sizeof SCH / sizeof SCH[0]; q++) {
            memcpy(PC, SCH[q].c, sizeof PC);
            song.rec = 1, song.sel = 1;
            snprintf(nm, sizeof nm, "sch-%s-tracks", SCH[q].n); shot(nm, scr_tracks);
            song.rec = 0, song.sel = 0;
            snprintf(nm, sizeof nm, "sch-%s-env", SCH[q].n); shot(nm, scr_env);
            song.sel = 3;
            snprintf(nm, sizeof nm, "sch-%s-drums", SCH[q].n); shot(nm, scr_drums);
        }
        memcpy(PC, SCH[0].c, sizeof PC);
    }
    song.sel = 1;
    song.rec = 1;
    shot("op-tracks", scr_tracks);
    song.rec = 0;
    song.sel = 0;
    shot("op-env", scr_env);
    shot("op-filter", scr_filter);
    song.sel = 1;
    shot("op-notes", scr_notes);
    song.sel = 3;
    shot("op-drums", scr_drums);
    shot("op-roll", scr_roll);
    transport_req = 2; frames(2);
    return 0;
}

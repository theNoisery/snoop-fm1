/* SPDX-License-Identifier: GPL-3.0-only */
/* SNOOP 0.5: clicks when a knob of the delay, the reverb or the chorus turns. A 220 Hz sine goes into the three
 * sends (fx_buses directly); a knob moves every 40 ms (up, then down, 1..3 detents a move, as a hand turning it);
 * a click is a jump the sine never makes: the largest second difference of the return, against the same render with
 * the knob still. Each knob's ratio is printed; a ratio over LIMIT fails. */
#define main hostsim_main
#include "hostsim.c"
#undef main
#include <math.h>
#include <string.h>

#define LIMIT 3.0
static uint32_t fails;

typedef struct {
    const char *name;
    int id, lo, hi;                         /* the knob and its sweep */
    int setup[8][2];                        /* other globals: {id, value}, -1 ends */
    double limit;                           /* 0: LIMIT */
} knob_t;

static void reset(void)
{
    uint32_t i;
    for (i = 0; i < G_COUNT; i++)
        song.g[i] = GP[i].def;
    song.g[G_BPM] = 120;
    memset(dly_buf, 0, sizeof dly_buf), memset(cho_buf, 0, sizeof cho_buf);
    memset(rev_line, 0, sizeof rev_line), memset(rev_ap, 0, sizeof rev_ap), memset(plt_ext, 0, sizeof plt_ext);
    memset(&fx, 0, sizeof fx), memset(&tape, 0, sizeof tape), memset(&plt, 0, sizeof plt), memset(&sm, 0, sizeof sm);
    rev_frz = 0;
    thr_dly = thr_rev = 0;
}

/* 3 s: 1 s to fill the effects, then 2 s turning (or not); the largest |second difference| of the last 2 s */
static double run(const knob_t *k, int turn, const char *wav)
{
    static int32_t ci[CTL], di[CTL], ri[CTL], wl[CTL], wr[CTL];
    const uint32_t frames = 3u * FS / CTL * CTL;
    uint32_t f, i, ph = 0, step = 0;
    int32_t p1 = 0, p2 = 0, v, dir = 1;
    double worst = 0;
    FILE *w = 0;
    reset();
    for (i = 0; i < 8u && k->setup[i][0] >= 0; i++)
        song.g[k->setup[i][0]] = (int16_t)k->setup[i][1];
    v = k->lo;
    song.g[k->id] = (int16_t)v;
    if (wav && (w = fopen(wav, "wb")))
        wav_hdr(w, frames);
    for (f = 0; f < frames; f += CTL) {
        if (turn && f >= FS && f / (FS / 25u) != step) {   /* every 40 ms */
            step = f / (FS / 25u);
            v += dir * (int32_t)(1u + step % 3u);
            if (v >= k->hi) v = k->hi, dir = -1;
            if (v <= k->lo) v = k->lo, dir = 1;
            song.g[k->id] = (int16_t)v;
        }
        for (i = 0; i < CTL; i++) {
            int32_t s = (int32_t)(sine_i(ph) / 4);         /* -6 dB of a full-scale send */
            ph += (uint32_t)(220.0 * 4294967296.0 / FS);
            ci[i] = s, di[i] = s, ri[i] = s;
        }
        fx_buses(ci, di, ri, wl, wr, CTL);
        for (i = 0; i < CTL; i++) {
            int32_t d2 = wl[i] - 2 * p1 + p2;
            if (f >= FS && (d2 < 0 ? -d2 : d2) > worst)
                worst = d2 < 0 ? -d2 : d2;
            p2 = p1, p1 = wl[i];
            if (w) wav_put(w, wl[i] >> 1, wr[i] >> 1);
        }
    }
    if (w) fclose(w);
    return worst;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : 0;
    static const knob_t K[] = {
        {"DIGI TIME (sync)", G_DTIME, 0, 8, {{-1}}},
        {"DIGI TIME (FREE ms)", G_DMS, 150, 450, {{G_DSYNC, 1}, {-1}}},
        {"DIGI FDBK", G_DFDBK, 30, 120, {{-1}}},
        {"DIGI COLR", G_DCOLOR, 0, 127, {{-1}}},
        {"DIGI MIX", G_DMIX, 0, 127, {{-1}}},
        {"DIGI PING on/off", G_DPING, 0, 1, {{-1}}},
        {"MODE DIGI/TAPE", G_DMODE, 0, 1, {{-1}}},
        {"TAPE TIME", G_DTIME, 0, 8, {{G_DMODE, 1}, {-1}}, 5.0},   /* (the tape's bend: the sine up to 1.75x its
                                                                 * pitch while the head speeds, ~3x the second difference) */
        {"TAPE FDBK", G_DFDBK, 30, 110, {{G_DMODE, 1}, {-1}}},
        {"TAPE DRIVE", G_DDRIVE, 0, 127, {{G_DMODE, 1}, {-1}}},
        {"TAPE WOW", G_DWOW, 0, 127, {{G_DMODE, 1}, {-1}}},
        {"TAPE PING on/off", G_DPING, 0, 1, {{G_DMODE, 1}, {-1}}},
        {"CHORUS DEPTH", G_CDEPTH, 0, 127, {{-1}}},
        {"CHORUS RATE", G_CRATE, 0, 127, {{-1}}},
        {"ROOM SIZE", G_RSIZE, 0, 127, {{-1}}},
        {"ROOM DAMP", G_RDAMP, 0, 127, {{-1}}},
        {"PLATE DECAY", G_RSIZE, 30, 127, {{G_RTYPE, 1}, {-1}}},
        {"PLATE DAMP", G_RDAMP, 0, 127, {{G_RTYPE, 1}, {-1}}},
        {"PLATE SIZE", G_RSZ, 0, 127, {{G_RTYPE, 1}, {-1}}},
        {"PLATE PRE", G_RPRE, 0, 100, {{G_RTYPE, 1}, {-1}}},
        {"PLATE MOD", G_RMOD, 0, 127, {{G_RTYPE, 1}, {-1}}},
        {"SPACE SIZE", G_RSZ, 0, 127, {{G_RTYPE, 2}, {-1}}},
        {"SPACE MOD", G_RMOD, 0, 127, {{G_RTYPE, 2}, {-1}}},
        {"REV TYPE", G_RTYPE, 0, 2, {{-1}}},
    };
    uint32_t k;
    puts("clicks when a knob turns (the largest jump of the return, turning / still; a sine in):");
    for (k = 0; k < sizeof K / sizeof K[0]; k++) {
        char wav[256] = "";
        double still = run(&K[k], 0, 0), turn, ratio;
        if (dir) {
            char *c;
            snprintf(wav, sizeof wav, "%s/click-%s.wav", dir, K[k].name);
            for (c = wav + strlen(dir) + 1; *c; c++)
                if (*c == ' ' || *c == '/' || *c == '(' || *c == ')') *c = '-';
        }
        turn = run(&K[k], 1, dir ? wav : 0);
        double lim = K[k].limit > 0 ? K[k].limit : LIMIT;
        ratio = turn / (still > 1.0 ? still : 1.0);
        printf("  %s  %-22s still %6.0f  turning %6.0f  x%.1f\n", ratio <= lim ? "ok  " : "FAIL", K[k].name, still, turn, ratio);
        if (ratio > lim)
            fails++;
    }
    printf("clicks: %u failed\n", fails);
    return fails != 0;
}

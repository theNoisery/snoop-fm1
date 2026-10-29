/* SPDX-License-Identifier: GPL-3.0-only */
/* SNOOP 0.4: the reverb's TYPEs (rev_plate.c: PLATE, SPACE; ROOM is SLOOP's, its golden renders in regress.c).
 * Renders demos into DIR (argv[1]) and checks: each TYPE's tail decays (and longer with DECAY), is wide (left and right
 * apart), has no DC, stays bounded under a loud held chord; the loudness against ROOM; SPACE at full DECAY still
 * sounds 20 s on; FREEZE holds; a SIZE sweep has no jump; a TYPE change is clean; TYPE / SIZE / PRE / MOD in a project;
 * the cost. */
#define FELUCCA_ARRANGER 1
#define main hostsim_main
#include "hostsim.c"
#undef main
#include <math.h>
#include <string.h>
#define PROJ_HOST 1
static uint32_t trk_def_engine(uint32_t i) { return i < NSYN ? i : 0; }
#include "../firmware/src/project.c"
static struct { uint8_t force; } ui;
static uint8_t sync_reload;
#include "../firmware/src/arranger_scene.c"

static uint32_t fails;
static void check(int ok, const char *what)
{
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        fails++;
}
static uint32_t preset_named(uint32_t e, const char *name)
{
    uint32_t i;
    for (i = 0; i < ENGINES[e]->npresets; i++)
        if (!strcmp(ENGINES[e]->presets[i].name, name))
            return i;
    return 0;
}

typedef struct {
    int type, decay, damp, size, pre, mod;
    double secs, hold;                      /* how long; how long the chord is held (s) */
    const char *wav;
    int sweep;                              /* SIZE swept 0 -> 127 -> 0 over the render */
    double freeze_at;                       /* > 0: FREEZE from then (the punch-in, as FX + its key) */
    int dry;                                /* the send at 0: record the dry */
} run_t;
static double rms[64], corr_tail, dc_late, wet_early, rev_rms[64];
static int32_t dryl[FS];                    /* the first second without the reverb (send 0): the dry, to take out */
static int32_t peak, max_jump;
static void render(const char *dir, const run_t *r)
{
    static const uint8_t CHORD[4] = {60, 64, 67, 71};
    char path[512];
    FILE *f = 0;
    int32_t out[CTL * 2], last = 0;
    uint32_t frames = (uint32_t)(r->secs * FS) / CTL * CTL, fr, k, on = 0;
    memset(rev_rms, 0, sizeof rev_rms);
    double sq[64] = {0}, sl = 0, sr = 0, slr = 0, sum_late = 0, we = 0;
    uint32_t cnt_late = 0;
    track_t *t = &trk[0];
    host_tracks_init();
    host_preset(t, 1, preset_named(1, "RHODES"));
    t->p[P_REV] = r->dry ? 0 : 127, t->p[P_DLY] = 0, t->p[P_CHOR] = 0;
    trk[1].p[P_LEVEL] = trk[2].p[P_LEVEL] = 0;
    song.g[G_DRLVL] = 0;
    song.g[G_DMIX] = 0;
    song.g[G_RTYPE] = (int16_t)r->type, song.g[G_RSIZE] = (int16_t)r->decay, song.g[G_RDAMP] = (int16_t)r->damp;
    song.g[G_RSZ] = (int16_t)r->size, song.g[G_RPRE] = (int16_t)r->pre, song.g[G_RMOD] = (int16_t)r->mod;
    memset(rev_line, 0, sizeof rev_line);
    memset(rev_ap, 0, sizeof rev_ap);
    fx.line_lp[0] = fx.line_lp[1] = fx.line_lp[2] = fx.line_lp[3] = 0;
    plt.inited = 0;
    rev_frz = 0;
    punch.req = -1;
    if (dir && r->wav) {
        snprintf(path, sizeof path, "%s/%s", dir, r->wav);
        if ((f = fopen(path, "wb")))
            wav_hdr(f, frames);
    }
    peak = 0, max_jump = 0;
    for (fr = 0; fr < frames; fr += CTL) {
        double s = (double)fr / FS;
        if (!on && s >= 0.1) {
            for (k = 0; k < 4u; k++)
                trk_note_on(t, CHORD[k], 115);
            on = 1;
        }
        if (on == 1 && s >= 0.1 + r->hold) {
            for (k = 0; k < 4u; k++)
                trk_note_off(t, CHORD[k]);
            on = 2;
        }
        if (r->sweep) {
            double p = s / r->secs;
            song.g[G_RSZ] = (int16_t)(127.0 * (p < 0.5 ? 2.0 * p : 2.0 - 2.0 * p));
        }
        if (r->freeze_at > 0 && s >= r->freeze_at)
            punch.req = PX_FREEZE;                      /* FX + its key held, as the FM-1 does */
        mix_block(out, CTL);
        for (k = 0; k < CTL; k++)
            if ((fr + k) / FS < 64u)
                rev_rms[(fr + k) / FS] += (double)rev_l[k] * rev_l[k];
        for (k = 0; k < CTL; k++) {
            int32_t l = out[2 * k], rr = out[2 * k + 1], a = l < 0 ? -l : l, j = l - last;
            uint32_t sec = (fr + k) / FS;
            last = l;
            if (a > peak) peak = a;
            if (s > 1.0 && (j < 0 ? -j : j) > max_jump) max_jump = j < 0 ? -j : j;
            if (sec < 64u) sq[sec] += (double)l * l;
            if (fr + k < FS) {
                if (r->dry) dryl[fr + k] = l;
                else if (s >= 0.1 && s < 0.6) we += (double)(l - dryl[fr + k]) * (l - dryl[fr + k]);
            }
            if (s >= 2.0 && s < 4.0) sl += (double)l * l, sr += (double)rr * rr, slr += (double)l * rr;
            if (fr + k >= frames - FS) sum_late += l, cnt_late++;
            if (f) wav_put(f, l, rr);
        }
    }
    if (f) fclose(f);
    for (k = 0; k < 64u; k++)
        rms[k] = sqrt(sq[k] / FS), rev_rms[k] = sqrt(rev_rms[k] / FS);
    wet_early = sqrt(we / (0.5 * FS));
    corr_tail = sl > 0 && sr > 0 ? slr / sqrt(sl * sr) : 1.0;
    dc_late = cnt_late ? sum_late / cnt_late : 0;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : 0;
    static const char *const NAME[3] = {"ROOM", "PLATE", "SPACE"};
    double room_wet = 0, t0, t1, cost[3];
    uint32_t ty;
    puts("reverb TYPEs (SNOOP):");
    render(0, &(run_t){0, 90, 60, 80, 10, 50, 1.0, 0.25, 0, 0, 0, 1});   /* the dry alone */
    for (ty = 0; ty < 3u; ty++) {                       /* the defaults: a chord stab, its tail */
        char wav[64];
        snprintf(wav, sizeof wav, "rev-%s-stab.wav", ty == 0 ? "room" : ty == 1 ? "plate" : "space");
        render(dir, &(run_t){(int)ty, 90, 60, 80, 10, 50, 8.0, 0.25, wav});
        printf("  %-5s the reverb's first 0.5 s %.0f; rms s1 %.0f s2 %.0f s4 %.0f s7 %.0f; L/R correlation of the tail %.2f\n",
               NAME[ty], wet_early, rms[1], rms[2], rms[4], rms[7], corr_tail);
        if (!ty)
            room_wet = wet_early;
        else {
            char what[128];
            snprintf(what, sizeof what, "%s: decays, wide (|corr| < 0.5), bounded, no DC", NAME[ty]);
            check(rms[7] < 0.5 * rms[1] && corr_tail < 0.5 && corr_tail > -0.5 && peak <= 32767 && dc_late < 200 &&
                  dc_late > -200, what);
            snprintf(what, sizeof what, "%s: as loud as ROOM in its first 0.5 s, within 3 dB (%.0f against %.0f)", NAME[ty],
                     wet_early, room_wet);
            check(wet_early > room_wet * 0.71 && wet_early < room_wet * 1.41, what);
        }
    }
    render(0, &(run_t){1, 30, 60, 80, 10, 50, 6.0, 0.25, 0});
    {
        double short_s3 = rms[3];
        render(0, &(run_t){1, 120, 60, 80, 10, 50, 6.0, 0.25, 0});
        check(rms[3] > 3.0 * short_s3, "PLATE: DECAY 120 rings much longer than DECAY 30");
    }
    render(dir, &(run_t){2, 127, 50, 127, 40, 100, 24.0, 0.25, "rev-space-max-decay.wav"});
    printf("  SPACE, full DECAY and SIZE: rms s1 %.0f, s10 %.0f, s22 %.0f\n", rms[1], rms[10], rms[22]);
    check(rms[22] > 0.1 * rms[1], "SPACE: full DECAY still sounds 20 s on (near infinite)");
    render(dir, &(run_t){2, 100, 40, 100, 30, 80, 8.0, 3.0, "rev-space-held-chord.wav"});
    check(peak <= 32767, "SPACE: a loud chord held 3 s into it: bounded");
    render(dir, &(run_t){2, 90, 50, 80, 10, 60, 12.0, 0.25, "rev-space-freeze.wav", 0, 0.5});
    printf("  FREEZE (the punch-in) from 0.5 s: the reverb's rms s2 %.0f, s11 %.0f\n", rev_rms[2], rev_rms[11]);
    check(rev_rms[11] > 0.7 * rev_rms[2] && rev_rms[11] < 1.41 * rev_rms[2], "FREEZE holds the tail (within 3 dB over 9 s)");
    render(0, &(run_t){0, 90, 50, 80, 10, 60, 12.0, 0.25, 0, 0, 0.5});
    printf("  (ROOM's FREEZE, the same: s2 %.0f, s11 %.0f)\n", rev_rms[2], rev_rms[11]);
    render(dir, &(run_t){2, 110, 40, 64, 0, 60, 12.0, 4.0, "rev-space-size-sweep.wav", 1});
    printf("  SIZE sweep: peak %d, the largest step %d\n", peak, max_jump);
    check(peak <= 32767, "a SIZE sweep while it rings: bounded (the heads move a sample a block)");
    {   /* a TYPE change while it rings: no burst (the lines cleared) */
        render(0, &(run_t){2, 120, 40, 100, 10, 60, 2.0, 0.25, 0});
        song.g[G_RTYPE] = 0;
        {
            int32_t out[CTL * 2];
            uint32_t b, k, big = 0;
            for (b = 0; b < 200u; b++) {
                mix_block(out, CTL);
                for (k = 0; k < CTL * 2u; k++)
                    if (out[k] > 20000 || out[k] < -20000)
                        big++;
            }
            check(!big, "SPACE -> ROOM while it rings: no burst (ROOM's lines start empty)");
        }
    }
    {   /* TYPE, SIZE, PRE, MOD in a project; a SLOOP project opens as ROOM */
        project_t p;
        song.g[G_RTYPE] = 2, song.g[G_RSZ] = 101, song.g[G_RPRE] = 77, song.g[G_RMOD] = 33;
        proj_capture(&p);
        song.g[G_RTYPE] = 0, song.g[G_RSZ] = 0, song.g[G_RPRE] = 0, song.g[G_RMOD] = 0;
        proj_apply(&p, 1);
        check(song.g[G_RTYPE] == 2 && song.g[G_RSZ] == 101 && song.g[G_RPRE] == 77 && song.g[G_RMOD] == 33,
              "a project keeps TYPE, SIZE, PRE, MOD");
        p.g[G_SYNC] = 0, p.g[G_MIDI] = 1;               /* as SLOOP saves them */
        proj_apply(&p, 1);
        check(song.g[G_RTYPE] == 0 && song.g[G_RSZ] == GP[G_RSZ].def, "a SLOOP project opens as ROOM");
        check(gdesc(G_RSIZE) == &GP[G_RSIZE], "ROOM: REV/CHO's SIZE is SIZE, as SLOOP");
        song.g[G_RTYPE] = 1;
        check(!strcmp(gdesc(G_RSIZE)->label, "DECAY"), "PLATE / SPACE: it reads DECAY");
        song.g[G_RTYPE] = 0;
    }
    for (ty = 0; ty < 3u; ty++) {                       /* the cost: 10 s, the chord held */
        t0 = (double)now_ns();
        render(0, &(run_t){(int)ty, 90, 60, 80, 10, 50, 10.0, 10.0, 0});
        t1 = (double)now_ns();
        cost[ty] = (t1 - t0) / (10.0 * FS);
    }
    printf("  cost (the whole mix, one track): ROOM %.0f, PLATE %.0f, SPACE %.0f ns a sample\n", cost[0], cost[1], cost[2]);
    printf("reverb: %u failed\n", fails);
    return fails != 0;
}

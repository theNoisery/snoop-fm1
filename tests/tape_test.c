/* SPDX-License-Identifier: GPL-3.0-only */
/* SNOOP: the TAPE delay (fx.c tape_*, FX > DLY 2). Renders demos into DIR (argv[1]) and checks:
 *  - FDBK 110 % self-oscillates: the echoes of one note still sound 10 s later, bounded, no DC;
 *  - FDBK 90 % dies away; FDBK 0 % gives a single echo;
 *  - a TIME change glides (no jump of the read head: no click), the output stays finite and bounded;
 *  - MODE, WOW and DRIVE go into a project and come back; a SLOOP project (no mark) opens as DIGI;
 *  - the cost of TAPE against DIGI (printed). */
#define FELUCCA_ARRANGER 1
#define main hostsim_main
#include "hostsim.c"
#undef main
#include <math.h>
#include <string.h>
#define PROJ_HOST 1
static uint32_t trk_def_engine(uint32_t i) { return i < NPART ? i : 0; }
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

/* a render: one track (KALIMBA, its delay send full, no reverb), notes at the given seconds (held 0.1 s), `secs`
 * long; the delay's settings as given. Fills rms[] with the RMS of each second (left), returns the peak */
typedef struct {
    int mode, fdbk, time, wow, drive, colr;
    double secs;
    const double *at;                       /* note times (s), < 0 ends */
    const int *time_at;                     /* optional: {second, TIME, second, TIME, ..., -1} */
    const char *wav;
    int ping, sync, ms;                     /* (0.3) PING, SYNC FREE, its ms */
    int dry;                                /* (0.3) MIX 0: the dry alone */
} run_t;
static int32_t outl[3 * FS], outr[3 * FS];  /* (0.3) the first 3 s */
static double rms[64], dc_late;
static int32_t peak, max_jump, head_step, head_glide_ms;
static int render(const char *dir, const run_t *r)
{
    char path[512];
    FILE *f = 0;
    int32_t out[CTL * 2], last = 0;
    uint32_t frames = (uint32_t)(r->secs * FS) / CTL * CTL, fr, k, n = 0, ti = 0;
    double sq[64] = {0}, sum_late = 0;
    uint32_t cnt_late = 0;
    track_t *t = &trk[0];
    host_tracks_init();
    host_preset(t, 1, preset_named(1, "KALIMBA"));
    t->p[P_DLY] = 127, t->p[P_REV] = 0, t->p[P_CHOR] = 0;
    trk[1].p[P_LEVEL] = trk[2].p[P_LEVEL] = 0;
    song.g[G_DRLVL] = 0;
    song.g[G_DMODE] = (int16_t)r->mode, song.g[G_DFDBK] = (int16_t)r->fdbk, song.g[G_DTIME] = (int16_t)r->time;
    song.g[G_DWOW] = (int16_t)r->wow, song.g[G_DDRIVE] = (int16_t)r->drive, song.g[G_DCOLOR] = (int16_t)r->colr;
    song.g[G_DMIX] = r->dry ? 0 : 110;
    song.g[G_DPING] = (int16_t)r->ping, song.g[G_DSYNC] = (int16_t)r->sync, song.g[G_DMS] = (int16_t)(r->ms ? r->ms : 375);
    memset(dly_buf, 0, sizeof dly_buf);
    fx.dly_lp = 0;
    tape.on = 0;
    if (dir && r->wav) {
        snprintf(path, sizeof path, "%s/%s", dir, r->wav);
        if ((f = fopen(path, "wb")))
            wav_hdr(f, frames);
    }
    peak = 0, max_jump = 0, head_step = 0, head_glide_ms = 0;
    for (fr = 0; fr < frames; fr += CTL) {
        double s = (double)fr / FS;
        if (r->at[n] >= 0 && s >= r->at[n]) {
            trk_note_on(t, n % 2u ? 69 : 64, 110);
            n++;
        }
        if (n && s >= r->at[n - 1] + 0.1)
            trk_note_off(t, (n - 1u) % 2u ? 69 : 64);
        if (r->time_at && r->time_at[ti] >= 0 && s >= r->time_at[ti]) {
            song.g[G_DTIME] = (int16_t)r->time_at[ti + 1];
            ti += 2;
        }
        {
            int32_t d0 = tape.d, st, was = tape.on;     /* (the first TAPE block puts the head at TIME) */
            mix_block(out, CTL);
            st = tape.d - d0;
            st = st < 0 ? -st : st;
            if (was && tape.on && st) {
                if (st > head_step) head_step = st;
                head_glide_ms += 1;               /* (blocks the head moved) */
            }
        }
        for (k = 0; k < CTL; k++) {
            int32_t l = out[2 * k], a = l < 0 ? -l : l, j = l - last;
            uint32_t sec = (fr + k) / FS;
            last = l;
            if (a > peak) peak = a;
            if (fr > FS && (j < 0 ? -j : j) > max_jump) max_jump = j < 0 ? -j : j;
            if (sec < 64u) sq[sec] += (double)l * l;
            if (fr + k >= frames - 2u * FS) sum_late += l, cnt_late++;
            if (fr + k < 3u * FS) outl[fr + k] = out[2 * k], outr[fr + k] = out[2 * k + 1];
            if (f) wav_put(f, out[2 * k], out[2 * k + 1]);
        }
    }
    if (f) fclose(f);
    for (k = 0; k < 64u; k++)
        rms[k] = sqrt(sq[k] / FS);
    dc_late = cnt_late ? sum_late / cnt_late : 0;
    return 0;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : 0;
    static const double ONE[] = {0.2, -1}, MELODY[] = {0.2, 0.55, 0.9, 1.6, 1.95, 2.3, -1};
    static const int SWEEP[] = {4, 0, 6, 2, 8, 4, 10, 8, 12, 1, -1};   /* 1/4 -> 1/16 -> 8T -> 1/4D -> 1/8 */
    double t0, t1;
    puts("tape delay (SNOOP):");

    /* 1. self-oscillation at 110 %: one note, still sounding (and bounded) 10 s on */
    render(dir, &(run_t){1, 110, 1, 30, 32, 70, 14.0, ONE, 0, "tape-selfosc-110.wav"});
    printf("  110 %%: rms s1 %.0f, s6 %.0f, s12 %.0f, peak %d, dc %.1f\n", rms[1], rms[6], rms[12], peak, dc_late);
    check(rms[12] > 1500.0 && rms[12] > 0.5 * rms[1], "FDBK 110 %: self-oscillates (the echoes grow to the tape's level and stay)");
    check(peak <= 32767 && rms[12] < 20000.0, "FDBK 110 %: bounded (the tape saturates, no runaway)");
    check(dc_late > -300.0 && dc_late < 300.0, "FDBK 110 %: no DC builds up in the loop");

    /* 2. 90 %: dies away; 0 %: one echo */
    render(dir, &(run_t){1, 90, 1, 30, 32, 70, 14.0, ONE, 0, "tape-fdbk-90.wav"});
    printf("  90 %%: rms s1 %.0f, s12 %.0f\n", rms[1], rms[12]);
    check(rms[12] < 0.2 * rms[1], "FDBK 90 %: the echoes die away");
    render(dir, &(run_t){1, 0, 1, 30, 32, 70, 4.0, ONE, 0, 0});
    check(rms[3] < 5.0, "FDBK 0 %: one echo, then silence");

    /* 3. the echoes of a melody, a long tail (the demo to listen to) */
    render(dir, &(run_t){1, 75, 6, 55, 50, 60, 10.0, MELODY, 0, "tape-echoes-wow.wav"});
    check(peak <= 32767 && rms[8] > 10.0, "FDBK 75 %, WOW 55, DRIVE 50: echoes, wow and flutter");
    render(dir, &(run_t){0, 120, 6, 0, 0, 60, 10.0, MELODY, 0, "digi-echoes-same-melody.wav"});

    /* 4. TIME changed while it self-oscillates: the pitch bends (tape speed), no click */
    render(dir, &(run_t){1, 104, 0, 25, 40, 75, 15.0, MELODY, SWEEP, "tape-time-sweeps.wav"});
    head_glide_ms = head_glide_ms * CTL * 1000 / FS;
    printf("  sweeps: peak %d; the read head moved at most %.2f samples a sample, gliding %d ms in all\n", peak,
           head_step / 256.0 / CTL, head_glide_ms);
    check(peak <= 32767 && head_step <= 192 * CTL, "TIME changes glide: the head moves at most 3/4 of a sample a sample (no jump, no click)");
    check(head_glide_ms > 4 * 150, "TIME changes glide: each of the 4 changes bends the pitch for a while (> 150 ms)");

    /* 5. MODE, WOW, DRIVE in a project; a SLOOP project opens as DIGI */
    {
        project_t p;
        song.g[G_DMODE] = 1, song.g[G_DWOW] = 99, song.g[G_DDRIVE] = 7;
        proj_capture(&p);
        song.g[G_DMODE] = 0, song.g[G_DWOW] = 0, song.g[G_DDRIVE] = 0;
        proj_apply(&p, 1);
        check(song.g[G_DMODE] == 1 && song.g[G_DWOW] == 99 && song.g[G_DDRIVE] == 7, "a project keeps MODE TAPE, WOW, DRIVE");
        song.g[G_DMODE] = 0;
        proj_apply(&p, 0);
        check(song.g[G_DMODE] == 0, "a song section leaves the delay as it is (as SLOOP's TIME / FDBK)");
        p.g[G_LOAD] = 0, p.g[G_SLOT] = 2;              /* as SLOOP saves them */
        song.g[G_DMODE] = 1;
        proj_apply(&p, 1);
        check(song.g[G_DMODE] == 0 && song.g[G_DWOW] == GP[G_DWOW].def, "a SLOOP project opens as DIGI");
    }

    /* 7. (0.3) PING and FREE: one note at 0.2 s (block 276: sample 8832), FREE 250 ms: the first echo on the left
     * 250 ms later, the second on the right 500 ms later; then TAPE 110 % with a long TIME, bounded */
    {
        const uint32_t note = 8832u, T = 250u * 441u / 10u;
        static int32_t dl[3 * FS], dr[3 * FS];
        uint32_t k, onl = 0, onr = 0, l_first = 0, r_first = 0;
        render(0, &(run_t){0, 90, 1, 0, 0, 80, 3.0, ONE, 0, 0, 1, 1, 250, 1});   /* the dry alone */
        memcpy(dl, outl, sizeof dl), memcpy(dr, outr, sizeof dr);
        render(dir, &(run_t){0, 90, 1, 0, 0, 80, 6.0, ONE, 0, "digi-ping-free-250ms.wav", 1, 1, 250});
        for (k = note; k < 3u * FS; k++) {              /* each side's echoes: the output less the dry */
            int32_t el = outl[k] - dl[k], er = outr[k] - dr[k];
            if (!onl && (el > 300 || el < -300)) onl = k, l_first = er > -300 && er < 300;
            if (!onr && (er > 300 || er < -300)) onr = k;
        }
        printf("  ping, FREE 250 ms: left echo at +%d samples, right at +%d (expected +%u, +%u)\n", (int)(onl - note),
               (int)(onr - note), T, 2u * T);
        check(onl >= note + T && onl < note + T + 200u && l_first, "FREE 250 ms: the first echo 250 ms after the note, left only");
        check(onr >= note + 2u * T && onr < note + 2u * T + 200u, "PING: the second echo 500 ms after it, on the right");
        render(dir, &(run_t){1, 110, 8, 40, 60, 70, 10.0, MELODY, 0, "tape-ping-110.wav", 1, 0, 0});
        check(peak <= 32767 && rms[9] > 1000.0, "PING + TAPE 110 %, TIME 1/4D (clamped to half the buffer): self-oscillates, bounded");
        render(dir, &(run_t){0, 80, 1, 0, 0, 80, 3.0, MELODY, 0, "digi-free-12ms-comb.wav", 0, 1, 12});
        check(peak <= 32767, "FREE 12 ms, FDBK 80 %: a comb, bounded");
    }
    {   /* (0.3) PING, SYNC, MS in a project; a 0.2 project (MS bits 0) opens with 375 ms; the DLY page's TIME is the ms
         * when FREE */
        project_t p;
        const page_t *pg = 0;
        int16_t *vp = 0;
        uint32_t k;
        song.g[G_DPING] = 1, song.g[G_DSYNC] = 1, song.g[G_DMS] = 1234, song.g[G_DMODE] = 1, song.g[G_DWOW] = 5;
        proj_capture(&p);
        song.g[G_DPING] = 0, song.g[G_DSYNC] = 0, song.g[G_DMS] = 1;
        proj_apply(&p, 1);
        check(song.g[G_DPING] == 1 && song.g[G_DSYNC] == 1 && song.g[G_DMS] == 1234 && song.g[G_DMODE] == 1 &&
              song.g[G_DWOW] == 5, "a project keeps PING, SYNC FREE and its ms (with MODE, WOW)");
        p.g[G_LOAD] = (int16_t)(p.g[G_LOAD] & 0x40FF), p.g[G_SLOT] = (int16_t)(p.g[G_SLOT] & 0x407F);   /* as 0.2 saved */
        proj_apply(&p, 1);
        check(song.g[G_DMS] == 375 && !song.g[G_DPING] && !song.g[G_DSYNC] && song.g[G_DWOW] == 5,
              "a SNOOP 0.2 project: PING off, SYNC, 375 ms; its MODE / WOW kept");
        for (k = 0; k < NPAGES; k++)
            if (!strcmp(PAGES[k].title, "DLY"))
                pg = &PAGES[k];
        song.g[G_DSYNC] = 1;
        page_desc(pg, 0, &vp);
        check(vp == &song.g[G_DMS], "FREE: the DLY page's TIME knob sets the ms");
        song.g[G_DSYNC] = 0;
        page_desc(pg, 0, &vp);
        check(vp == &song.g[G_DTIME], "SYNC: it sets the note value, as SLOOP");
    }

    /* 6. the cost: 20 s of the self-oscillating loop, DIGI vs TAPE (one track, so the delay weighs) */
    t0 = (double)now_ns();
    render(0, &(run_t){0, 120, 1, 30, 32, 70, 20.0, ONE, 0, 0});
    t1 = (double)now_ns();
    {
        double digi = t1 - t0;
        t0 = (double)now_ns();
        render(0, &(run_t){1, 110, 1, 30, 32, 70, 20.0, ONE, 0, 0});
        t1 = (double)now_ns();
        printf("  cost: TAPE %.0f ns a sample against DIGI %.0f (the whole mix, one track playing)\n",
               (t1 - t0) / (20.0 * FS), digi / (20.0 * FS));
    }
    printf("tape: %u failed\n", fails);
    return fails != 0;
}

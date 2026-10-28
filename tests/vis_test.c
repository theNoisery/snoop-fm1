/* SPDX-License-Identifier: GPL-3.0-only */
/* SNOOP 0.9: the visualiser (ui_vis.c) on the ui_pages_test harness: a groove on the four tracks, each style shown
 * for 4 s, every drawn frame saved (DIR/vis-<style>-NNN.ppm, for a GIF) when a DIR is given. Checks:
 *   - every style draws, and moves (frames differ) while the groove plays;
 *   - HEXAGON: a ball for each track playing, kept inside the hexagon;
 *   - TRACKS: a track's LEVEL shown, its needle following what it plays;
 *   - the draw time of a frame per style, against the TRACKS screen's (host time: the device is slower, the ratio
 *     is what counts) */
#define UI_PAGES_HARNESS_ONLY 1
#include "ui_pages_test.c"
#include <time.h>

static double vt_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }
static void groove(void)
{
    static const uint8_t BASS[2] = {38, 34};
    uint32_t j, k;
    for (j = 0; j < NTRK; j++) { steps_clear(&trk[j]); trk[j].p[P_SLEN] = 16; }
    for (j = 0; j < 16u; j++) {
        step_t *sp;
        if (!(j & 1u)) { sp = &trk[0].step[j]; sp->n = 1; sp->note[0] = (uint8_t)(BASS[j / 8u] + ((j & 2u) ? 12 : 0)); sp->time = ST_NOTE; sp->vel = 100; }
        if ((j % 4u) == 2u) { sp = &trk[1].step[j]; sp->n = 3; sp->note[0] = 65; sp->note[1] = 69; sp->note[2] = 72; sp->time = ST_NOTE; sp->vel = 100; }
        if (!(j % 8u)) { sp = &trk[2].step[j]; sp->n = 2; sp->note[0] = 57; sp->note[1] = 64; sp->time = ST_NOTE; sp->vel = 100; }
        memset(&TDRUM->dstep[j], 0, sizeof(dstep_t));
        if (!(j % 4u)) dstep_set(&TDRUM->dstep[j], 0, LV_NORM, 0);
        if ((j % 8u) == 4u) dstep_set(&TDRUM->dstep[j], 3, LV_NORM, 0);
        if (j & 1u) dstep_set(&TDRUM->dstep[j], 4, LV_SOFT, 0);
    }
    for (k = 0; k < 4u; k++) trk[k].seq_active = 1;
    trk[1].p[P_PAN] = -40, trk[2].p[P_PAN] = 30;
    trk[2].p[P_LEVEL] = 80;
}
static uint32_t lit(void)
{
    uint32_t n = 0, x;
    for (x = 0; x < 240u * 240u; x++)
        n += screen[x] != swap16(OP_BG);
    return n;
}

int main(int argc, char **argv)
{
    static uint16_t first[240 * 240];
    uint32_t st, i, f;
    double base = 0, t[VIS_N];
    const char *dir = argc > 1 ? argv[1] : 0;
    outdir = dir ? dir : "build/host";
    panel = PANEL_DEFAULT;
    layers_init();
    settings.palette = 4;
    palette_set(4);
    host_tracks_init();
    for (i = 0; i < NPART; i++) { set_engine_of(&trk[i], TRK_DEF[i][0]); apply_preset_to(&trk[i], TRK_DEF[i][1]); trk[i].engine = trk[i].eng_req; }
    TDRUM->p[P_E0] = DRUM_DEFAULT_KIT;
    song.sel = 0;
    go_home(); frames(4);
    groove();
    transport_req = 1; frames(30);
    {   /* the TRACKS screen's draw time: the reference */
        double t0 = vt_ns();
        for (f = 0; f < 60u; f++) { ui.force = 1; ui_draw(); }
        base = (vt_ns() - t0) / 60.0;
    }
    tap(B_HOME); frames(2);
    check(vis_shown(), "visualiser open");
    for (st = 0; st < VIS_N; st++) {
        uint32_t moved = 0, n = 0;
        double t0, sum = 0;
        vis_style = (uint8_t)st;
        vs.bounces = 0;
        vis_name_t = 0;
        ui.force = 1; frames(40);
        memcpy(first, screen, sizeof first);
        for (f = 0; f < 240u; f++) {                /* ~4 s */
            uint32_t q;
            static int32_t o[CTL * 2];
            for (q = 0; q < 22u; q++) {             /* (as frame(), the draw timed apart) */
                uint32_t k;
                mix_block(o, CTL);
                for (k = 1; k < CTL; k += 2u) {
                    scope_bufr[scope_w & (SCOPE_N - 1u)] = vis_tap[2u * k + 1u];
                    scope_buf[scope_w++ & (SCOPE_N - 1u)] = vis_tap[2u * k];
                }
            }
            ui_input(); ui_leds();
            t0 = vt_ns();
            ui_draw();
            if (!(ui.frame & 1u) || 1) sum += vt_ns() - t0;
            fm1_ms += 16;
            if (f & 1u) {
                char nm[64];
                n++;
                moved += memcmp(first, screen, sizeof first) != 0;
                if (dir && f < 180u) {
                    snprintf(nm, sizeof nm, "anim-%u-%03u", st + 1u, f / 2u);
                    ppm(nm);
                }
            }
            if (st == 4u) {                         /* HEXAGON: the balls inside it */
                uint32_t k, out = 0;
                for (k = 0; k < 4u; k++)
                    if (vs.alive[k]) {
                        int32_t dx = (vs.bx[k] - HX_CX) >> 8, dy = (vs.by[k] - HX_CY) >> 8;
                        out += dx * dx + dy * dy > (hx_r() >> 8) * (hx_r() >> 8);
                    }
                if (out) { check(0, "HEXAGON: a ball outside the hexagon"); break; }
            }
        }
        t[st] = sum / 120.0;                        /* (a frame drawn every other one) */
        {
            char nm[80];
            snprintf(nm, sizeof nm, "%-10s draws (%u px lit) and moves (%u of %u frames differ)", VIS_NAME[st], lit(), moved, n);
            check(lit() > 400u && moved > n / 2u, nm);
        }
        if (st == 4u) {
            uint32_t k, alive = 0;
            for (k = 0; k < 4u; k++) alive += vs.alive[k] != 0;
            check(alive == 4u, "HEXAGON: a ball for each of the four tracks playing");
            printf("  HEXAGON: %u bounces off the walls in 4 s\n", vs.bounces);
            check(vs.bounces > 20u, "HEXAGON: the balls reach the walls and bounce off them");
        }
        if (st == 5u) {                             /* TAPE: the red dot follows the MASTER knob, not the music */
            static uint16_t a[240 * 240];
            uint32_t m0 = song.master_q12, y, ya = 0, yb = 0;
            song.master_q12 = 4096u; ui.force = 1; frames(4); memcpy(a, screen, sizeof a);
            for (y = 20; y < 150u; y++) if (swap16(a[y * 240u + 232u]) == OP_RED && !ya) ya = y;
            song.master_q12 = 1024u; ui.force = 1; frames(4);
            for (y = 20; y < 150u; y++) if (swap16(screen[y * 240u + 232u]) == OP_RED && !yb) yb = y;
            printf("  TAPE: the master's dot at row %u (knob full), %u (knob half)\n", ya, yb);
            check(ya && yb > ya + 40u && yb < ya + 70u, "TAPE: the red slider shows the MASTER knob");
            song.master_q12 = m0;
        }
        if (st == 8u)
            check(vs.sm[0] > 300 && vs.sm[2] > 0, "TRACKS: the needles follow what the tracks play");
    }
    printf("draw time a frame (host), against the TRACKS screen's %.0f us:", base / 1000);
    for (st = 0; st < VIS_N; st++)
        printf(" %s %.0f us (x%.1f)%s", VIS_NAME[st], t[st] / 1000, t[st] / (base > 1 ? base : 1), st + 1u < VIS_N ? "," : "\n");
    transport_req = 2; frames(4);
    printf("vis: %d failed\n", fails);
    return fails != 0;
}

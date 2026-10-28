/* SPDX-License-Identifier: GPL-3.0-only */
/* SNOOP 0.6: a flash write (autosave, SAVE) stops the audio ~50 ms (the code runs from that flash). Checks:
 *  - the hush (fx.c audio_hush, around every st_save): the output reaches exact silence within ~3 ms, and comes back
 *    without a jump;
 *  - the output's silence (out_quiet_n, project.c audio_quiet): a delay tail still ringing after the voices ended
 *    is not quiet; once it has died away it is. */
#define main hostsim_main
#include "hostsim.c"
#undef main
#include <math.h>
#include <string.h>
#define QUIET_BLOCKS (FS * 16u / 10u / CTL)    /* (as project.c) */

static uint32_t fails;
static void check(int ok, const char *what)
{
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        fails++;
}

int main(void)
{
    int32_t out[CTL * 2], p1 = 0, p2 = 0, worst_back = 0, worst_still = 0;
    uint32_t b, k, zero_at = 0, voices_off_at = 0, quiet_at = 0;
    track_t *t = &trk[0];
    puts("the hush around a flash write, and the output's silence (SNOOP):");
    host_tracks_init();
    host_preset(t, 0, 5);                               /* a pad: a sustained, smooth sound */
    t->p[P_DLY] = 0, t->p[P_REV] = 0, t->p[P_CHOR] = 0;
    trk_note_on(t, 57, 100);
    for (b = 0; b < 400u; b++) {                        /* the pad going: the jumps it makes by itself */
        mix_block(out, CTL);
        for (k = 0; k < CTL; k++) {
            int32_t d2 = out[2 * k] - 2 * p1 + p2;
            if (b > 100u && (d2 < 0 ? -d2 : d2) > worst_still) worst_still = d2 < 0 ? -d2 : d2;
            p2 = p1, p1 = out[2 * k];
        }
    }
    hush_req = 1;                                       /* a save begins (audio_hush(1), the ISR's side) */
    for (b = 0; b < 20u && !hush_done; b++)
        mix_block(out, CTL);
    zero_at = b;
    check(hush_done && zero_at <= 5u, "the hush: silent within 5 blocks (3.6 ms), hush_done set");
    for (b = 0; b < 3u; b++) {                          /* (the write: the ISR may run or not; silent) */
        mix_block(out, CTL);
        for (k = 0; k < CTL * 2u; k++)
            if (out[k])
                zero_at = 99;
    }
    check(zero_at <= 5u, "held: still silent while the write runs");
    hush_req = 0;                                       /* the save ended (audio_hush(0)) */
    p1 = p2 = 0;
    for (b = 0; b < 40u; b++) {
        mix_block(out, CTL);
        for (k = 0; k < CTL; k++) {
            int32_t d2 = out[2 * k] - 2 * p1 + p2;
            if ((b || k > 1u) && (d2 < 0 ? -d2 : d2) > worst_back) worst_back = d2 < 0 ? -d2 : d2;
            p2 = p1, p1 = out[2 * k];
        }
    }
    printf("  the pad's own largest step %d, coming back %d\n", worst_still, worst_back);
    check(worst_back <= 2 * worst_still + 8, "back in without a jump (no larger than the pad's own steps x2)");
    trk_note_off(t, 57);

    /* a delay tail after the voices: not quiet until it has died away */
    host_tracks_init();
    host_preset(t, 0, 5);
    t->p[P_DLY] = 127, t->p[P_REV] = 0, t->p[P_CHOR] = 0;
    song.g[G_DFDBK] = 60, song.g[G_DMIX] = 120;
    song.g[G_DSYNC] = 1, song.g[G_DMS] = 1400;          /* a long time: long silences between the echoes */
    out_quiet_n = 0;
    trk_note_on(t, 57, 110);
    for (b = 0; b < 30000u && !quiet_at; b++) {         /* up to ~22 s */
        uint32_t v, any = 0;
        if (b == 100u) trk_note_off(t, 57);
        mix_block(out, CTL);
        for (v = 0; v < NVOICE; v++) any |= t->v[v].active;
        if (!any && !voices_off_at && b > 100u) voices_off_at = b;
        if (voices_off_at && out_quiet_n >= QUIET_BLOCKS) quiet_at = b;
    }
    printf("  voices off after %.2f s; the output quiet (with the delay's tail) after %.2f s\n",
           voices_off_at * (double)CTL / FS, quiet_at * (double)CTL / FS);
    check(voices_off_at && quiet_at > voices_off_at + 1000u, "a delay tail ringing on: not quiet (SLOOP looked at the voices only)");
    check(quiet_at > 0, "once it has died away: quiet");
    printf("hush: %u failed\n", fails);
    return fails != 0;
}

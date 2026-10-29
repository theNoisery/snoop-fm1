/* SPDX-License-Identifier: GPL-3.0-only */
/* SNOOP: STRETCH (eng_stretch.c). Renders demos into DIR (argv[1]) and checks:
 *  - a chord held 10 s: it sounds all along (the playhead moves on, the grains overlap), bounded;
 *  - the playhead: at 1x it moves as the source plays (~22050 source samples a second), FREEZ not at all;
 *  - LIVE: a part on LIV1 halves the delay's ring (32768) and records track 1; a STRETCH note holds the recording
 *    (the write index stands still while it sounds) and plays it; once the voice ends it records again; back to a
 *    sample set the delay has its whole buffer again;
 *  - LIVE from the drums;
 *  - the cost against GRAIN's. */
#define main hostsim_main
#include "hostsim.c"
#undef main
#include <math.h>
#include <string.h>

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

static FILE *wav;
static double sec_rms[64];
static int32_t peak;
static void open_wav(const char *dir, const char *name, double secs)
{
    char path[512];
    wav = 0;
    if (!dir) return;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    if ((wav = fopen(path, "wb")))
        wav_hdr(wav, (uint32_t)(secs * FS) / CTL * CTL);
}
static uint32_t frame;                      /* (into the current render) */
static void blocks(double secs)
{
    int32_t out[CTL * 2];
    uint32_t b, k, nb = (uint32_t)(secs * FS) / CTL;
    static double sq[64];
    if (!frame) memset(sq, 0, sizeof sq), peak = 0;
    for (b = 0; b < nb; b++, frame += CTL) {
        mix_block(out, CTL);
        for (k = 0; k < CTL; k++) {
            int32_t a = out[2 * k] < 0 ? -out[2 * k] : out[2 * k];
            if (a > peak) peak = a;
            if ((frame + k) / FS < 64u) sq[(frame + k) / FS] += (double)out[2 * k] * out[2 * k];
            if (wav) wav_put(wav, out[2 * k], out[2 * k + 1]);
        }
    }
    for (k = 0; k < 64u; k++)
        sec_rms[k] = sqrt(sq[k] / FS);
}
static void start(void)
{
    host_tracks_init();
    memset(dly_buf, 0, sizeof dly_buf);
    frame = 0;
    wav = 0;                                        /* (the last render's file is closed: none for this one yet) */
    trk[0].p[P_LEVEL] = trk[1].p[P_LEVEL] = trk[2].p[P_LEVEL] = 100;
    song.g[G_DRLVL] = 0;
    song.g[G_RTYPE] = 2;                            /* SPACE: as one would play it */
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : 0;
    const uint32_t ES = ENGI_STRETCH;
    static const uint8_t CHORD[3] = {60, 64, 67};
    uint32_t k;
    puts("STRETCH (SNOOP):");

    /* 1. a piano chord held, stretched 50x */
    start();
    host_preset(&trk[0], ES, preset_named(ES, "PNO STRETCH"));
    open_wav(dir, "stretch-pno-chord.wav", 12.0);
    for (k = 0; k < 3u; k++) trk_note_on(&trk[0], CHORD[k], 110);
    blocks(10.0);
    for (k = 0; k < 3u; k++) trk_note_off(&trk[0], CHORD[k]);
    blocks(2.0);
    if (wav) fclose(wav);
    printf("  PNO STRETCH: rms s1 %.0f s5 %.0f s9 %.0f, peak %d\n", sec_rms[1], sec_rms[5], sec_rms[9], peak);
    check(sec_rms[9] > 0.3 * sec_rms[2] && sec_rms[2] > 300.0 && peak <= 32767, "a chord held 10 s: it sounds all along, bounded");

    start();
    host_preset(&trk[0], ES, preset_named(ES, "STR ETHER"));
    open_wav(dir, "stretch-strings.wav", 12.0);
    for (k = 0; k < 3u; k++) trk_note_on(&trk[0], CHORD[k] - 12u, 110);
    blocks(10.0);
    for (k = 0; k < 3u; k++) trk_note_off(&trk[0], CHORD[k] - 12u);
    blocks(2.0);
    if (wav) fclose(wav);
    start();
    host_preset(&trk[0], ES, preset_named(ES, "PNO FROZEN"));
    open_wav(dir, "stretch-pno-frozen.wav", 10.0);
    trk_note_on(&trk[0], 60, 110);
    blocks(8.0);
    trk_note_off(&trk[0], 60);
    blocks(2.0);
    if (wav) fclose(wav);

    /* 2. the playhead: 1x and FREEZ */
    {
        uint32_t h0, h1;
        start();
        host_preset(&trk[0], ES, preset_named(ES, "PNO STRETCH"));
        trk[0].p[P_E2] = 15, trk[0].p[P_E1] = 0;        /* 1x, from the start */
        trk_note_on(&trk[0], 60, 100);
        blocks(0.05);
        h0 = (uint32_t)trk[0].v[0].s[3] >> 12;
        blocks(0.5);
        h1 = (uint32_t)trk[0].v[0].s[3] >> 12;
        printf("  1x: the playhead moved %u source samples in 0.5 s (22.05 kHz: 11025)\n", h1 - h0);
        check(h1 - h0 > 10500u && h1 - h0 < 11600u, "SPED 1x: the playhead moves as the source plays");
        trk[0].p[P_E2] = 0;
        h0 = (uint32_t)trk[0].v[0].s[3];
        blocks(0.5);
        check((uint32_t)trk[0].v[0].s[3] == h0, "SPED FREEZ: it stands still");
        trk_note_off(&trk[0], 60);
    }

    /* 3. LIVE: track 1 a Rhodes phrase, track 2 STRETCH on LIV1 */
    {
        static const uint8_t PH[8] = {60, 64, 67, 72, 71, 67, 64, 62};
        uint32_t w0, w1, ring_live, ring_back;
        start();
        host_preset(&trk[0], 1, preset_named(1, "RHODES"));
        host_preset(&trk[1], ES, preset_named(ES, "LIVE STRETCH"));
        trk[0].p[P_REV] = 30;
        open_wav(dir, "live-rhodes-then-stretch.wav", 13.0);
        blocks(0.1);
        ring_live = dly_ring;
        for (k = 0; k < 8u; k++) {                      /* the phrase, 1.6 s */
            trk_note_on(&trk[0], PH[k], 105);
            blocks(0.18);
            trk_note_off(&trk[0], PH[k]);
            blocks(0.02);
        }
        blocks(0.1);
        w0 = live_w;
        trk_note_on(&trk[1], 60, 110);                  /* stretch what was just played */
        blocks(9.0);
        w1 = live_w;
        printf("  LIVE: the ring %u while LIV1 is chosen; held: write index %u -> %u; rms while stretched s4 %.0f s9 %.0f\n",
               ring_live, w0, w1, sec_rms[4], sec_rms[9]);
        check(ring_live == DLY_LEN / 2u && live_on, "a part on LIV1: the delay on half its buffer, the ring recording");
        check(w0 == w1, "a STRETCH note holds the recording (never records itself)");
        check(sec_rms[5] > 300.0 && sec_rms[9] > 300.0 && peak <= 32767, "it plays the phrase stretched, all along, bounded");
        trk_note_off(&trk[1], 60);
        blocks(3.0);
        if (wav) fclose(wav);
        check(live_w != w1, "the voice ended: it records again");
        trk[1].p[P_E0] = 0;                             /* back to PIANO */
        blocks(0.1);
        ring_back = dly_ring;
        check(ring_back == DLY_LEN && !live_on, "back to a sample set: the delay has its whole buffer again");
    }

    /* 4. LIVE from the drums */
    {
        uint32_t b;
        start();
        song.g[G_DRLVL] = 100;
        TDRUM->p[P_E0] = DRUM_DEFAULT_KIT;
        host_preset(&trk[1], ES, preset_named(ES, "LIVE FREEZE"));
        open_wav(dir, "live-drums-then-freeze.wav", 10.0);
        blocks(0.1);
        for (b = 0; b < 8u; b++) {                      /* a bar of 8ths: kick, hat, snare, hat */
            drum_on(b % 4u == 0u ? 36u : b % 4u == 2u ? 38u : 42u, 110);
            blocks(0.2);
        }
        trk_note_on(&trk[1], 60, 110);
        blocks(7.0);
        printf("  LIVD: rms s3 %.0f s7 %.0f\n", sec_rms[3], sec_rms[7]);
        check(sec_rms[3] > 200.0 && sec_rms[7] > 200.0 && peak <= 32767, "LIVD: the drums recorded, frozen, held");
        trk_note_off(&trk[1], 60);
        blocks(1.6);
        if (wav) fclose(wav);
    }

    /* 4b. (SNOOP 0.10.1) high notes: the grains cut short (the note reads the sample faster than SIZE lasts) still
     * overlap DENS deep; and a new note while another is released gets its grains (the held one first) */
    {
        uint32_t b, i, c, ok84 = 0, nb = 0, newg = 0, vnew = NVOICE;
        start();
        host_preset(&trk[0], ES, preset_named(ES, "PNO STRETCH"));
        trk_note_on(&trk[0], 84, 110);
        for (b = 0; b < 2000u; b++) {
            st_part_t *P = (st_part_t *)eng_arena_of(&trk[0], ES);
            blocks((CTL + 0.5) / FS);           /* (one block: half a sample over, never rounded to 0) */
            if (b < 600u) continue;
            for (c = 0, i = 0; i < ST_NG; i++) c += P->g[i].owner != 0;
            ok84 += c >= 5u, nb++;
        }
        printf("  note 84 (grains cut short): 5 or more grains at once %u %% of the time (DENS 6)\n", ok84 * 100u / nb);
        check(ok84 * 10u >= nb * 8u, "a high note: its short grains overlap DENS deep (was 3..4 of 6: choppy)");
        trk_note_off(&trk[0], 84);
        start();
        host_preset(&trk[0], ES, preset_named(ES, "PNO STRETCH"));
        trk_note_on(&trk[0], 48, 110);
        blocks(2.0);
        trk_note_off(&trk[0], 48);                      /* (its long release goes on) */
        trk_note_on(&trk[0], 91, 110);
        for (i = 0; i < NVOICE; i++) if (trk[0].v[i].active && trk[0].v[i].gate) vnew = i;
        for (b = 0; b < 1400u; b++) {                   /* ~1 s */
            st_part_t *P = (st_part_t *)eng_arena_of(&trk[0], ES);
            blocks((CTL + 0.5) / FS);           /* (one block: half a sample over, never rounded to 0) */
            for (c = 0, i = 0; i < ST_NG; i++) c += P->g[i].owner == vnew + 1u;
            if (b >= 700u && c > newg) newg = c;
        }
        printf("  48 released, 91 played: the new note's grains after 0.5 s: up to %u\n", newg);
        check(vnew < NVOICE && newg >= 4u, "a new note over a released one: its grains first (was 2..3: thin, slow)");
        trk_note_off(&trk[0], 91);
    }

    /* 5. the cost: GRAIN and STRETCH, a chord of three */
    {
        double t0, t1, cg, cs;
        start();
        host_preset(&trk[0], ENGI_GRAIN, 0);
        for (k = 0; k < 3u; k++) trk_note_on(&trk[0], CHORD[k], 100);
        t0 = (double)now_ns(); blocks(5.0); t1 = (double)now_ns();
        cg = (t1 - t0) / (5.0 * FS);
        start();
        host_preset(&trk[0], ES, 0);
        for (k = 0; k < 3u; k++) trk_note_on(&trk[0], CHORD[k], 100);
        t0 = (double)now_ns(); blocks(5.0); t1 = (double)now_ns();
        cs = (t1 - t0) / (5.0 * FS);
        printf("  cost (the whole mix, a chord of three): GRAIN %.0f, STRETCH %.0f ns a sample\n", cg, cs);
    }
    printf("stretch: %u failed\n", fails);
    return fails != 0;
}

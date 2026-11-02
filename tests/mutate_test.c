/* SPDX-License-Identifier: GPL-3.0-only */
/* SNOOP 0.11: MUTATE (seq.c amut, after Arturia's KeyStep): ARP held + OCT+ held changes the arpeggio, the longer
 * the more; ARP + OCT- takes it back; both: none; KNOB 2 on the ARP layer too. On the ui_pages_test harness (the real
 * UI, the audio between the frames). Checks:
 *   - MUTATE at 0: the arp as ever (the same notes as before it was touched);
 *   - OCT+ held: a change at the press and one every 110 ms held, on the pattern as it is (0.11.2: as the KeyStep's, no
 *     end to it); held again: on from there; the ARP layer shows how much changed (its bar); rests, other notes, octaves;
 *   - a new chord: the changes stay (the amount does not move);
 *   - OCT- / KNOB 2 left: the changes taken back, the last first, to the plain arp note for note; both: at once;
 *   - the drum track: ARP + OCT+ is its hard roll as ever, nothing mutated.
 * Screens: DIR/mutate-*.ppm */
#define UI_PAGES_HARNESS_ONLY 1
#include "ui_pages_test.c"

#define NREC 64u
/* the arp's next NREC steps: the note each step starts (0: a rest), from the arp's step count */
static void arp_record(track_t *t, uint8_t *out)
{
    uint32_t n = 0, idx = t->arp_idx, guard = 0;
    while (n < NREC && guard++ < 4000u) {
        frame();
        while (t->arp_idx != idx && n < NREC) {
            idx++;
            out[n++] = t->arp_idx == idx ? t->arp_note : 0u;
        }
    }
}
static uint32_t differ(const uint8_t *a, const uint8_t *b)
{
    uint32_t i, d = 0;
    for (i = 0; i < NREC; i++) d += a[i] != b[i];
    return d;
}
/* how many of the steps differ from the plain arp (it repeats every 6: at its best phase) */
static uint32_t vs_plain(const uint8_t *m, const uint8_t *plain)
{
    uint32_t best = NREC, sh, i;
    for (sh = 0; sh < 6u; sh++) {
        uint32_t d = 0;
        for (i = 0; i < NREC; i++) d += m[i] != plain[(i + sh) % 6u];
        if (d < best) best = d;
    }
    return best;
}
static void hold_keys(uint32_t mask) { fm1_in.notes = mask; frames(2); }
/* (0.11.1) the FM-1's main loop: the input read every ~1 ms, the screen drawn far less often; ms passes */
static void fast(uint32_t ms)
{
    static int32_t o[CTL * 2];
    uint32_t i;
    for (i = 0; i < ms; i++) {
        if (i % 16u == 0u) { mix_block(o, CTL); }
        ui_input();
        fm1_ms += 1;
    }
    ui_leds(); ui_draw();
}

int main(int argc, char **argv)
{
    static uint8_t plain[NREC], plain2[NREC], m1[NREC], m2[NREC], back[NREC];
    uint32_t i, chord, chord2, d1, d2, a1, a2, h1, h2;
    static amu_step_t st1[AMUT_N], st3[AMUT_N];
    track_t *t = &trk[0];
    outdir = argc > 1 ? argv[1] : "build/host";
    panel = PANEL_DEFAULT;
    layers_init();
    settings.palette = 4;
    palette_set(4);
    host_tracks_init();
    for (i = 0; i < NSYN; i++) { set_engine_of(&trk[i], TRK_DEF[i][0]); apply_preset_to(&trk[i], TRK_DEF[i][1]); trk[i].engine = trk[i].eng_req; }
    song.sel = 0;
    t->p[P_AMODE] = 1, t->p[P_ARATE] = 6, t->p[P_AOCT] = 2, t->p[P_APROB] = 127, t->p[P_AGATE] = 64;   /* UP, 1/16, 2 oct */
    go_home(); frames(4);
    transport_req = 1; frames(4);
    chord = 1u << key_of_white(0) | 1u << key_of_white(2) | 1u << key_of_white(4);   /* C E G */
    chord2 = 1u << key_of_white(1) | 1u << key_of_white(3) | 1u << key_of_white(5);  /* D F A */
    hold_keys(chord);
    frames(10);
    /* the cycle of the changes is 16 arp steps; the plain arp (3 notes x 2 octaves: 6): 48 steps apart it is the same */
    arp_record(t, plain);
    arp_record(t, plain2);
    printf("  plain arp: %u %u %u %u %u %u %u %u ...\n", plain[0], plain[1], plain[2], plain[3], plain[4], plain[5], plain[6], plain[7]);
    check(differ(plain, plain2) == 0 || differ(plain, plain2) == NREC, "MUTATE 0: the plain arp (recorded)");

    /* ARP + OCT+ held 0.8 s (the input read every ms, as on the FM-1) */
    fm1_in.buttons |= 1u << panel.btn[B_ARP]; frame();
    fm1_in.buttons |= 1u << panel.btn[B_OCTUP];
    fast(800);
    fm1_in.buttons &= ~(1u << panel.btn[B_OCTUP]); frames(2);
    a1 = amut_amount(0);
    h1 = amut[0].nh;
    memcpy(st1, amut[0].st, sizeof st1);
    ui.force = 1; frame(); ppm("mutate-layer-a");
    printf("  OCT+ held 0.8 s: %u changes, the pattern %u %% changed\n", h1, a1 / 10u);
    check(h1 >= 7u && h1 <= 9u, "ARP + OCT+ held 0.8 s: a change at the press, one every 110 ms after (by the time held)");
    check(a1 > 150u && a1 < 600u, "... part of the pattern changed (the bar)");
    check(ui.layer == LY_ROLL, "the ARP layer shows (its MUTATE bar)");
    fm1_in.buttons &= ~(1u << panel.btn[B_ARP]); frames(4);
    arp_record(t, m1);
    d1 = vs_plain(m1, plain);
    printf("  %u of %u arp steps differ from the plain arp\n", d1, NREC);
    check(d1 > 4u && d1 < 48u, "MUTATE a little: some steps changed, many as they were");

    /* let go, hold again: it goes on from there */
    fm1_in.buttons |= 1u << panel.btn[B_ARP]; frame();
    fm1_in.buttons |= 1u << panel.btn[B_OCTUP];
    fast(250);
    fm1_in.buttons &= ~(1u << panel.btn[B_OCTUP]); frames(2);
    h2 = amut[0].nh;
    {
        uint32_t k, moved = 0;
        for (k = 0; k < AMUT_N; k++) moved += memcmp(&st1[k], &amut[0].st[k], sizeof st1[k]) != 0;
        printf("  held again 0.25 s: %u more changes, %u of 16 steps moved\n", h2 - h1, moved);
        check(h2 - h1 >= 2u && h2 - h1 <= 4u && moved >= 1u && moved <= h2 - h1,
              "held again: it goes on from where it was (the changes so far kept, a few more on them)");
    }
    /* KNOB 2 left, a detent a change: back exactly to where the first hold left it */
    for (i = h1; i < h2; i++) { encs[panel.enc[EN_K2]] = -1; frames(1); }
    check(!memcmp(st1, amut[0].st, sizeof st1) && amut[0].nh == h1, "KNOB 2 left: the last changes taken back, the pattern as it was");

    /* held on and on: no end to it */
    fm1_in.buttons |= 1u << panel.btn[B_OCTUP];
    fast(3000);
    memcpy(st3, amut[0].st, sizeof st3);
    fast(3000);
    fm1_in.buttons &= ~(1u << panel.btn[B_OCTUP]); frames(2);
    a2 = amut_amount(0);
    ui.force = 1; frame(); ppm("mutate-layer-full");
    fm1_in.buttons &= ~(1u << panel.btn[B_ARP]); frames(4);
    printf("  held 6 s more: the pattern %u %% changed\n", a2 / 10u);
    check(memcmp(st3, amut[0].st, sizeof st3) != 0, "held on: it keeps changing (no ceiling, as the KeyStep's)");
    check(a2 >= 750u, "... most of the pattern changed");
    arp_record(t, m2);
    {
        uint32_t rests = 0, outside = 0;
        d2 = vs_plain(m2, plain);
        for (i = 0; i < NREC; i++) {
            uint32_t j, in = 0;
            rests += !m2[i];
            for (j = 0; j < 6u; j++) in |= m2[i] == plain[j];
            outside += m2[i] && !in;
        }
        printf("  %u of %u steps differ (%u rests, %u notes outside the plain arp's)\n", d2, NREC, rests, outside);
        check(d2 > d1, "further: more steps changed");
        check(rests > 0u && outside > 0u, "the changes: rests and octaves the plain arp never plays");
    }

    /* a new chord: the changes stay */
    memcpy(st3, amut[0].st, sizeof st3);
    hold_keys(chord2);
    frames(20);
    check(!memcmp(st3, amut[0].st, sizeof st3), "a new chord: the changes stay");
    hold_keys(chord);
    frames(10);

    /* back with OCT- (held): the plain arp again, note for note */
    fm1_in.buttons |= 1u << panel.btn[B_ARP]; frame();
    fm1_in.buttons |= 1u << panel.btn[B_OCTDN];
    fast(700);
    printf("  OCT- held 0.7 s: %u changes left in the history, the pattern %u %% changed\n", amut[0].nh, amut_amount(0) / 10u);
    check(amut[0].nh > 0u, "OCT- held: back a change at a time (the last first)");
    fast(6000);
    fm1_in.buttons &= ~(1u << panel.btn[B_OCTDN]); frames(2);
    fm1_in.buttons &= ~(1u << panel.btn[B_ARP]); frames(4);
    check(amut_amount(0) == 0u && amut[0].nh == 0u, "ARP + OCT- held: all the way back");
    arp_record(t, back);
    check(vs_plain(back, plain) == 0u, "back: the plain arp, note for note");

    /* both at once: as it was */
    fm1_in.buttons |= 1u << panel.btn[B_ARP]; frame();
    fm1_in.buttons |= 1u << panel.btn[B_OCTUP]; fast(1000);
    check(amut_amount(0) > 0u, "(mutated again)");
    fm1_in.buttons |= 1u << panel.btn[B_OCTDN]; frames(2);
    check(amut_amount(0) == 0u && amut[0].nh == 0u, "ARP + OCT+ and OCT- together: the arp as it was, at once");
    fm1_in.buttons &= ~(1u << panel.btn[B_OCTUP] | 1u << panel.btn[B_OCTDN]); frames(2);
    /* KNOB 2 on the ARP layer: a change a detent */
    encs[panel.enc[EN_K2]] = 10; frames(2);
    check(amut[0].nh == 10u && amut_amount(0) > 0u, "KNOB 2 on the ARP layer: a change a detent");
    encs[panel.enc[EN_K2]] = -10; frames(2);
    check(amut_amount(0) == 0u, "KNOB 2 back: the arp as it was");
    fm1_in.buttons &= ~(1u << panel.btn[B_ARP]); frames(4);
    hold_keys(0); frames(10);

    /* the drum track: ARP + OCT+ is the hard roll */
    song.sel = TRK_DRUM; go_home(); frames(4);
    fm1_in.buttons |= 1u << panel.btn[B_ARP]; frame();
    fm1_in.buttons |= 1u << panel.btn[B_OCTUP]; frames(60);
    check(amut_amount(TRK_DRUM) == 0u && key_lvl() == LV_HARD, "the drum track: ARP + OCT+ the hard roll, nothing mutated");
    ui.force = 1; frame(); ppm("mutate-layer-drums");
    fm1_in.buttons &= ~(1u << panel.btn[B_OCTUP] | 1u << panel.btn[B_ARP]); frames(4);
    transport_req = 2; frames(4);
    printf("mutate: %d failed\n", fails);
    return fails != 0;
}

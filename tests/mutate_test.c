/* SPDX-License-Identifier: GPL-3.0-only */
/* SNOOP 0.11: MUTATE (seq.c amut, after Arturia's KeyStep): ARP held + OCT+ held changes the arpeggio, the longer
 * the more; ARP + OCT- takes it back; both: none; KNOB 2 on the ARP layer too. On the ui_pages_test harness (the real
 * UI, the audio between the frames). Checks:
 *   - MUTATE at 0: the arp as ever (the same notes as before it was touched);
 *   - OCT+ held: the amount grows with the time held, the ARP layer shows it (its bar), the arp's notes differ from
 *     the plain arp's, more of them the further it goes (rests, other notes, octaves, repeats, ties);
 *   - a new chord: the changes stay (the amount does not move);
 *   - OCT- back to 0: the plain arp again, note for note; OCT+ and OCT- together: 0 at once;
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
static void hold_keys(uint32_t mask) { fm1_in.notes = mask; frames(2); }

int main(int argc, char **argv)
{
    static uint8_t plain[NREC], plain2[NREC], m1[NREC], m2[NREC], back[NREC];
    uint32_t i, chord, chord2, d1, d2, a1, a2;
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

    /* ARP + OCT+ held, a little */
    fm1_in.buttons |= 1u << panel.btn[B_ARP]; frame();
    fm1_in.buttons |= 1u << panel.btn[B_OCTUP];
    frames(50);                                         /* ~0.8 s */
    fm1_in.buttons &= ~(1u << panel.btn[B_OCTUP]); frames(2);
    a1 = amut[0].amt;
    ui.force = 1; frame(); ppm("mutate-layer-a");
    check(a1 > 200u && a1 < 450u, "ARP + OCT+ held 0.8 s: MUTATE about a third");
    check(ui.layer == LY_ROLL, "the ARP layer shows (its MUTATE bar)");
    fm1_in.buttons &= ~(1u << panel.btn[B_ARP]); frames(4);
    arp_record(t, m1);
    /* (the plain arp at the same steps: it repeats every 6 notes, the record started at a multiple of 6? compare by the
     * cycle: the plain one shifted to the same phase) */
    {
        uint32_t best = NREC, sh;
        for (sh = 0; sh < 6u; sh++) {
            uint32_t d = 0;
            for (i = 0; i < NREC; i++) d += m1[i] != plain[(i + sh) % 6u];
            if (d < best) best = d;
        }
        d1 = best;
    }
    printf("  MUTATE %u %%: %u of %u steps changed\n", a1 / 10u, d1, NREC);
    check(d1 > 4u && d1 < 40u, "MUTATE a third: some steps changed, most as they were");

    /* further */
    fm1_in.buttons |= 1u << panel.btn[B_ARP]; frame();
    fm1_in.buttons |= 1u << panel.btn[B_OCTUP];
    frames(150);
    fm1_in.buttons &= ~(1u << panel.btn[B_OCTUP]); frames(2);
    a2 = amut[0].amt;
    ui.force = 1; frame(); ppm("mutate-layer-full");
    fm1_in.buttons &= ~(1u << panel.btn[B_ARP]); frames(4);
    check(a2 == 1000u, "held on: MUTATE all the way (100 %)");
    arp_record(t, m2);
    {
        uint32_t best = NREC, sh, rests = 0, outside = 0;
        for (sh = 0; sh < 6u; sh++) {
            uint32_t d = 0;
            for (i = 0; i < NREC; i++) d += m2[i] != plain[(i + sh) % 6u];
            if (d < best) best = d;
        }
        d2 = best;
        for (i = 0; i < NREC; i++) {
            uint32_t j, in = 0;
            rests += !m2[i];
            for (j = 0; j < 6u; j++) in |= m2[i] == plain[j];
            outside += m2[i] && !in;
        }
        printf("  MUTATE 100 %%: %u of %u steps changed (%u rests, %u notes outside the plain arp's)\n", d2, NREC, rests, outside);
        check(d2 > d1 + 8u, "MUTATE further: more steps changed");
        check(rests > 0u && outside > 0u, "the changes: rests and octaves the plain arp never plays");
    }

    /* a new chord: the changes stay */
    hold_keys(chord2);
    frames(20);
    check(amut[0].amt == 1000u, "a new chord: MUTATE stays");
    hold_keys(chord);
    frames(10);

    /* back with OCT- to 0: the plain arp again */
    fm1_in.buttons |= 1u << panel.btn[B_ARP]; frame();
    fm1_in.buttons |= 1u << panel.btn[B_OCTDN];
    frames(130);
    fm1_in.buttons &= ~(1u << panel.btn[B_OCTDN]); frames(2);
    fm1_in.buttons &= ~(1u << panel.btn[B_ARP]); frames(4);
    check(amut[0].amt == 0u, "ARP + OCT- held: back to 0");
    arp_record(t, back);
    {
        uint32_t best = NREC, sh;
        for (sh = 0; sh < 6u; sh++) {
            uint32_t d = 0;
            for (i = 0; i < NREC; i++) d += back[i] != plain[(i + sh) % 6u];
            if (d < best) best = d;
        }
        check(best == 0u, "back at 0: the plain arp, note for note");
    }

    /* both at once: none */
    fm1_in.buttons |= 1u << panel.btn[B_ARP]; frame();
    fm1_in.buttons |= 1u << panel.btn[B_OCTUP]; frames(60);
    fm1_in.buttons |= 1u << panel.btn[B_OCTDN]; frames(2);
    check(amut[0].amt == 0u, "ARP + OCT+ and OCT- together: MUTATE 0 at once");
    fm1_in.buttons &= ~(1u << panel.btn[B_OCTUP] | 1u << panel.btn[B_OCTDN]); frames(2);
    /* KNOB 2 on the ARP layer */
    encs[panel.enc[EN_K2]] = 10; frames(2);
    check(amut[0].amt == 200u, "KNOB 2 on the ARP layer: MUTATE too");
    encs[panel.enc[EN_K2]] = -10; frames(2);
    fm1_in.buttons &= ~(1u << panel.btn[B_ARP]); frames(4);
    hold_keys(0); frames(10);

    /* the drum track: ARP + OCT+ is the hard roll */
    song.sel = TRK_DRUM; go_home(); frames(4);
    fm1_in.buttons |= 1u << panel.btn[B_ARP]; frame();
    fm1_in.buttons |= 1u << panel.btn[B_OCTUP]; frames(60);
    check(amut[TRK_DRUM].amt == 0u && key_lvl() == LV_HARD, "the drum track: ARP + OCT+ the hard roll, nothing mutated");
    ui.force = 1; frame(); ppm("mutate-layer-drums");
    fm1_in.buttons &= ~(1u << panel.btn[B_OCTUP] | 1u << panel.btn[B_ARP]); frames(4);
    transport_req = 2; frames(4);
    printf("mutate: %d failed\n", fails);
    return fails != 0;
}

/* SPDX-License-Identifier: GPL-3.0-only */
/* SNOOP 0.10: track 4 as the drums or a fourth synth part (GLO > TRACK 4; core.h is_drum, seq.c t4_switch, ui.c
 * t4_set), on the ui_pages_test harness (the real UI, the audio between the frames). Checks:
 *   - GLO > TRACK 4: one detent arms ("AGAIN"), a second one switches; the drums' pattern and kit kept aside;
 *   - SYNTH: an empty pattern, ANALOG WARM PAD; its keys and its steps sound on track 4's own voices (not the
 *     drums), through its LEVEL; the four parts share the voice budget; the TRACKS screen names its sound; the SEQ
 *     button opens the synths' NOTES page, not the drum grid; the drums' channel plays it from MIDI;
 *   (the second core: tests/mix_split_test.c, scene t4synth);
 *   - back to DRUMS: the beat and the kit as they were, nothing of the synth sounding; SYNTH again: its steps back;
 *   - a new project (TOOLS > NEW): DRUMS.
 * Screens: DIR/t4-*.ppm */
#define UI_PAGES_HARNESS_ONLY 1
#include "ui_pages_test.c"

static int32_t mix_peak(uint32_t blocks, int32_t *t4pk, int32_t *drpk)
{
    static int32_t o[CTL * 2];
    int32_t pk = 0;
    uint32_t b, k;
    *t4pk = *drpk = 0;
    for (b = 0; b < blocks; b++) {
        mix_block(o, CTL);
        for (k = 0; k < CTL * 2u; k++)
            if ((o[k] < 0 ? -o[k] : o[k]) > pk) pk = o[k] < 0 ? -o[k] : o[k];
        if (trk[TRK_DRUM].peak > *t4pk) *t4pk = trk[TRK_DRUM].peak;
        if (drums.peak > *drpk) *drpk = drums.peak;
        trk[TRK_DRUM].peak = 0, drums.peak = 0;
    }
    return pk;
}
static void glo_page(const char *title)
{
    uint32_t n = 0;
    go_home(); frame();
    open_family(FAM_GLO); frame();
    while (strcmp(PAGES[ui.page].title, title) && n++ < 20u) { open_family(FAM_GLO); frame(); }
}
static void knob1(int32_t s) { encs[panel.enc[EN_K1]] = s; frame(); frame(); }

int main(int argc, char **argv)
{
    uint32_t i, j, voices;
    int32_t pk, t4pk, drpk;
    static dstep_t beat[NSTEP];
    outdir = argc > 1 ? argv[1] : "build/host";
    panel = PANEL_DEFAULT;
    layers_init();
    settings.palette = 4;
    palette_set(4);
    host_tracks_init();
    for (i = 0; i < NSYN; i++) { set_engine_of(&trk[i], TRK_DEF[i][0]); apply_preset_to(&trk[i], TRK_DEF[i][1]); trk[i].engine = trk[i].eng_req; }
    TDRUM->p[P_E0] = DRUM_DEFAULT_KIT + 1;
    for (j = 0; j < 16u; j++) {                         /* a beat on the drums */
        memset(&TDRUM->dstep[j], 0, sizeof(dstep_t));
        if (!(j % 4u)) dstep_set(&TDRUM->dstep[j], 0, LV_NORM, 0);
        if ((j % 8u) == 4u) dstep_set(&TDRUM->dstep[j], 3, LV_HARD, 0);
    }
    TDRUM->p[P_SLEN] = 16, TDRUM->seq_active = 1;
    memcpy(beat, TDRUM->dstep, sizeof beat);
    go_home(); frames(4);
    check(is_drum(TDRUM) && !song.g[G_T4], "power-on: track 4 the drums (SLOOP's)");

    /* GLO > TRACK 4: armed, then switched */
    glo_page("TRACK 4");
    check(!strcmp(PAGES[ui.page].title, "TRACK 4"), "GLO > TRACK 4: the page is there");
    ui.force = 1; frame(); ppm("t4-page-drums");
    knob1(1);
    check(song.g[G_T4] == 0 && ui.arm == G_T4, "TRACK 4: one detent arms it (AGAIN), nothing changed yet");
    knob1(1);
    check(song.g[G_T4] == 1 && !is_drum(TDRUM) && trk_is_part(TRK_DRUM), "TRACK 4: a second detent: SYNTH");
    ui.force = 1; frame(); ppm("t4-page-synth");
    {
        const engine_t *e = ENGINES[TDRUM->eng_req];
        uint32_t empty = 1;
        for (j = 0; j < NSTEP; j++) empty &= TDRUM->step[j].time == ST_REST && !TDRUM->step[j].n;
        check(TDRUM->eng_req == 0u && !strcmp(e->presets[TDRUM->preset % e->npresets].name, "WARM PAD") &&
              TDRUM->engine == TDRUM->eng_req, "SYNTH: ANALOG WARM PAD");
        check(empty, "SYNTH: an empty pattern (the beat kept aside)");
    }

    /* its keys: track 4's own voices, not the drums */
    song.sel = TRK_DRUM;
    go_home(); frames(2);
    trk_note_on(TDRUM, 60, 110); trk_note_on(TDRUM, 64, 110); trk_note_on(TDRUM, 67, 110);
    pk = mix_peak(200, &t4pk, &drpk);
    for (voices = 0, j = 0; j < NVOICE; j++) voices += TDRUM->v[j].active;
    printf("  a chord on track 4: out %d, track 4's peak %d, the drums' %d, %u voices\n", pk, t4pk, drpk, voices);
    check(pk > 2000 && t4pk > 1000 && drpk == 0 && voices == 3u, "SYNTH: a chord on track 4 sounds on its own voices, not the drums");
    ui.force = 1; frame(); ppm("t4-tracks-synth");
    {   /* the TRACKS screen: its sound's name */
        char b[16];
        trk_short_name(TRK_DRUM, b);
        check(!strcmp(b, "WARM PAD"), "TRACKS: track 4's name is its sound (not DRUM)");
    }
    TDRUM->p[P_LEVEL] = 0;
    pk = mix_peak(40, &t4pk, &drpk);
    pk = mix_peak(40, &t4pk, &drpk);
    check(t4pk == 0, "SYNTH: track 4's LEVEL (not the drums' GLO > DRUMS > LVL) sets its level");
    TDRUM->p[P_LEVEL] = 100;
    trk_note_off(TDRUM, 60); trk_note_off(TDRUM, 64); trk_note_off(TDRUM, 67);
    mix_peak(800, &t4pk, &drpk);

    /* the shared voice budget: four parts, 8 voices */
    for (i = 0; i < NTRK; i++)
        for (j = 0; j < 4u; j++)
            trk_note_on(&trk[i], 48u + i * 7u + j * 3u, 100);
    mix_peak(20, &t4pk, &drpk);
    for (voices = 0, i = 0; i < NPART; i++)
        for (j = 0; j < NVOICE; j++) voices += trk[i].v[j].active && trk[i].v[j].stage != 4u;
    check(voices <= NVOICE, "SYNTH: four parts share the budget of 8 voices");
    for (i = 0; i < NTRK; i++)
        for (j = 0; j < 4u; j++)
            trk_note_off(&trk[i], 48u + i * 7u + j * 3u);
    mix_peak(1500, &t4pk, &drpk);

    /* its steps play */
    TDRUM->p[P_SLEN] = 16;
    for (j = 0; j < 16u; j += 4u) {
        step_t *s = &TDRUM->step[j];
        s->n = 1, s->note[0] = (uint8_t)(55u + j), s->time = ST_NOTE, s->vel = 100;
    }
    TDRUM->seq_active = 1;
    transport_req = 1; frames(4);
    pk = mix_peak(1500, &t4pk, &drpk);
    check(t4pk > 1000 && drpk == 0, "SYNTH: its pattern plays on track 4 (the drums silent)");
    transport_req = 2; frames(4);
    mix_peak(1500, &t4pk, &drpk);

    /* SEQ on track 4: the synths' NOTES page, not the drum grid */
    song.sel = TRK_DRUM;
    go_home(); frames(2);
    tap(B_SEQ); frames(2);
    check(cur_page()->scope != SC_DRUM && !on_drum_page(), "SYNTH: SEQ opens the NOTES page, not the drum grid");
    ui.force = 1; frame(); ppm("t4-notes-synth");

    /* MIDI on the drums' channel plays it */
    check(midi_own((uint32_t)song.g[G_DRCH] - 1u) == TDRUM, "SYNTH: the drums' MIDI channel (GLO > DRUMS > CH) is track 4's");

    /* back to DRUMS: the beat and the kit as they were */
    {
        static step_t syn_steps[NSTEP];
        memcpy(syn_steps, TDRUM->step, sizeof syn_steps);
        trk_note_on(TDRUM, 60, 100);
        mix_peak(10, &t4pk, &drpk);
        glo_page("TRACK 4");
        knob1(-1); knob1(-1);
        for (voices = 0, j = 0; j < NVOICE; j++) voices += TDRUM->v[j].active;
        check(song.g[G_T4] == 0 && is_drum(TDRUM) && !voices, "back to DRUMS: nothing of the synth sounds");
        check(!memcmp(TDRUM->dstep, beat, sizeof beat) && TDRUM->p[P_E0] == DRUM_DEFAULT_KIT + 1,
              "back to DRUMS: the beat and the kit as they were");
        transport_req = 1; frames(4);
        pk = mix_peak(1500, &t4pk, &drpk);
        check(drpk > 1000 && t4pk == 0, "DRUMS: the beat plays on the drums again");
        transport_req = 2; frames(4);
        mix_peak(600, &t4pk, &drpk);
        song.sel = TRK_DRUM; go_home(); frames(2);
        tap(B_SEQ); frames(2);
        check(on_drum_page(), "DRUMS: SEQ opens the drum grid again");
        go_home(); frames(2);
        glo_page("TRACK 4");
        knob1(1); knob1(1);
        check(song.g[G_T4] == 1 && !memcmp(TDRUM->step, syn_steps, sizeof syn_steps) && TDRUM->eng_req == 0u,
              "SYNTH again: its steps and its sound back");
    }

    /* TOOLS > NEW: the drums */
    project_new(); frames(4);
    check(song.g[G_T4] == 0 && is_drum(TDRUM), "TOOLS > NEW: track 4 the drums again");
    go_home(); ui.force = 1; frame(); ppm("t4-tracks-drums");
    printf("t4: %d failed\n", fails);
    return fails != 0;
}

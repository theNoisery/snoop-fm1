/* SPDX-License-Identifier: GPL-3.0-only */
/* SNOOP 0.10: GLO > TRACK 4 in a project (project.c proj_t4_put / proj_apply). Checks:
 *   - a project saved with track 4 a synth loads as one: the mode, its engine, preset, parameters and synth steps;
 *   - a project of SLOOP's (no mark in G_ROUTE's slot) loads with track 4 the drums, as ever; loading it while track 4
 *     is a synth turns it back to the drums (its voices stopped);
 *   - a section stored in the other mode leaves track 4 as it is (tracks 1..3 load);
 *   - the drum sounds' TUNE / DECAY never overwrite a synth track 4's engine values. */
#define main hostsim_main
#include <stddef.h>
#include "hostsim.c"
#undef main
#define PROJ_HOST 1
static uint32_t trk_def_engine(uint32_t i) { return i < NSYN ? i : 0u; }
#include "../firmware/src/project.c"

static int fails;
static void check(const char *what, int ok)
{
    printf("%-74s %s\n", what, ok ? "ok" : "FAIL");
    fails += !ok;
}

int main(void)
{
    static project_t syn, drm, sec;
    uint32_t j;
    int16_t e[8];
    static step_t steps[NSTEP];
    host_tracks_init();
    host_preset(&trk[0], 0, 1);
    /* a project with track 4 a synth: FM6, a pattern */
    t4_switch(1);
    host_preset(TDRUM, ENGI_FM6, 4);
    for (j = 0; j < 16u; j += 2u) {
        step_t *s = &TDRUM->step[j];
        s->n = 2, s->note[0] = (uint8_t)(48u + j), s->note[1] = (uint8_t)(55u + j), s->time = ST_NOTE, s->vel = 90;
    }
    TDRUM->p[P_LEVEL] = 77, TDRUM->p[P_PAN] = -20;
    drum_tune[3] = 5, drum_dec[3] = -2;                  /* (the drums' tuning: must not land in its E1..E7) */
    for (j = 0; j < 8u; j++) e[j] = TDRUM->p[P_E0 + j];
    memcpy(steps, TDRUM->step, sizeof steps);
    proj_capture(&syn);
    check("saved as SYNTH: G_ROUTE's slot holds the mark and the mode", proj_t4_of(&syn) == 1u);
    {
        uint32_t same = 1;
        for (j = 0; j < 8u; j++) same &= syn.t[TRK_DRUM].e[j] == e[j];
        check("saved as SYNTH: track 4's engine values as they are (not the drums' TUNE / DECAY)", same);
    }

    /* a project of SLOOP's: the drums */
    t4_switch(0);
    host_tracks_init();
    TDRUM->p[P_E0] = 3;
    for (j = 0; j < 16u; j++) {
        memset(&TDRUM->dstep[j], 0, sizeof(dstep_t));
        if (!(j % 4u)) dstep_set(&TDRUM->dstep[j], 0, LV_NORM, 0);
    }
    proj_capture(&drm);
    drm.g[G_ROUTE] = 2;                                  /* (as SLOOP saves it: GLO > SYSTEM > IN) */
    drm.sum = proj_sum(&drm);
    check("a SLOOP project (no mark): the drums", proj_t4_of(&drm) == 0u);

    /* load SYNTH */
    proj_apply(&syn, 1);
    check("load: track 4 a synth again", song.g[G_T4] == 1 && !is_drum(TDRUM));
    check("load: its engine, preset, LEVEL, PAN", TDRUM->eng_req == ENGI_FM6 && TDRUM->preset == 4u &&
                                                  TDRUM->p[P_LEVEL] == 77 && TDRUM->p[P_PAN] == -20);
    check("load: its synth steps", !memcmp(TDRUM->step, steps, sizeof steps));
    {
        uint32_t same = 1;
        for (j = 0; j < 8u; j++) same &= TDRUM->p[P_E0 + j] == e[j];
        check("load: its engine values", same);
    }
    /* a section stored as DRUMS, played while track 4 is a synth: track 4 stays */
    sec = drm;
    host_preset(&trk[1], 1, 2);
    sec.t[1].engine = 6, sec.t[1].preset = 1;
    proj_apply(&sec, 0);
    check("a section stored as DRUMS, track 4 a synth: track 4 stays as it is",
          song.g[G_T4] == 1 && TDRUM->eng_req == ENGI_FM6 && !memcmp(TDRUM->step, steps, sizeof steps));
    check("... the other tracks load", trk[1].eng_req == 6u);

    /* load the SLOOP project while track 4 sounds as a synth */
    TDRUM->engine = TDRUM->eng_req;
    trk_note_on(TDRUM, 60, 100);
    {
        static int32_t o[CTL * 2];
        uint32_t b, v, any = 0;
        for (b = 0; b < 20u; b++) mix_block(o, CTL);
        proj_apply(&drm, 1);
        for (v = 0; v < NVOICE; v++) any |= TDRUM->v[v].active;
        check("load a DRUMS project over a sounding synth: the drums, its voices stopped", song.g[G_T4] == 0 &&
              is_drum(TDRUM) && !any);
        check("... its beat and kit", TDRUM->p[P_E0] == 3 && dstep_has(&TDRUM->dstep[4], 0) && !dstep_has(&TDRUM->dstep[5], 0));
        for (b = 0; b < 400u; b++) mix_block(o, CTL);
        check("... and the drums play it (the engine state gone)", TDRUM->eng_req == 0u);
    }
    printf("t4 projects: %d failed\n", fails);
    return fails != 0;
}

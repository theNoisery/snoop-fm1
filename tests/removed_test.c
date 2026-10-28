/* SPDX-License-Identifier: GPL-3.0-only */
/* SNOOP: the sounds taken out for flash (tools/gen_samples.py REMOVED_SETS, tools/gen_drumkits.py REMOVED_KITS).
 * Checks: the KIT knob steps over the kits taken out; a project's kit number taken out plays 808; the numbers of the
 * kits kept are as before; SAMPLE and GRAIN on an emptied set are silent and safe. */
#define main hostsim_main
#include "hostsim.c"
#undef main
#include <string.h>

static uint32_t fails;
static void check(int ok, const char *what)
{
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        fails++;
}
static uint32_t eng_named(const char *n)
{
    uint32_t e;
    for (e = 0; e < NENGINES; e++)
        if (!strcmp(ENGINES[e]->name, n))
            return e;
    return 0;
}

int main(void)
{
    static const char *const GONE[8] = {"VINTAGE", "TRAP", "DRILL", "DUBSTEP", "DEMBOW", "AFRO", "TRIBAL", "HYPER"};
    uint32_t i, k, gone = 0, bad = 0;
    int32_t kit;
    puts("the sounds taken out (SNOOP):");
    for (i = 0; i < DS_NKITS; i++) {                    /* the names by number: as SLOOP's; the 8 gone */
        int g = 0;
        for (k = 0; k < 8u; k++)
            g |= !strcmp(DRUM_KIT_NAMES[DRUM_SAMPLED + i], GONE[k]);
        if (g != ds_gone(i)) bad++;
        gone += ds_gone(i);
        if (!g && strcmp(ds_kit(i)->name, DRUM_KIT_NAMES[DRUM_SAMPLED + i])) bad++;
    }
    check(gone == 8u && !bad && DS_NKITS == 32u, "the kit numbers as SLOOP's (projects keep theirs); 8 taken out, the rest themselves");
    for (kit = 0, k = 0; k < 60u; k++) {
        int32_t n = kit_step(kit, 1);
        if (kit_gone(n)) bad++;
        kit = n;
    }
    check(!bad && kit == (int32_t)DRUM_KITS - 1, "KIT knob up: never on a kit taken out, to the last kit");
    for (k = 0; k < 60u; k++)
        if (kit_gone(kit = kit_step(kit, -1))) bad++;
    check(!bad && kit == 0, "KIT knob down: the same, back to the first");
    check(kit_step(DRUM_SAMPLED + 3u, 1) == (int16_t)(DRUM_SAMPLED + 7u), "80S + 1 detent: BOOMBAP (VINTAGE, TRAP, DRILL skipped)");
    check(!strcmp(ds_kit(5)->name, "808"), "a project on TRAP (number 5): plays 808");
    {   /* SAMPLE and GRAIN on the emptied sets: silent, no crash */
        int32_t out[CTL * 2];
        uint32_t eng, set, b, loud = 0;
        static const char *const ENG[2] = {"SAMPLE", "GRAIN"};
        for (eng = 0; eng < 2u; eng++)
            for (set = 1; set <= 6u; set += 5u) {           /* BASS (1), SCRCH (6) */
                host_tracks_init();
                host_preset(&trk[0], eng_named(ENG[eng]), 0);
                trk[0].p[P_E0] = (int16_t)set;
                trk[0].p[P_DLY] = trk[0].p[P_REV] = trk[0].p[P_CHOR] = 0;
                song.g[G_DRLVL] = 0;
                for (k = 36; k < 96u; k += 7u)
                    trk_note_on(&trk[0], k, 120);
                for (b = 0; b < 1400u; b++) {
                    mix_block(out, CTL);
                    for (i = 0; i < CTL * 2u; i++)
                        loud += out[i] > 64 || out[i] < -64;
                }
                for (k = 36; k < 96u; k += 7u)
                    trk_note_off(&trk[0], k);
            }
        check(!loud, "SAMPLE and GRAIN on BASS and SCRCH (emptied): silent, no crash");
    }
    printf("removed: %u failed\n", fails);
    return fails != 0;
}

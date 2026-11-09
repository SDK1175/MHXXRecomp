// Monsters' attention scores at high frame rates. Each monster keeps a score per hunter, Palico and enemy (status block
// *(monster + 0x1428): +0x254, +0x264, +0x274); the target pickers (0x2c5b14) take the highest. Every update
// 0x2bbf98 adds a rate from the species' table (distance to the target and so on, 0x2bb828) as
//     score += (int)(rate * step)
// with the step in 60 fps frames (monster +0x1c): 2.0 on the Switch's 30 updates a second. The rates are small (a Jaggi
// near a hunter: 3 or 4), so above 30 fps the cast loses them: at 240 fps (step 0.25) any rate from -3 to 3 adds nothing,
// scores no longer rise with closeness nor fall with distance, and those that have risen stay. At 60 fps a step just
// under 1.0 still takes 1 off a rate.
// Here the monster's time is cut into 30 fps updates (2.0 of its step): 0x2bbf98 runs once for each with the step 2.0,
// so the scores move as in the 30 fps game. At 30 fps and below the original. MHGU_NO_HATESTEP=1: the original per-update
// add (for comparing).
#include "../game/addr.h"
#include <math.h>
#include <stdlib.h>

#include "../hle.h"

#define FN_HATE  A_HATESTEP_FN_HATE  // (monster): the update of its attention scores
#define EN_STEP  0x1cu      // monster: its step (f32, 60 fps frames)
#define SLOTS    32

static struct { uint32_t en; uint64_t seen; float t; } g_slot[SLOTS];

// MHXX_HATE_LOG=1: each monster's scores (hunters, Palicos, the rest: +0x254 / +0x264 / +0x274, 4 each) about once a
// second of game time, to compare a run at 30 fps with one above it (the same hunt: the scores should move alike)
static int g_log = -1;
static void log_scores(uint32_t en, float step) {
    static struct { uint32_t en; float t; } last[8];
    int k = 0;
    while (k < 7 && last[k].en && last[k].en != en) k++;
    if (last[k].en != en) last[k].en = en, last[k].t = 0;
    if ((last[k].t += step) < 60.0f) return;
    last[k].t = 0;
    uint32_t st = LD32(en + 0x1428u);
    if (!st) return;
    int32_t v[12];
    for (int i = 0; i < 12; i++) v[i] = (int32_t)LD32(st + 0x254u + 4u * i);
    LOG("[hate] %08x step %.2f  hunters %d %d %d %d  palicos %d %d %d %d  other %d %d %d %d\n", en, step, v[0], v[1],
        v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11]);
}

static void hate(Cpu *c) {
    uint32_t en = c->r[0];
    float step = LDF(en + EN_STEP);
    if (g_log) log_scores(en, step);
    if (!(step > 0.0f && step < 2.0f)) {
        fnhook_call_original(c, FN_HATE);
        return;
    }
    int s = 0, old = 0;
    while (s < SLOTS && g_slot[s].en != en) {
        if (g_slot[s].seen < g_slot[old].seen) old = s;
        s++;
    }
    if (s == SLOTS) {  // (new here: its first update starts a 30 fps update)
        s = old;
        g_slot[s].en = en, g_slot[s].t = 2.0f - step;
    }
    g_slot[s].seen = g_present_count;
    g_slot[s].t += step;
    if (g_slot[s].t < 2.0f) return;
    g_slot[s].t = fmodf(g_slot[s].t, 2.0f);
    STF(en + EN_STEP, 2.0f);
    fnhook_call_original(c, FN_HATE);
    STF(en + EN_STEP, step);
}

static void log_only(Cpu *c) {
    log_scores(c->r[0], LDF(c->r[0] + EN_STEP));
    fnhook_call_original(c, FN_HATE);
}

void hatestep_init(void) {
    PORT_OFF(HATESTEP, "monster attention at high frame rates");
    const char *l = getenv("MHXX_HATE_LOG");
    g_log = l && *l == '1';
    const char *e = getenv("MHGU_NO_HATESTEP");
    if (e && *e == '1' && !g_log) return;
    if (e && *e == '1') {  // (the original, logged)
        LOG("[hate] original update (MHGU_NO_HATESTEP), scores logged\n");
        fnhook_set(FN_HATE, hle_register("hatestep:log", log_only));
        return;
    }
    fnhook_set(FN_HATE, hle_register("hatestep:hate", hate));
}

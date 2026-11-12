// MHGU 1.4.0 plugins in MHXX 1.5.1: see gucompat.h. The table (game/gu2xx.h) has the ranges of code the two games
// share: the code is the same and sits elsewhere, and so do the functions it calls. A region as large as GU's
// image holds each range at GU's address, its calls out of the function written as GU has them (the target
// at GU's address), so a plugin that checks the code it is about to hook finds what it expects.
#include "gucompat.h"
#include "game/gu2xx.h"
#include "rt.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define NCODE (sizeof GU2XX_CODE / sizeof GU2XX_CODE[0])
typedef struct { uint32_t gu, xx, len; } Range;  // (offsets from the bases)
static const Range *by_gu = (const Range *)GU2XX_CODE;  // (the table: sorted by gu)
static Range by_xx[NCODE];
static uint32_t nranges;
static int state;  // 0: not built; 1: built; -1: nothing to build
static int unknown;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
_Static_assert(sizeof(Range) == sizeof GU2XX_CODE[0], "the table's rows are Ranges");

static bool is_branch(uint32_t w) { return (w & 0x0E000000u) == 0x0A000000u && (w >> 28) != 0xF; }

static const Range *range_of_xx(uint32_t xx) {
    uint32_t lo = 0, hi = nranges;  // the first range above xx
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (by_xx[mid].xx <= xx) lo = mid + 1;
        else hi = mid;
    }
    return lo && xx - by_xx[lo - 1].xx < by_xx[lo - 1].len ? &by_xx[lo - 1] : NULL;
}
static const Range *range_of_gu(uint32_t gu) {
    uint32_t lo = 0, hi = NCODE;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (by_gu[mid].gu <= gu) lo = mid + 1;
        else hi = mid;
    }
    return lo && gu - by_gu[lo - 1].gu < by_gu[lo - 1].len ? &by_gu[lo - 1] : NULL;
}
static int cmp_xx(const void *a, const void *b) {
    uint32_t x = ((const Range *)a)->xx, y = ((const Range *)b)->xx;
    return x < y ? -1 : x > y;
}

// Data the code refers to by distance (ldr rd, [pc, #n] of a literal, then add / ldr with pc): the literal shown as
// GU's, the distance from GU's address of the code to GU's of the data (plugins check such words in the code they
// hook). From MHXX's data to GU's: GU2XX_GLOBAL backwards (gucompat.c's data part, declared below).
static uint32_t global_gu(uint32_t xx);
static int pc_literals(const Range *r) {
    uint32_t lit_at[16] = {0}, n = 0;
    int k = 0;
    for (uint32_t o = 0; o < r->len; o += 4, n++) {
        uint32_t w = LD32(MAIN_BASE + r->xx + o);
        if ((w >> 28) != 0xE) continue;
        if ((w & 0x0F7F0000u) == 0x051F0000u) {  // ldr rd, [pc, #+-imm]
            uint32_t imm = w & 0xFFF, la = r->xx + o + 8 + ((w & 0x00800000u) ? imm : -imm);
            lit_at[(w >> 12) & 15] = la - r->xx < r->len ? la : 0;
            continue;
        }
        uint32_t rm = 16;
        if ((w & 0x0FF00FF0u) == 0x00800000u && ((w >> 16) & 15) == 15) rm = w & 15;           // add rd, pc, rm
        else if ((w & 0x0FF00FF0u) == 0x07900000u && ((w >> 16) & 15) == 15) rm = w & 15;      // ldr rd, [pc, rm]
        else if ((w & 0x0FF00FF0u) == 0x00800000u && (w & 15) == 15) rm = (w >> 16) & 15;      // add rd, rn, pc
        if (rm == 16 || !lit_at[rm]) continue;
        uint32_t la = lit_at[rm], t = r->xx + o + 8 + LD32(MAIN_BASE + la), gt = global_gu(t);
        if (gt) ST32(GU_SHADOW + r->gu + (la - r->xx), gt - (r->gu + o + 8)), k++;
    }
    (void)n;
    return k;
}

bool gucompat_build(void) {
    pthread_mutex_lock(&lock);
    if (state) { pthread_mutex_unlock(&lock); return state > 0; }
    const Module *main = &g_modules[0];
    for (uint32_t i = 0; i < NCODE; i++) {  // (all of them or none: the two lookups share the table)
        const Range *r = &by_gu[i];
        if ((uint64_t)r->gu + r->len > GU_SHADOW_SIZE || (uint64_t)r->xx + r->len > main->text_size) break;
        by_xx[nranges++] = *r;
    }
    if (!NCODE || nranges != NCODE) {
        nranges = 0, state = -1;
        pthread_mutex_unlock(&lock);
        LOG("[forge] plugins for MHGU: the table of addresses does not fit this game: not translated\n");
        return false;
    }
    qsort(by_xx, nranges, sizeof by_xx[0], cmp_xx);
    plat_commit(GUEST_BASE + GU_SHADOW, GU_SHADOW_SIZE);
    kernel_map_region(GU_SHADOW, GU_SHADOW_SIZE, 3, 5);
    uint32_t bytes = 0;
    for (uint32_t i = 0; i < nranges; i++) {
        const Range *r = &by_xx[i];
        bytes += r->len;
        for (uint32_t o = 0; o < r->len; o += 4) {
            uint32_t w = LD32(MAIN_BASE + r->xx + o);
            if (is_branch(w)) {  // a call or jump out of the function: to GU's address of the target, when it is known
                uint32_t t = r->xx + o + 8 + (uint32_t)((int32_t)(w << 8) >> 6);
                const Range *tr = t - r->xx < r->len ? NULL : range_of_xx(t);  // (inside the range: the same distance)
                if (tr) w = (w & 0xFF000000u) | ((((t - tr->xx + tr->gu) - (r->gu + o) - 8) >> 2) & 0xFFFFFFu);
            }
            ST32(GU_SHADOW + r->gu + o, w);
        }
    }
    int lits = 0;
    for (uint32_t i = 0; i < nranges; i++) lits += pc_literals(&by_xx[i]);
    for (uint32_t i = 0; i < sizeof GU2XX_DATA / sizeof GU2XX_DATA[0]; i++)
        if ((uint64_t)GU2XX_DATA[i].gu + GU2XX_DATA[i].len <= GU_SHADOW_SIZE && kernel_mapped(MAIN_BASE + GU2XX_DATA[i].xx, GU2XX_DATA[i].len))
            memcpy(G2H(GU_SHADOW + GU2XX_DATA[i].gu), G2H(MAIN_BASE + GU2XX_DATA[i].xx), GU2XX_DATA[i].len);
    for (uint32_t i = 0; i < sizeof GU2XX_WORD / sizeof GU2XX_WORD[0]; i++)
        if (GU2XX_WORD[i].gu + 4 <= GU_SHADOW_SIZE) ST32(GU_SHADOW + GU2XX_WORD[i].gu, GU2XX_WORD[i].word);
    state = 1;
    pthread_mutex_unlock(&lock);
    LOG("[forge] plugins for MHGU: %u ranges (%u bytes) of the game are shown at their MHGU 1.4.0 addresses (%08x), %d data references as GU has them\n",
        nranges, bytes, GU_SHADOW, lits);
    return true;
}

uint32_t gucompat_real(uint32_t a) {
    uint32_t gu = a - GU_SHADOW;
    const Range *r = state > 0 ? range_of_gu(gu) : NULL;
    if (r) return MAIN_BASE + r->xx + (gu - r->gu);
    if (__atomic_add_fetch(&unknown, 1, __ATOMIC_RELAXED) <= 16)
        LOG("[forge] MHGU address %x (main+%x) is not in the table of addresses for MHXX (runtime/game/gu2xx.h)\n", gu + MAIN_BASE, gu);
    return 0;
}

void gucompat_mirror(uint32_t real, uint32_t n) {
    if (state <= 0) return;
    for (uint32_t k = 0; k < n; k++) {
        const Range *r = range_of_xx(real + k - MAIN_BASE);
        if (r) ST8(GU_SHADOW + r->gu + (real + k - MAIN_BASE - r->xx), LD8(real + k));
    }
}

int gucompat_unknown(void) { return __atomic_load_n(&unknown, __ATOMIC_RELAXED); }

// ---- data. A plugin reaches the game's data the same way (the base plus GU's offset), and compares what it reads
// there with such sums (an object's vtable: "is this a monster?"). The region can't stand in for data that changes or
// for addresses the game itself holds, so the plugin's own constants are changed where it is loaded: an offset into
// GU's data becomes the one that, added to the region's base, gives the address in MHXX.
#define NGLOBAL (sizeof GU2XX_GLOBAL / sizeof GU2XX_GLOBAL[0])
static uint32_t global_xx(uint32_t gu) {
    uint32_t lo = 0, hi = NGLOBAL;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (GU2XX_GLOBAL[mid].gu <= gu) lo = mid + 1;
        else hi = mid;
    }
    if (!lo || gu > GU2XX_GLOBAL[lo - 1].last + 0x40) return 0;
    return GU2XX_GLOBAL[lo - 1].xx + (gu - GU2XX_GLOBAL[lo - 1].gu);
}

static uint32_t by_xx_global[sizeof GU2XX_GLOBAL / sizeof GU2XX_GLOBAL[0]];  // indexes into GU2XX_GLOBAL, by xx
static int cmp_global_xx(const void *a, const void *b) {
    uint32_t x = GU2XX_GLOBAL[*(const uint32_t *)a].xx, y = GU2XX_GLOBAL[*(const uint32_t *)b].xx;
    return x < y ? -1 : x > y;
}
static uint32_t global_gu(uint32_t xx) {
    static bool sorted;
    if (!sorted) {
        for (uint32_t i = 0; i < NGLOBAL; i++) by_xx_global[i] = i;
        qsort(by_xx_global, NGLOBAL, sizeof by_xx_global[0], cmp_global_xx);
        sorted = true;
    }
    uint32_t lo = 0, hi = NGLOBAL;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (GU2XX_GLOBAL[by_xx_global[mid]].xx <= xx) lo = mid + 1;
        else hi = mid;
    }
    if (!lo) return 0;
    uint32_t i = by_xx_global[lo - 1], gu = GU2XX_GLOBAL[i].gu + (xx - GU2XX_GLOBAL[i].xx);
    return gu <= GU2XX_GLOBAL[i].last + 0x40 ? gu : 0;
}

static bool shown_data(uint32_t gu) {  // (data the region shows as GU's: the build id)
    for (uint32_t i = 0; i < sizeof GU2XX_DATA / sizeof GU2XX_DATA[0]; i++)
        if (gu - GU2XX_DATA[i].gu < GU2XX_DATA[i].len) return true;
    return false;
}

int gucompat_fix_plugin(const char *name, uint32_t text, uint32_t size) {
    int n = 0;
    for (uint32_t a = text; a + 8 <= text + size; a += 4) {
        uint32_t w = LD32(a);
        if ((w & 0x0FF00000u) != 0x03000000u || (w >> 28) != 0xE) continue;  // movw rd, #lo
        uint32_t rd = (w >> 12) & 15;
        for (uint32_t b = a + 4; b < a + 20 && b + 4 <= text + size; b += 4) {
            uint32_t t = LD32(b);
            if (((t >> 12) & 15) != rd || (t >> 28) != 0xE) continue;
            if ((t & 0x0FF00000u) != 0x03400000u) break;  // (another write of rd first: not a pair)
            uint32_t v = (((t >> 4) & 0xF000u) | (t & 0xFFFu)) << 16 | ((w >> 4) & 0xF000u) | (w & 0xFFFu);
            // (a value the plugin compares with what it read from the game's code: a word as GU has it, which the
            // region shows as GU has it, not an address)
            bool compared = false;
            for (uint32_t c = b + 4; c < b + 28 && c + 4 <= text + size; c += 4) {
                uint32_t u = LD32(c);
                bool reads = ((u >> 16) & 15) == rd || (u & 15) == rd;
                if ((u & 0x0FF0F000u) == 0x01500000u && reads) { compared = true; break; }  // cmp rn, rm
                if (reads || ((u >> 12) & 15) == rd) break;
            }
            if (compared) break;
            uint32_t xx = v >= g_modules[0].text_size && !shown_data(v) ? global_xx(v) : 0;
            if (xx) {
                uint32_t nv = MAIN_BASE + xx - GU_SHADOW;
                ST32(a, (w & 0xFFF0F000u) | ((nv & 0xF000u) << 4) | (nv & 0xFFFu));
                ST32(b, (t & 0xFFF0F000u) | ((nv >> 28) << 16) | ((nv >> 16) & 0xFFFu));
                LOG("[forge] %s+%x: MHGU data main+%x is MHXX's main+%x\n", name, a - text, v, xx);
                n++;
            }
            break;
        }
    }
    return n;
}

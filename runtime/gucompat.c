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
    for (uint32_t i = 0; i < sizeof GU2XX_DATA / sizeof GU2XX_DATA[0]; i++)
        if ((uint64_t)GU2XX_DATA[i].gu + GU2XX_DATA[i].len <= GU_SHADOW_SIZE && kernel_mapped(MAIN_BASE + GU2XX_DATA[i].xx, GU2XX_DATA[i].len))
            memcpy(G2H(GU_SHADOW + GU2XX_DATA[i].gu), G2H(MAIN_BASE + GU2XX_DATA[i].xx), GU2XX_DATA[i].len);
    for (uint32_t i = 0; i < sizeof GU2XX_WORD / sizeof GU2XX_WORD[0]; i++)
        if (GU2XX_WORD[i].gu + 4 <= GU_SHADOW_SIZE) ST32(GU_SHADOW + GU2XX_WORD[i].gu, GU2XX_WORD[i].word);
    state = 1;
    pthread_mutex_unlock(&lock);
    LOG("[forge] plugins for MHGU: %u ranges (%u bytes) of the game are shown at their MHGU 1.4.0 addresses (%08x)\n",
        nranges, bytes, GU_SHADOW);
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

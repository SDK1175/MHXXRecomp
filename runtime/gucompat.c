// MHGU 1.4.0 plugins in MHXX 1.5.1: see gucompat.h. The table (game/gu2xx.h) pairs addresses of the two games, one
// per function: the code is the same and sits elsewhere, and so do the functions it calls. A region as large as GU's
// image holds each known function at GU's address, its calls out of the function written as GU has them (the target
// at GU's address), so a plugin that checks the code it is about to hook finds what it expects.
#include "gucompat.h"
#include "game/gu2xx.h"
#include "rt.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define NCODE (sizeof GU2XX_CODE / sizeof GU2XX_CODE[0])
typedef struct { uint32_t gu, xx, len; } Range;  // (offsets from the bases)
static Range ranges[NCODE];
static uint32_t nranges;
static int state;  // 0: not built; 1: built; -1: nothing to build
static int unknown;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static bool is_branch(uint32_t w) { return (w & 0x0E000000u) == 0x0A000000u && (w >> 28) != 0xF; }

static const Range *range_of_xx(uint32_t xx) {
    for (uint32_t i = 0; i < nranges; i++)
        if (xx - ranges[i].xx < ranges[i].len) return &ranges[i];
    return NULL;
}

bool gucompat_build(void) {
    pthread_mutex_lock(&lock);
    if (state) { pthread_mutex_unlock(&lock); return state > 0; }
    for (uint32_t i = 0; i < NCODE; i++) {
        uint32_t next, xx = MAIN_BASE + GU2XX_CODE[i].xx, entry = dispatch_function_at(xx, &next);
        if (!entry || !next || next <= xx) continue;
        Range r = {GU2XX_CODE[i].gu - (xx - entry), entry - MAIN_BASE, next - entry};
        if ((uint64_t)r.gu + r.len > GU_SHADOW_SIZE || range_of_xx(r.xx)) continue;
        ranges[nranges++] = r;
    }
    if (!nranges) { state = -1; pthread_mutex_unlock(&lock); return false; }
    plat_commit(GUEST_BASE + GU_SHADOW, GU_SHADOW_SIZE);
    kernel_map_region(GU_SHADOW, GU_SHADOW_SIZE, 3, 5);
    uint32_t bytes = 0;
    for (uint32_t i = 0; i < nranges; i++) {
        const Range *r = &ranges[i];
        bytes += r->len;
        for (uint32_t o = 0; o < r->len; o += 4) {
            uint32_t w = LD32(MAIN_BASE + r->xx + o);
            if (is_branch(w)) {  // a call or jump out of the function: to GU's address of the target, when it is known
                uint32_t t = r->xx + o + 8 + (uint32_t)((int32_t)(w << 8) >> 6);
                const Range *tr = t - r->xx < r->len ? NULL : range_of_xx(t);
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
    LOG("[forge] plugins for MHGU: %u functions (%u bytes) of the game are shown at their MHGU 1.4.0 addresses (%08x)\n",
        nranges, bytes, GU_SHADOW);
    return true;
}

uint32_t gucompat_real(uint32_t a) {
    uint32_t gu = a - GU_SHADOW;
    if (state > 0)
        for (uint32_t i = 0; i < nranges; i++)
            if (gu - ranges[i].gu < ranges[i].len) return MAIN_BASE + ranges[i].xx + (gu - ranges[i].gu);
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

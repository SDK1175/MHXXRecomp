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

// ---- the game's imports. A plugin may find an import's slot (main's own table of the functions it calls in the SDK)
// by the module's header, as the loader does: the NSO's MOD0 (its offset at +4), the dynamic section, the symbols and
// the PLT relocations, and put its own function there (MHGU Utility: the controller reads, nn::hid::GetNpadState).
// The region shows such a header (at IMPORTS, past GU's code and data) for the imports below, each slot holding what
// MHXX's has; when a plugin's forge_onInit has written one, MHXX's slot gets it (gucompat_imports_sync), so the game's
// calls go to the plugin, whose own call of the slot's old value goes on to the SDK.
#define IMPORTS 0x017F0000u  // (in the region, below GU_SHADOW_SIZE: GU's code ends at 0x13f0000, its data at 0x172e600)
static const char *const IMPORT_NAMES[] = {
    "_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj",
    "_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj",
    "_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj",
    "_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj",
    "_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj",
};
#define NIMPORTS (sizeof IMPORT_NAMES / sizeof IMPORT_NAMES[0])
static uint32_t imp_xx[NIMPORTS], imp_shown[NIMPORTS];  // MHXX's slot (0: none), what the region's slot was last
// A plugin may also take GU 1.4.0's slots at their fixed places (base + offset: MHGU Skill Simulator), past the region's
// end: that page is shown too, the slots in it holding what MHXX's do, and kept in step the same way.
static const uint32_t GU_GOT[NIMPORTS] = {0x1838494, 0x1838498, 0x183849c, 0x18384a0, 0x18384a4};
#define GU_GOT_PAGE 0x01838000u

static void imports_show(void) {
    const Module *m = &g_modules[0];
    uint32_t jmprel = 0, pltsz = 0;
    for (uint32_t d = m->dynamic; d && LD32(d); d += 8) {
        if (LD32(d) == 23) jmprel = m->base + LD32(d + 4);  // DT_JMPREL
        if (LD32(d) == 2) pltsz = LD32(d + 4);              // DT_PLTRELSZ
    }
    int found = 0;
    for (uint32_t i = 0; jmprel && i < pltsz / 8 && m->symtab && m->strtab; i++) {
        uint32_t off = LD32(jmprel + 8 * i), info = LD32(jmprel + 8 * i + 4), si = info >> 8;
        if ((info & 0xFF) != 22 || si >= m->nsyms) continue;  // R_ARM_JUMP_SLOT
        uint32_t name = LD32(m->symtab + 16 * si);
        if (name >= m->strsz) continue;
        const char *n = G2H(m->strtab + name);
        for (uint32_t k = 0; k < NIMPORTS; k++)
            if (!imp_xx[k] && !strcmp(n, IMPORT_NAMES[k])) imp_xx[k] = m->base + off, found++;
    }
    // the header: MOD0 at IMPORTS, the dynamic section at +0x40, the symbols at +0x100, their names at +0x200, the
    // relocations at +0x800, the slots at +0x900 (offsets from the region's base, as a module's are from its own)
    const uint32_t mod0 = IMPORTS, dyn = mod0 + 0x40, sym = mod0 + 0x100, str = mod0 + 0x200, rel = mod0 + 0x800,
                   slot = mod0 + 0x900;
    ST32(GU_SHADOW + 4, mod0);
    memcpy(G2H(GU_SHADOW + mod0), "MOD0", 4);
    ST32(GU_SHADOW + mod0 + 4, dyn - mod0);
    uint32_t tags[][2] = {{5, str}, {6, sym}, {23, rel}, {2, 0}, {20, 17}, {0, 0}};  // STRTAB SYMTAB JMPREL PLTRELSZ PLTREL
    uint32_t nrel = 0, at = 1;
    ST8(GU_SHADOW + str, 0);
    for (uint32_t k = 0; k < NIMPORTS; k++) {
        if (!imp_xx[k]) continue;
        size_t l = strlen(IMPORT_NAMES[k]) + 1;
        memcpy(G2H(GU_SHADOW + str + at), IMPORT_NAMES[k], l);
        ST32(GU_SHADOW + sym + 16 * (nrel + 1), at);  // (st_name; the rest 0)
        at += (uint32_t)l;
        ST32(GU_SHADOW + rel + 8 * nrel, slot + 4 * k);
        ST32(GU_SHADOW + rel + 8 * nrel + 4, (nrel + 1) << 8 | 22);
        imp_shown[k] = LD32(imp_xx[k]);
        ST32(GU_SHADOW + slot + 4 * k, imp_shown[k]);
        ST32(GU_SHADOW + GU_GOT[k], imp_shown[k]);
        nrel++;
    }
    tags[3][1] = nrel * 8;
    for (uint32_t i = 0; i < sizeof tags / sizeof tags[0]; i++) ST32(GU_SHADOW + dyn + 8 * i, tags[i][0]), ST32(GU_SHADOW + dyn + 8 * i + 4, tags[i][1]);
    if (found) LOG("[forge] plugins for MHGU: %d of the game's controller imports shown where a plugin looks for them\n", found);
}

void gucompat_imports_sync(const char *plugin) {
    if (state <= 0) return;
    for (uint32_t k = 0; k < NIMPORTS; k++) {
        if (!imp_xx[k]) continue;
        uint32_t v = LD32(GU_SHADOW + IMPORTS + 0x900 + 4 * k);
        if (v == imp_shown[k]) v = LD32(GU_SHADOW + GU_GOT[k]);
        if (v == imp_shown[k]) continue;
        LOG("[forge] %s takes the game's import %s (its slot: %08x -> %08x)\n", plugin, IMPORT_NAMES[k], LD32(imp_xx[k]), v);
        ST32(imp_xx[k], v);
        imp_shown[k] = v;
        ST32(GU_SHADOW + IMPORTS + 0x900 + 4 * k, v), ST32(GU_SHADOW + GU_GOT[k], v);  // (both show it now)
    }
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
    plat_commit(GUEST_BASE + GU_SHADOW + GU_GOT_PAGE, 0x1000);  // (GU's import slots: imports_show)
    kernel_map_region(GU_SHADOW + GU_GOT_PAGE, 0x1000, 4, 3);
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
    imports_show();
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

// The word at this GU address in the region is GU's own (GU2XX_WORD), not MHXX's code shown at it
bool gucompat_shown_as_gu(uint32_t a) {
    for (uint32_t i = 0; i < sizeof GU2XX_WORD / sizeof GU2XX_WORD[0]; i++)
        if (GU2XX_WORD[i].gu == a - GU_SHADOW) return true;
    return false;
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

// The address in MHXX of GU's data at main + gu, 0: not one the table knows (mhxx_gu_data: for a plugin that computes
// the address at run time, which gucompat_fix_plugin can't find in its code)
uint32_t gucompat_data(uint32_t gu) {
    uint32_t xx = global_xx(gu);
    return xx ? MAIN_BASE + xx : 0;
}

// The address in MHXX of GU's code at main + gu, 0: not in the table (mhxx_gu_code: for a plugin that compares a
// function pointer it read from the game, MHXX's real one, with one of GU's)
uint32_t gucompat_code(uint32_t gu) {
    const Range *r = range_of_gu(gu);
    return r ? MAIN_BASE + r->xx + (gu - r->gu) : 0;
}

static bool shown_data(uint32_t gu) {  // (data the region shows as GU's: the build id)
    for (uint32_t i = 0; i < sizeof GU2XX_DATA / sizeof GU2XX_DATA[0]; i++)
        if (gu - GU2XX_DATA[i].gu < GU2XX_DATA[i].len) return true;
    return false;
}

// ---- GU code pointers a plugin compares with pointers it read from the game (a vtable's slots: the IAI SLASH HELM
// BREAKER mod checks 30 of them before it hooks anything). It builds them as its main + a GU code offset, and main is
// the region (GU's code as GU has it), while the game's vtables hold MHXX's real code: they never matched. Such an
// offset is given so that main + it is MHXX's twin of that code. Told apart from a hook target or a read of GU's code
// (which want the region) by what is done with it: added to another register (the main) and then only compared, never
// read through. The plugin's two forms:
//   movw / movt rd, #off; add ra, rbase, rd; [add rb, ra, #imm;] cmp rx, ra|rb    -> off changed
//   add ra, rbase, #i1; add ra, ra, #i2; cmp rx, ra (rbase a main of the first form) -> movw / movt ra, #real
static int arm_reg_written(uint32_t u) {  // the register an instruction writes, or -1 (data processing, loads)
    if ((u >> 28) == 0xF) return -1;
    if ((u & 0x0C000000u) == 0x00000000u) {
        if ((u & 0x0F900000u) == 0x01100000u) return -1;  // (tst teq cmp cmn)
        if ((u & 0x0E000090u) == 0x00000090u && (u & 0x60u)) return (u & 0x00100000u) ? (int)((u >> 12) & 15) : -1;
        return (int)((u >> 12) & 15);
    }
    if ((u & 0x0C000000u) == 0x04000000u) return (u & 0x00100000u) ? (int)((u >> 12) & 15) : -1;  // ldr / str
    return -1;
}
static uint32_t arm_imm(uint32_t u) {
    uint32_t v = u & 0xFF, rot = ((u >> 8) & 15) * 2;
    return rot ? (v >> rot) | (v << (32 - rot)) : v;
}
// main + gu as MHXX's real code, 0: not mapped
static uint32_t real_code(uint32_t gu) {
    const Range *r = range_of_gu(gu);
    return r ? MAIN_BASE + r->xx + (gu - r->gu) : 0;
}
static int fix_code_pointers(const char *name, uint32_t text, uint32_t size) {
    int n = 0;
    uint32_t bases[16] = {0};  // a register used as the main: the last address it was seen at (for the second form)
    for (uint32_t a = text; a + 8 <= text + size; a += 4) {
        uint32_t w = LD32(a);
        if ((w & 0x0FF00000u) != 0x03000000u || (w >> 28) != 0xE) continue;  // movw rd, #lo
        uint32_t rd = (w >> 12) & 15, b = 0, t = 0;
        for (uint32_t c = a + 4; c < a + 20 && c + 4 <= text + size; c += 4) {
            uint32_t u = LD32(c);
            if (arm_reg_written(u) != (int)rd) continue;
            if ((u & 0x0FF00000u) == 0x03400000u && (u >> 28) == 0xE) b = c, t = u;
            break;
        }
        if (!b) continue;
        uint32_t v = (((t >> 4) & 0xF000u) | (t & 0xFFFu)) << 16 | ((w >> 4) & 0xF000u) | (w & 0xFFFu);
        if (v < 0x10000 || v >= g_modules[0].text_size || !range_of_gu(v)) continue;
        int32_t off[16];
        bool in[16] = {0}, added = false, bad = false;
        int cmps = 0, base = -1;
        int32_t seen[8];
        in[rd] = true, off[rd] = 0;
        for (uint32_t c = b + 4; c < b + 4 + 4 * 40 && c + 4 <= text + size && !bad; c += 4) {
            uint32_t u = LD32(c);
            if ((u & 0x0E000000u) == 0x0A000000u || (u & 0x0FFFFFF0u) == 0x012FFF10u) break;  // (a branch: as far as it goes)
            uint32_t un = (u >> 16) & 15, um = u & 15, ud = (u >> 12) & 15;
            if ((u & 0x0FF0F010u) == 0x01500000u && !(u & 0xFF0u)) {  // cmp rn, rm
                int r = in[un] ? (int)un : in[um] ? (int)um : -1;
                if (r >= 0 && cmps < 8) seen[cmps++] = off[r];
                continue;
            }
            if ((u & 0x0FE00FF0u) == 0x00800000u && (in[un] != in[um])) {  // add rd, rn, rm (one of them the offset)
                off[ud] = off[in[un] ? un : um], in[ud] = true, added = true;
                base = in[un] ? (int)um : (int)un;
                continue;
            }
            if ((u & 0x0FE00000u) == 0x02800000u && in[un] && (u >> 28) == 0xE) {  // add rd, rn, #imm
                off[ud] = off[un] + (int32_t)arm_imm(u), in[ud] = true;
                continue;
            }
            if ((u & 0x0C000000u) == 0x04000000u && (in[un] || (!(u & 0x02000000u) ? false : in[um]))) bad = true;
            if ((u & 0x0E000090u) == 0x00000090u && (u & 0x60u) && in[un]) bad = true;  // (read through: GU's code)
            int wr = arm_reg_written(u);
            if (wr >= 0) in[wr] = false;
            bool any = false;
            for (int r = 0; r < 16; r++) any |= in[r];
            if (!any) break;
        }
        if (bad || !added || !cmps || base < 0) continue;
        // (one twin for all the offsets compared: the same range)
        uint32_t real = real_code(v + (uint32_t)seen[0]) - (uint32_t)seen[0];
        bool same = real_code(v + (uint32_t)seen[0]) != 0;
        for (int k = 1; k < cmps; k++) same &= real_code(v + (uint32_t)seen[k]) - (uint32_t)seen[k] == real;
        if (!same) continue;
        uint32_t nv = real - GU_SHADOW;
        ST32(a, (w & 0xFFF0F000u) | ((nv & 0xF000u) << 4) | (nv & 0xFFFu));
        ST32(b, (t & 0xFFF0F000u) | ((nv >> 28) << 16) | ((nv >> 16) & 0xFFFu));
        bases[base] = a;
        LOG("[forge] %s+%x: GU code main+%x compared as a pointer: MHXX's main+%x\n", name, a - text, v, real - MAIN_BASE);
        n++;
    }
    // the second form: two adds of immediates to a main seen above (within 0x400 bytes), then compared
    for (uint32_t a = text; a + 12 <= text + size; a += 4) {
        uint32_t u1 = LD32(a), u2 = LD32(a + 4);
        if ((u1 & 0x0FE00000u) != 0x02800000u || (u1 >> 28) != 0xE) continue;
        uint32_t ra = (u1 >> 12) & 15, rb = (u1 >> 16) & 15;
        if (!bases[rb] || a - bases[rb] > 0x400 || ra == 15 || rb == 15) continue;
        if ((u2 & 0x0FE00000u) != 0x02800000u || ((u2 >> 16) & 15) != ra || ((u2 >> 12) & 15) != ra || (u2 >> 28) != 0xE) continue;
        uint32_t k = arm_imm(u1) + arm_imm(u2), real = k < g_modules[0].text_size ? real_code(k) : 0;
        if (!real) continue;
        bool compared = false;
        for (uint32_t c = a + 8; c < a + 24 && c + 4 <= text + size; c += 4) {
            uint32_t u = LD32(c);
            if ((u & 0x0FF0F010u) == 0x01500000u && !(u & 0xFF0u) && (((u >> 16) & 15) == ra || (u & 15) == ra)) {
                compared = true;
                break;
            }
            if (arm_reg_written(u) == (int)ra || ((u >> 16) & 15) == ra || (u & 15) == ra) break;
        }
        if (!compared) continue;
        ST32(a, 0xE3000000u | (ra << 12) | ((real & 0xF000u) << 4) | (real & 0xFFFu));         // movw ra, #lo
        ST32(a + 4, 0xE3400000u | (ra << 12) | ((real >> 28) << 16) | ((real >> 16) & 0xFFFu));  // movt ra, #hi
        LOG("[forge] %s+%x: GU code main+%x compared as a pointer: MHXX's main+%x\n", name, a - text, k, real - MAIN_BASE);
        n++;
    }
    return n;
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
            // (the base of a field read right after: add ra, rd, base; ldr rb, [ra, #imm]. GU's word at v + imm is
            // what the plugin reads, and MHXX may keep it elsewhere than xx + imm: FasterFieldHunter reads the import
            // slot 0x1bc past one it names, and MHXX's import slots are laid out otherwise)
            int32_t field = 0;
            for (uint32_t c = b + 4, ra = 16; xx && c < b + 40 && c + 4 <= text + size; c += 4) {
                uint32_t u = LD32(c);
                if ((u >> 28) != 0xE) continue;
                if (ra == 16 && (u & 0x0FE00FF0u) == 0x00800000u && (((u >> 16) & 15) == rd || (u & 15) == rd)) {
                    ra = (u >> 12) & 15;  // add ra, rn, rm
                    continue;
                }
                if (ra != 16 && (u & 0x0F500000u) == 0x05100000u && ((u >> 16) & 15) == ra) {  // ldr rb, [ra, #imm]
                    field = (int32_t)(u & 0xFFFu) * ((u & 0x00800000u) ? 1 : -1);
                    break;
                }
                if (ra != 16 && ((u >> 12) & 15) == ra) break;
            }
            if (field) {
                uint32_t at = global_xx(v + (uint32_t)field);
                if (at && at != xx + (uint32_t)field) {
                    LOG("[forge] %s+%x: the word %+d past MHGU data main+%x is at MHXX's main+%x\n", name, a - text,
                        field, v, at);
                    xx = at - (uint32_t)field;
                }
            }
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
    return n + fix_code_pointers(name, text, size);
}

// A plugin's checks of GU's code, before its forge_onLoad: records of a GU code offset and the 32 bytes the plugin
// expects there (Key Quest Markers, World Sharpness Gauge, and the like keep them in their read-only data). One that
// nearly matches what the region shows (4 to 7 of 8 words) is logged with the words that differ: why a plugin says
// "code mismatch".
void gucompat_check_signatures(const char *plugin, uint32_t ro, uint32_t len) {
    if (state <= 0) return;
    int logged = 0, checked = 0;
    for (uint32_t a = ro; a + 36 <= ro + len && logged < 12; a += 4) {
        uint32_t off = LD32(a);
        if (off < 0x10000 || off >= 0x13f2000 || (off & 3) || off + 32 > GU_SHADOW_SIZE) continue;
        int same = 0;
        for (int k = 0; k < 8; k++) same += LD32(a + 4 + 4 * k) == LD32(GU_SHADOW + off + 4 * k);
        if (same == 8) { checked++; continue; }
        // (only code this port maps and words that read as ARM code: a plugin's other tables only look like this)
        if (same < 4 || !range_of_gu(off) || (LD32(a + 4) >> 28) != 0xE) continue;
        char diff[200];
        int n = 0;
        for (int k = 0; k < 8 && n < (int)sizeof diff - 40; k++) {
            uint32_t want = LD32(a + 4 + 4 * k), shown = LD32(GU_SHADOW + off + 4 * k);
            if (want != shown) n += snprintf(diff + n, sizeof diff - n, " +%d %08x/%08x", 4 * k, want, shown);
        }
        const Range *r = range_of_gu(off);
        LOG("[forge] %s expects GU's code at main+%x (MHXX main+%x): %d of 8 words differ (wanted/shown):%s\n", plugin,
            off, r ? r->xx + (off - r->gu) : 0, 8 - same, diff);
        logged++;
    }
    if (checked || logged) LOG("[forge] %s: %d checks of GU's code match, %d differ (above)\n", plugin, checked, logged);
}

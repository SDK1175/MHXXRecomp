// The smithy's weapon trees (work in progress): the game's own tables, read where the game reads them, and the names
// the game shows; a panel over the smithy (runtime/smithytree_ui.cpp) while a weapon list is open. MHXX_SMITHY_DUMP=
// <weapon type 0-14>: a type's whole tree in the log.
//
// The tables (nativeNX/table, loose files of the game; a mod's copy if one replaces them):
//   weaponNNBaseData.wNNd   8 bytes of header (version, count), then 33 bytes a weapon: +0 its number (u32)
//   weaponNNLevelData.wNNd  8 bytes, then 15 bytes a level: +4 the weapon, +5 the level, +8 attack (u16), +14 which of
//                           the weapon's names (0, 1, 2: it is renamed at some levels)
//   weaponCreateWNN.wcd     8 bytes, then 38 bytes a recipe: +0 the kind (1: the usual one; 2-8: another way to make
//                           it), +2 the weapon, +4 the weapon it is made from (0: from materials alone), +6 four
//                           materials (item u16, count u8, 1: a key material - u8), +26 two more (item u16, count u16)
//   weaponProcessWNN.wpd    8 bytes, then 32 bytes a level up: +0 the weapon, +2 the level it reaches, +4 four
//                           materials (item u16, count u8)
// The names: the game's message resource table\weaponNNMsgData_jpn (its strings: six a weapon, from the weapon's
// number x 6: name, text, name 2, text 2, name 3, text 3), as loaded - with the Korean patch, Hangul in the patch's
// codes, turned back into Hangul here.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "romfs.h"
#include "rt.h"
#include "smithytree.h"

#define TYPES 15
#define GMD_VTABLE 0x17b6b74u  // rGUIMessage's (main+): the resources the names are looked for in

uint32_t hangulfont_decode(const char *in, char *out, uint32_t size);

typedef struct {
    uint8_t *base, *level, *create, *process;
    uint32_t nbase, nlevel, ncreate, nprocess;
    uint32_t names;  // the message resource (guest), 0 not found yet
} Type;
static Type g_type[TYPES];

static uint32_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static uint8_t *table(const char *name, uint32_t stride, uint32_t *count) {
    char rpath[128];
    snprintf(rpath, sizeof rpath, "nativeNX/table/%s", name);
    const char *hp = romfs_host_path(rpath);
    FILE *f = hp ? fopen(hp, "rb") : NULL;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = n >= 8 ? malloc((size_t)n) : NULL;
    bool ok = d && fread(d, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    *count = ok ? rd32(d + 4) : 0;
    if (!ok || 8 + (uint64_t)*count * stride > (uint64_t)n) {
        LOG("[smithytree] %s: not as expected (%ld bytes, %u entries of %u)\n", name, n, *count, stride);
        free(d), *count = 0;
        return NULL;
    }
    return d;
}

static bool load(int t) {
    Type *ty = &g_type[t];
    if (ty->base) return true;
    char n[64];
    snprintf(n, sizeof n, "weapon%02dBaseData.w%02dd", t, t), ty->base = table(n, 33, &ty->nbase);
    snprintf(n, sizeof n, "weapon%02dLevelData.w%02dd", t, t), ty->level = table(n, 15, &ty->nlevel);
    snprintf(n, sizeof n, "weaponCreateW%02d.wcd", t), ty->create = table(n, 38, &ty->ncreate);
    snprintf(n, sizeof n, "weaponProcessW%02d.wpd", t), ty->process = table(n, 32, &ty->nprocess);
    return ty->base && ty->level && ty->create && ty->process;
}

// The game's message resource "table\\weaponNNMsgData_jpn": an rGUIMessage, its path at +8, its strings' count at
// +0x88, their bytes at +0x90, the text at +0x94 and their pointers at +0x98. Looked for in the heap (once a type).
static uint32_t find_names(int t) {
    char want[40];
    snprintf(want, sizeof want, "table\\weapon%02dMsgData_jpn", t);
    uint32_t vt = MAIN_BASE + GMD_VTABLE;
    for (uint64_t page = 0x08000000; page < 0x40000000; page += 0x10000) {
        if (!kernel_mapped(page, 0x10000)) continue;
        for (uint32_t a = (uint32_t)page; a < page + 0x10000 - 0xa8; a += 4) {
            if (LD32(a) != vt || strcmp((const char *)G2H(a + 8), want)) continue;
            uint32_t n = LD32(a + 0x88);
            if (!n || n >= 0x10000) continue;
            LOG("[smithytree] %s at %08x: %u strings (%u bytes; text %08x, pointers %08x)\n", want, a, n, LD32(a + 0x90),
                LD32(a + 0x94), LD32(a + 0x98));
            return a;
        }
    }
    return 0;
}

static const char *name_of(int t, uint32_t weapon, int which) {
    static char out[4][192];
    static int k;
    Type *ty = &g_type[t];
    uint32_t i = weapon * 6 + (uint32_t)which * 2, n = ty->names ? LD32(ty->names + 0x88) : 0;
    if (i >= n) return "?";
    // (the text at +0x94, the strings one after another; +0x98 the array of their pointers, used when it points into it)
    uint32_t data = LD32(ty->names + 0x94), arr = LD32(ty->names + 0x98), size = LD32(ty->names + 0x90), s = 0;
    if (kernel_mapped(arr, n * 4) && LD32(arr) - data < size) s = LD32(arr + 4 * i);
    else if (kernel_mapped(data, size)) {
        s = data;
        for (uint32_t k2 = 0; k2 < i && s < data + size; k2++) s += (uint32_t)strlen((const char *)G2H(s)) + 1;
    }
    if (!s || !kernel_mapped(s, 1)) return "?";
    char *o = out[k++ & 3];
    if (!hangulfont_decode((const char *)G2H(s), o, sizeof out[0])) snprintf(o, sizeof out[0], "%.180s", (const char *)G2H(s));
    for (char *c = o; *c; c++)
        if (*c == '\n') *c = ' ';
    return o;
}

static void level_range(int t, uint32_t weapon, int *maxlv, int *attack) {
    Type *ty = &g_type[t];
    *maxlv = 0, *attack = 0;
    for (uint32_t i = 0; i < ty->nlevel; i++) {
        const uint8_t *e = ty->level + 8 + 15 * i;
        if (e[4] == weapon && e[5] > *maxlv) *maxlv = e[5], *attack = (int)rd16(e + 8);
    }
}

static void dump_branch(int t, uint32_t weapon, int depth, uint8_t *seen) {
    Type *ty = &g_type[t];
    if (weapon >= 256 || seen[weapon]) return;
    seen[weapon] = 1;
    int maxlv, attack;
    level_range(t, weapon, &maxlv, &attack);
    LOG("[smithytree] %*s%s (%u): Lv%d, attack %d at the top\n", depth * 2, "", name_of(t, weapon, 0), weapon, maxlv, attack);
    for (uint32_t i = 0; i < ty->ncreate; i++) {
        const uint8_t *e = ty->create + 8 + 38 * i;
        if (rd16(e) == 1 && rd16(e + 4) == weapon && rd16(e + 2) != weapon) dump_branch(t, rd16(e + 2), depth + 1, seen);
    }
}

static void dump(int t) {
    Type *ty = &g_type[t];
    {  // (so one run says how the strings are kept, whichever way it is: the array's first entries and what they point at)
        uint32_t arr = LD32(ty->names + 0x98), data = LD32(ty->names + 0x94);
        for (uint32_t i = 0; i < 8 && kernel_mapped(arr + 4 * i, 4); i++) {
            uint32_t p = LD32(arr + 4 * i);
            char raw[40] = "";
            if (kernel_mapped(p, 16)) for (int k = 0; k < 12; k++) snprintf(raw + 3 * k, 4, "%02x ", LD8(p + (uint32_t)k));
            LOG("[smithytree] string %u: pointer %08x (text +%x): %s\n", i, p, p - data, raw);
        }
    }
    LOG("[smithytree] weapon type %d: %u weapons, %u levels, %u recipes, %u level ups; names %08x (%u strings)\n", t,
        ty->nbase, ty->nlevel, ty->ncreate, ty->nprocess, ty->names, ty->names ? LD32(ty->names + 0x88) : 0);
    uint8_t seen[256] = {0};
    for (uint32_t i = 0; i < ty->ncreate; i++) {  // (the roots: made from materials alone)
        const uint8_t *e = ty->create + 8 + 38 * i;
        uint32_t from = rd16(e + 4), w = rd16(e + 2);
        if (rd16(e) == 1 && (!from || from == w)) dump_branch(t, w, 0, seen);
    }
    for (uint32_t i = 0; i < ty->ncreate; i++) {  // (made from a weapon no recipe makes: listed apart)
        const uint8_t *e = ty->create + 8 + 38 * i;
        uint32_t w = rd16(e + 2);
        if (rd16(e) == 1 && w < 256 && !seen[w]) LOG("[smithytree] (from %u, not reached)\n", rd16(e + 4)), dump_branch(t, w, 1, seen);
    }
}

// (debug) MHXX_SMITHY_DUMP=<type>: the tree of that weapon type in the log, once its names are loaded (tried every
// 5 s: the game loads them at the title screen)
void smithytree_frame(void) {
    static int t = -2;
    static uint64_t next;
    if (t == -2) {
        const char *e = getenv("MHXX_SMITHY_DUMP");
        t = e && *e ? atoi(e) : -1;
        if (t >= TYPES || t == 5) t = -1;
    }
    if (t < 0) return;
    uint64_t now = plat_now_ns();
    if (now < next) return;
    next = now + 5000000000ull;
    if (!load(t)) { LOG("[smithytree] weapon type %d: the tables couldn't be read\n", t); t = -1; return; }
    if (!(g_type[t].names = find_names(t))) return;
    dump(t);
    t = -1;
}

// ------------------------------------------------------------------ the smithy screen
// Its object's update, every frame the smithy is open (GU 0x6f1ae8, found by MHGU Utility's smithy preview): +4 / +5
// the screen (7 / 25: the upgrade list), +0x5c the category (a weapon type + 7), +0x1f71 the cursor, +0x1f78 the
// rows, from +0x28d4 0x30 bytes a row: +0 flags (0x210: hidden), +2 the weapon, +9 its level - 1.
#define FN_SMITHY_UPDATE 0x6e67d8u

static struct {
    uint64_t at;  // when last seen (ns); 0 never
    uint32_t obj;
    uint8_t screen, sub, cursor;
    uint16_t category, rows, weapon, level;
} g_seen;

static void smithy_update(Cpu *c) {
    uint32_t fn = MAIN_BASE + FN_SMITHY_UPDATE, obj = c->r[0];
    fnhook_call_original(c, fn);
    if (obj < 0x08000000 || (obj & 3) || !kernel_mapped(obj, 0x2a00)) return;
    uint8_t screen = LD8(obj + 4), sub = LD8(obj + 5), cursor = LD8(obj + 0x1f71);
    uint16_t category = LD16(obj + 0x5c), rows = LD16(obj + 0x1f78), weapon = 0, level = 0;
    if (cursor < rows && rows <= 64) {
        uint32_t row = obj + 0x28d4 + cursor * 0x30u;
        if (!(LD16(row) & 0x210)) weapon = LD16(row + 2), level = (uint16_t)(LD8(row + 9) + 1);
    }
    if (screen != g_seen.screen || sub != g_seen.sub || category != g_seen.category || weapon != g_seen.weapon ||
        level != g_seen.level)
        LOG("[smithytree] smithy: screen %u/%u, category %u, row %u of %u: weapon %u Lv%u\n", screen, sub, category,
            cursor, rows, weapon, level);
    g_seen.at = plat_now_ns(), g_seen.obj = obj, g_seen.screen = screen, g_seen.sub = sub, g_seen.cursor = cursor;
    g_seen.category = category, g_seen.rows = rows, g_seen.weapon = weapon, g_seen.level = level;
}

void smithytree_init(void) {
    fnhook_set(MAIN_BASE + FN_SMITHY_UPDATE, hle_register("smithytree:update", smithy_update));
}

// What the panel shows: the weapon under the smithy's cursor, its levels by name, where it is made from, what is made
// from it. False: no weapon list open (the smithy not seen in the last 0.2 s), or its data not there.
bool smithytree_view(SmithyView *v) {
    memset(v, 0, sizeof *v);
    if (!g_seen.at || plat_now_ns() - g_seen.at > 200000000ull) return false;
    int cat = g_seen.category;
    if (cat < 7 || cat - 7 >= TYPES || cat - 7 == 5 || !g_seen.weapon) return false;
    int t = cat - 7;
    Type *ty = &g_type[t];
    if (!load(t)) return false;
    if (!ty->names) {
        static uint64_t tried[TYPES];
        uint64_t now = plat_now_ns();
        if (now - tried[t] < 2000000000ull) return false;  // (a heap search: not every frame)
        tried[t] = now;
        if (!(ty->names = find_names(t))) return false;
    }
    uint32_t w = g_seen.weapon;
    v->type = t, v->weapon = w, v->level = g_seen.level, v->screen = g_seen.screen, v->sub = g_seen.sub;
    for (uint32_t i = 0; i < ty->nlevel; i++) {  // (its levels, a tier for each of its names)
        const uint8_t *e = ty->level + 8 + 15 * i;
        if (e[4] != w || e[14] > 2) continue;
        SmithyTier *tr = &v->tier[e[14]];
        if (!tr->top || e[5] < tr->low) tr->low = e[5];
        if (e[5] > tr->top) tr->top = e[5], tr->attack = (int)rd16(e + 8);
        if (e[14] + 1 > v->ntiers) v->ntiers = e[14] + 1;
    }
    for (int k = 0; k < v->ntiers; k++) snprintf(v->tier[k].name, sizeof v->tier[k].name, "%s", name_of(t, w, k));
    for (uint32_t i = 0; i < ty->ncreate; i++) {  // (made from; made from it)
        const uint8_t *e = ty->create + 8 + 38 * i;
        uint32_t made = rd16(e + 2), from = rd16(e + 4);
        if (made == w && from && from != w && !v->parent) {
            v->parent = from;
            snprintf(v->parent_name, sizeof v->parent_name, "%s", name_of(t, from, 0));
        }
        if (from == w && made != w && v->nchildren < SMITHY_CHILDREN) {
            SmithyChild *ch = &v->child[v->nchildren++];
            int maxlv, attack;
            level_range(t, made, &maxlv, &attack);
            ch->weapon = made, ch->top = maxlv, ch->attack = attack;
            snprintf(ch->name, sizeof ch->name, "%s", name_of(t, made, 0));
        }
    }
    return true;
}

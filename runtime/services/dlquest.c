// Download (event / challenge) quests shown with the Korean patch's text.
//
// The Switch MHXX save keeps a copy of each downloaded quest's ARC: when a quest was downloaded, the game copied
// it from the RomFS into the shared part of `system`, and reads it from there afterwards. A save from a Switch
// without the patch has the Japanese copies, so the patch's own quest files (it replaces nativeNX/arc/quest/
// q1010001.arc .. q1120013.arc in the RomFS) never show, and the Japanese text comes out in the patch's font.
//
// Save file layout (MHXX 1.5.1 Switch, 0x481dac bytes; from a save with 173 downloads, docs/research/dl-quests.md):
// 200 slots from 0xf898, 0x1400 apart (160 event quests, then 40 challenge quests), each [u32 quest id]
// [u32 size][ARC]; an empty slot's id is 0.
//
// When the game reads `system` (or `system_backup`), each slot it reads that has a quest gets the RomFS's file
// of that quest instead, if a mod replaces it with one that fits in the slot (the game's own RomFS files are
// larger than a slot, so without a mod nothing changes). Nothing is added: an empty slot stays empty, and only
// quests the save already has are touched. The file on disk is left as it is; the game's next save writes what
// it has in memory (the patch's copies).
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../rt.h"
#include "../romfs.h"

#define SAVE_SIZE  0x481dacu
#define SLOTS_AT   0xf898u
#define SLOT_SIZE  0x1400u
#define SLOT_COUNT 200u

typedef struct { uint32_t id; uint32_t size; uint8_t *arc; } Patched;  // (arc NULL: none that fits)
static Patched g_cache[SLOT_COUNT * 2];
static int g_ncache, g_used;

// The mod's file of quest id that fits in a slot, or NULL (size in *size).
static const uint8_t *quest_arc(uint32_t id, uint32_t *size) {
    for (int i = 0; i < g_ncache; i++)
        if (g_cache[i].id == id) return *size = g_cache[i].size, g_cache[i].arc;
    Patched p = {id, 0, NULL};
    char rpath[64];
    snprintf(rpath, sizeof rpath, "nativeNX/arc/quest/q%07u.arc", id);
    const char *host = romfs_host_path(rpath);
    FILE *f = host ? fopen(host, "rb") : NULL;
    if (f) {
        uint8_t *b = malloc(SLOT_SIZE);
        size_t n = fread(b, 1, SLOT_SIZE, f);  // (one more than fits: a file that big doesn't fit)
        fclose(f);
        if (n >= 16 && n <= SLOT_SIZE - 8 && !memcmp(b, "ARC", 4)) p.arc = b, p.size = (uint32_t)n;
        else free(b);
    }
    if (g_ncache < (int)(sizeof g_cache / sizeof *g_cache)) g_cache[g_ncache++] = p;
    return *size = p.size, p.arc;
}

// After the game read [off, off+n) of a save file into buf (path: the host file, fd: its descriptor).
void dlquest_patch(const char *path, int fd, int64_t off, uint8_t *buf, size_t n) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (strcmp(base, "system") && strcmp(base, "system_backup")) return;
    struct stat sb;
    if (fstat(fd, &sb) || (uint64_t)sb.st_size != SAVE_SIZE) return;
    uint64_t a = (uint64_t)off, e = a + n;
    if (e <= SLOTS_AT || a >= SLOTS_AT + SLOT_COUNT * SLOT_SIZE) return;
    int done = 0;
    static uint8_t slot[SLOT_SIZE];
    for (uint32_t i = 0; i < SLOT_COUNT; i++) {
        uint64_t so = SLOTS_AT + (uint64_t)i * SLOT_SIZE, se = so + SLOT_SIZE;
        if (se <= a || so >= e) continue;
        uint32_t id = 0;
        if (pread(fd, &id, 4, (off_t)so) != 4 || !id) continue;
        uint32_t size = 0;
        const uint8_t *arc = quest_arc(id, &size);
        if (!arc) continue;
        memset(slot, 0, sizeof slot);
        memcpy(slot, &id, 4);
        memcpy(slot + 4, &size, 4);
        memcpy(slot + 8, arc, size);
        uint64_t from = so > a ? so : a, to = se < e ? se : e;
        memcpy(buf + (from - a), slot + (from - so), (size_t)(to - from));
        done++;
    }
    if (done && !g_used) LOG("[dlquest] %s: download quests shown with the mods' quest files (Korean patch)\n", path);
    g_used += done;
}

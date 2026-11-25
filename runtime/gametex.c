// The game's own pictures for plugins (mhxx_game_texture): a texture from an .arc in the game's files (a mod's copy if
// one replaces it), decoded here (runtime/nvn: Tegra deswizzle, BCn) and handed to ImGui as a user texture, which a
// plugin then draws with ImDrawList_AddImage by its ID (mhxx_game_texture_id). Nothing of the game is in a plugin: the
// picture comes from the player's files each run (a smithy mod's weapon and armor icons: the game's icon atlas).
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "rt.h"
#include "nvn/bcn.h"
#include "nvn/tegra.h"
#include "romfs.h"

#define ARC_TEX 0x241f5debu  // rTexture's type hash in an .arc's table

void *overlay_user_texture(const uint8_t *rgba, int w, int h);  // (overlay.cpp: ImGui's, registered)
uint64_t overlay_user_texture_id(void *tex);                    // (its ID once uploaded, else 0)

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static uint8_t *member(const uint8_t *arc, long n, const char *name, uint32_t *size) {
    if (n < 12 || memcmp(arc, "ARC", 4)) return NULL;
    uint32_t count = arc[6] | arc[7] << 8;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = arc + 12 + (uint64_t)i * 80;
        if (e + 80 > arc + n) break;
        char nm[65];
        memcpy(nm, e, 64), nm[64] = 0;
        if (rd32(e + 64) != ARC_TEX || SDL_strcasecmp(nm, name)) continue;
        uint32_t csize = rd32(e + 68), usize = rd32(e + 72) & 0x1fffffff, off = rd32(e + 76);
        if ((uint64_t)off + csize > (uint64_t)n) return NULL;
        uint8_t *out = malloc(usize ? usize : 1);
        uLongf un = usize;
        if (!out || uncompress(out, &un, arc + off, csize) != Z_OK) { free(out); return NULL; }
        *size = (uint32_t)un;
        return out;
    }
    return NULL;
}

// A TEX file's first level as RGBA (RGBA8, BC1, BC3, BC4 as grey, BC5): w * h * 4 bytes, or NULL
static uint8_t *decode(const uint8_t *t, uint32_t n, uint32_t *ow, uint32_t *oh) {
    if (n < 0x18 || memcmp(t, "TEX", 4)) return NULL;
    uint32_t d1 = rd32(t + 4), d2 = rd32(t + 8), d3 = rd32(t + 12);
    uint32_t levels = d2 & 0x3f, w = (d2 >> 6) & 0x1fff, h = (d2 >> 19) & 0x1fff, count = d3 & 0xff, code = (d3 >> 8) & 0xff;
    if (d1 >> 28 != 2 || count != 1 || !w || !h) return NULL;
    static const struct { uint8_t tex; uint32_t nvn; int bcn; } fmts[] = {
        {0x07, 0x25, 0}, {0x13, 0x42, BCN_BC1}, {0x17, 0x44, BCN_BC3}, {0x19, 0x49, BCN_BC4U}, {0x1f, 0x4b, BCN_BC5U}};
    int fi = -1;
    for (int i = 0; i < 5; i++)
        if (fmts[i].tex == code) fi = i;
    if (fi < 0) return NULL;
    TexLayout lay = {.width = w, .height = h, .depth = 1, .layers = 1, .levels = levels ? levels : 1,
                     .fmt = nvn_format_info(fmts[fi].nvn)};
    tegra_layout_init(&lay);
    LevelLayout l = tegra_level(&lay, 0);
    uint32_t bpb = lay.fmt->bpb;
    uint64_t size = (uint64_t)l.wb * l.hb * bpb, data = 0x14 + 4ull * lay.levels;
    if (data > n) return NULL;
    data += rd32(t + 0x14);
    if (data + size > n) return NULL;
    uint8_t *lin = malloc(size), *px = malloc((uint64_t)w * h * 4);
    if (!lin || !px) { free(lin), free(px); return NULL; }
    tegra_deswizzle(t + data, lin, l.wb, l.hb, 1, bpb, l.block_height_log2, l.block_depth_log2);
    if (!fmts[fi].bcn) memcpy(px, lin, (uint64_t)w * h * 4);
    else {
        uint32_t ts = bcn_texel_size(fmts[fi].bcn);
        uint8_t *d = ts == 4 ? px : malloc((uint64_t)w * h * ts);
        if (!d) { free(lin), free(px); return NULL; }
        bcn_decode(fmts[fi].bcn, lin, w, h, d);
        if (ts != 4)
            for (uint64_t i = (uint64_t)w * h; i-- > 0;) {
                uint8_t r = d[i * ts], g = ts > 1 ? d[i * ts + 1] : r, b = ts > 1 ? 0 : r;
                px[i * 4] = r, px[i * 4 + 1] = g, px[i * 4 + 2] = b, px[i * 4 + 3] = 255;
            }
        if (d != px) free(d);
    }
    free(lin);
    *ow = w, *oh = h;
    return px;
}

#define MAX_GAMETEX 16
static struct { char key[200]; void *tex; uint32_t w, h; } loaded[MAX_GAMETEX];
static int nloaded;

// The texture <name> in the .arc <arc> (a RomFS path, "nativeNX/arc/resident.arc"): a handle (1..), its size in *w,
// *h, or 0. Each is loaded once; ImGui uploads it with its next frame.
int gametex_load(const char *arc, const char *name, uint32_t *w, uint32_t *h) {
    char key[200];
    SDL_snprintf(key, sizeof key, "%s|%s", arc, name);
    for (int i = 0; i < nloaded; i++)
        if (!strcmp(loaded[i].key, key)) return *w = loaded[i].w, *h = loaded[i].h, i + 1;
    if (nloaded == MAX_GAMETEX) return 0;
    const char *path = romfs_host_path(arc);
    SDL_IOStream *io = path ? SDL_IOFromFile(path, "rb") : NULL;
    Sint64 n = io ? SDL_GetIOSize(io) : -1;
    uint8_t *a = n > 12 ? malloc((size_t)n) : NULL;
    bool ok = a && SDL_ReadIO(io, a, (size_t)n) == (size_t)n;
    if (io) SDL_CloseIO(io);
    uint32_t tn = 0, tw = 0, th = 0;
    uint8_t *t = ok ? member(a, (long)n, name, &tn) : NULL;
    uint8_t *px = t ? decode(t, tn, &tw, &th) : NULL;
    free(t), free(a);
    void *tex = px ? overlay_user_texture(px, (int)tw, (int)th) : NULL;
    free(px);
    LOG("[gametex] %s %s: %s (%ux%u)\n", arc, name, tex ? "loaded" : "not found / not decoded", tw, th);
    if (!tex) return 0;
    SDL_strlcpy(loaded[nloaded].key, key, sizeof loaded[nloaded].key);
    loaded[nloaded].tex = tex, loaded[nloaded].w = tw, loaded[nloaded].h = th;
    *w = tw, *h = th;
    return ++nloaded;
}

// A handle's ImTextureID for drawing, 0 until ImGui has uploaded it
uint64_t gametex_id(int handle) {
    return handle >= 1 && handle <= nloaded ? overlay_user_texture_id(loaded[handle - 1].tex) : 0;
}

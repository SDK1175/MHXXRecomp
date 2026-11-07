// mhxx-texextract: every texture in the game files, as PNGs named the way the texture dump names them
// ([Graphics] TextureDump, runtime/nvn/vulkan/texpack.c), without playing through the game to see each one.
//
//   mhxx-texextract <romfs folder> <output folder> [<mod romfs folder> ...]
//
// Reads every .arc under <romfs folder>/nativeNX/arc (a mod's own copy of an .arc - the Korean patch's, say - takes
// its place, the last mod given winning, as in the game), and saves each 2D texture's full-size level as
// <output>/<key>.png. <output>/index.txt says, for each: key, size, kind (color / mask / normal), the .arc and the
// texture's name inside it ("enemy\em001\em001_00\mod\em001_00_BM": em001 = the monster, _BM base colour, _NM
// normal map, ...), to choose what to upscale.
//
// The textures are stored as the Switch's GPU reads them (block-linear); key is the dump's: the hash of the level
// deswizzled to block rows, its size and its NVN format. Nothing is sent anywhere; the PNGs are for the player's own
// texture pack.
#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

#include "../../runtime/nvn/astc.h"
#include "../../runtime/nvn/bcn.h"
#include "../../runtime/nvn/tegra.h"

#ifdef _WIN32
#include <direct.h>
#define mkdir_one(p) _mkdir(p)
#else
#define mkdir_one(p) mkdir(p, 0777)
#endif

#define ARC_TEX 0x241f5debu  // rTexture's type hash in an .arc's table

// the dump's key (runtime/nvn/vulkan/texpack.c texpack_key): keep the two the same
static uint64_t texpack_key(const uint8_t *p, uint64_t n, uint32_t w, uint32_t h, uint32_t nvn_format) {
    uint64_t k = 0xcbf29ce484222325ull ^ ((uint64_t)w << 40 | (uint64_t)h << 16 | nvn_format);
    uint64_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        k = (k ^ v) * 0x100000001b3ull;
        k ^= k >> 32;
    }
    for (; i < n; i++) k = (k ^ p[i]) * 0x100000001b3ull;
    return k;
}

// ---------------------------------------------------------------- PNG (RGBA8, one IDAT, zlib)
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v; }
static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
    uint8_t hd[8];
    be32(hd, n);
    memcpy(hd + 4, type, 4);
    fwrite(hd, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    uint32_t crc = crc32(crc32(0, (const Bytef *)type, 4), data, n);
    be32(hd, crc);
    fwrite(hd, 1, 4, f);
}
static bool save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    uint64_t raw_n = (uint64_t)(w * 4 + 1) * h;
    uint8_t *raw = malloc(raw_n);
    uLongf zn = compressBound(raw_n);
    uint8_t *z = malloc(zn);
    if (!raw || !z) { free(raw), free(z); return false; }
    for (uint32_t y = 0; y < h; y++) {
        raw[(uint64_t)y * (w * 4 + 1)] = 0;
        memcpy(raw + (uint64_t)y * (w * 4 + 1) + 1, rgba + (uint64_t)y * w * 4, (size_t)w * 4);
    }
    bool ok = compress2(z, &zn, raw, raw_n, 6) == Z_OK;
    FILE *f = ok ? fopen(path, "wb") : NULL;
    if (f) {
        static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
        uint8_t ihdr[13];
        be32(ihdr, w), be32(ihdr + 4, h);
        ihdr[8] = 8, ihdr[9] = 6, ihdr[10] = ihdr[11] = ihdr[12] = 0;
        fwrite(sig, 1, 8, f);
        chunk(f, "IHDR", ihdr, 13);
        chunk(f, "IDAT", z, (uint32_t)zn);
        chunk(f, "IEND", NULL, 0);
        ok = fclose(f) == 0;
    } else ok = false;
    free(raw), free(z);
    return ok;
}

// ---------------------------------------------------------------- textures
typedef struct { uint8_t tex; uint32_t nvn; int bcn; const char *kind; } Fmt;
static const Fmt fmts[] = {
    {0x07, 0x25, 0, "color"},              // RGBA8
    {0x13, 0x42, BCN_BC1, "color"},        // BC1
    {0x17, 0x44, BCN_BC3, "color"},        // BC3
    {0x19, 0x49, BCN_BC4U, "mask"},        // BC4
    {0x1f, 0x4b, BCN_BC5U, "normal"},      // BC5 (normal maps: _NM)
};
static uint32_t unknown_fmt[256], n_tex, n_saved, n_known, n_other;
static uint64_t *seen;
static uint32_t seen_cap, seen_n;
static bool seen_add(uint64_t k) {
    if (!k) k = 1;
    if (seen_n * 2 >= seen_cap) {
        uint32_t cap = seen_cap ? seen_cap * 2 : 1 << 16;
        uint64_t *n = calloc(cap, sizeof *n);
        for (uint32_t i = 0; i < seen_cap; i++)
            if (seen[i])
                for (uint32_t j = (uint32_t)seen[i] & (cap - 1);; j = (j + 1) & (cap - 1))
                    if (!n[j]) { n[j] = seen[i]; break; }
        free(seen);
        seen = n, seen_cap = cap;
    }
    for (uint32_t j = (uint32_t)k & (seen_cap - 1);; j = (j + 1) & (seen_cap - 1)) {
        if (seen[j] == k) return false;
        if (!seen[j]) { seen[j] = k, seen_n++; return true; }
    }
}

static const char *g_out;
static FILE *g_index;

static void texture(const char *arc, const char *name, const uint8_t *t, uint64_t n) {
    if (n < 0x18 || memcmp(t, "TEX", 4)) return;
    // header: "TEX\0", three packed words, the data's size (+0x10), then each level's offset into the data (+0x14,
    // one word a level); the data follows that table (+0x18 with one level, +0x40 with eleven)
    uint32_t d1, d2, d3;
    memcpy(&d1, t + 4, 4), memcpy(&d2, t + 8, 4), memcpy(&d3, t + 12, 4);
    uint32_t levels = d2 & 0x3f, w = (d2 >> 6) & 0x1fff, h = (d2 >> 19) & 0x1fff, count = d3 & 0xff,
             code = (d3 >> 8) & 0xff, dim = d1 >> 28;
    n_tex++;
    if (dim != 2 || count != 1 || w < 16 || h < 16) { n_other++; return; }  // (cube maps, arrays, tiny ones)
    const Fmt *f = NULL;
    for (unsigned i = 0; i < sizeof fmts / sizeof *fmts; i++)
        if (fmts[i].tex == code) f = &fmts[i];
    if (!f) {
        if (!unknown_fmt[code]++) fprintf(stderr, "format %#x not known (first: %s %s)\n", code, arc, name);
        return;
    }
    TexLayout lay = {.width = w, .height = h, .depth = 1, .layers = 1, .levels = levels ? levels : 1,
                     .fmt = nvn_format_info(f->nvn)};
    tegra_layout_init(&lay);
    LevelLayout l = tegra_level(&lay, 0);
    uint32_t bpb = lay.fmt->bpb;
    uint64_t size = (uint64_t)l.wb * l.hb * bpb;
    uint32_t level0;
    uint64_t data = 0x14 + 4ull * lay.levels;
    if (data > n) return;
    memcpy(&level0, t + 0x14, 4);
    data += level0;
    if (data + size > n) {
        fprintf(stderr, "%s %s: shorter than its size says\n", arc, name);
        return;
    }
    uint8_t *lin = malloc(size);
    tegra_deswizzle(t + data, lin, l.wb, l.hb, 1, bpb, l.block_height_log2, l.block_depth_log2);
    uint64_t key = texpack_key(lin, size, w, h, f->nvn);
    n_known++;
    if (seen_add(key)) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%016llx.png", g_out, (unsigned long long)key);
        uint8_t *px = malloc((uint64_t)w * h * 4);
        if (!f->bcn) memcpy(px, lin, (uint64_t)w * h * 4);
        else {
            uint32_t ts = bcn_texel_size(f->bcn);
            uint8_t *d = ts == 4 ? px : malloc((uint64_t)w * h * ts);
            bcn_decode(f->bcn, lin, w, h, d);
            if (ts != 4)
                for (uint64_t i = (uint64_t)w * h; i-- > 0;) {
                    uint8_t r = d[i * ts], g = ts > 1 ? d[i * ts + 1] : r, b = ts > 1 ? 0 : r;
                    px[i * 4] = r, px[i * 4 + 1] = g, px[i * 4 + 2] = b, px[i * 4 + 3] = 255;
                }
            if (d != px) free(d);
        }
        if (save_png(path, px, w, h)) n_saved++;
        free(px);
        fprintf(g_index, "%016llx %ux%u %s %s %s\n", (unsigned long long)key, w, h, f->kind, arc, name);
    }
    free(lin);
}

static void arc_file(const char *path, const char *rel) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *d = malloc(n);
    if (!d || fread(d, 1, n, fp) != (size_t)n || n < 12 || memcmp(d, "ARC", 4)) { fclose(fp), free(d); return; }
    fclose(fp);
    uint16_t count;
    memcpy(&count, d + 6, 2);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = d + 12 + (uint64_t)i * 0x50;
        if (e + 0x50 > d + n) break;
        uint32_t type, csize, usize, off;
        memcpy(&type, e + 64, 4), memcpy(&csize, e + 68, 4), memcpy(&usize, e + 72, 4), memcpy(&off, e + 76, 4);
        if (type != ARC_TEX || (uint64_t)off + csize > (uint64_t)n) continue;
        uLongf un = usize & 0x1fffffff;
        uint8_t *t = malloc(un);
        if (t && uncompress(t, &un, d + off, csize) == Z_OK) {
            char name[65];
            memcpy(name, e, 64), name[64] = 0;
            texture(rel, name, t, un);
        }
        free(t);
    }
    free(d);
}

// every .arc under dir (rel: its path below nativeNX/arc), the mods' copies first
static int nmods;
static char **mods;
static void walk(const char *dir, const char *rel) {
    DIR *dd = opendir(dir);
    if (!dd) return;
    struct dirent *e;
    while ((e = readdir(dd))) {
        if (e->d_name[0] == '.') continue;
        char p[1024], r[1024];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        snprintf(r, sizeof r, "%s%s%s", rel, *rel ? "/" : "", e->d_name);
        struct stat st;
        if (stat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) { walk(p, r); continue; }
        size_t len = strlen(e->d_name);
        if (len < 4 || strcmp(e->d_name + len - 4, ".arc")) continue;
        const char *use = p;
        char mp[1024];
        for (int m = nmods - 1; m >= 0; m--) {
            snprintf(mp, sizeof mp, "%s/nativeNX/arc/%s", mods[m], r);
            if (!stat(mp, &st)) { use = mp; break; }
        }
        arc_file(use, r);
        static unsigned arcs;
        if (!(++arcs % 100)) fprintf(stderr, "%u .arc files, %u textures saved\n", arcs, n_saved);
    }
    closedir(dd);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <romfs folder> <output folder> [<mod romfs folder> ...]\n", argv[0]);
        return 2;
    }
    g_out = argv[2];
    mkdir_one(g_out);
    mods = argv + 3, nmods = argc - 3;
    char idx[1024], arcdir[1024];
    snprintf(idx, sizeof idx, "%s/index.txt", g_out);
    g_index = fopen(idx, "w");
    if (!g_index) { fprintf(stderr, "can't write %s\n", idx); return 1; }
    snprintf(arcdir, sizeof arcdir, "%s/nativeNX/arc", argv[1]);
    struct stat st;
    if (stat(arcdir, &st)) { fprintf(stderr, "%s not found: give the folder that holds nativeNX\n", arcdir); return 1; }
    walk(arcdir, "");
    fclose(g_index);
    fprintf(stderr, "done: %u textures, %u saved (%u repeated), %u cube maps / arrays / tiny ones skipped\n", n_tex,
            n_saved, n_known - seen_n, n_other);
    for (int i = 0; i < 256; i++)
        if (unknown_fmt[i]) fprintf(stderr, "  format %#x: %u textures not saved\n", i, unknown_fmt[i]);
    return 0;
}

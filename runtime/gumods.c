// Data mods made for MHGU 1.4.0 (English), put into MHXX's files (gumods.h, romfs.c).
//
// GU keeps its archives in nativeNX/eng/arc (resident_eng.arc: the English texts and pictures) and nativeNX/loc/arc
// (resident.arc: the rest), and a few textures loose (nativeNX/eng/GUI/99_texture/HD_hud00_BM_NOMIP.tex). XX has one
// archive for both (nativeNX/arc/resident.arc), with those textures inside it (GUI\99_texture\HD_hud00_BM_NOMIP).
// So a GU mod's files land where XX never looks.
//
// GU mods usually ship a whole archive for a few changed textures. Put over XX's, its untouched GU copies would
// replace XX's own (English pictures over the Japanese or the Korean patch's), so a mod's change is told from GU's
// own file by extras/gu140.idx (tools/gu_manifest.py: fingerprints of GU's entries and loose files, no game data):
// an entry or file as GU's is left out. What the mod changed and XX also has (an archive entry of the same name and
// type) goes into an archive holding only those entries, at XX's path, which romfs.c lays over XX's archive like a
// mod's own partial archive (arcmerge.c). Only textures (TEX) are taken: a GU menu layout or text over XX's could
// break it; the rest is counted in the log. Without the fingerprints (an empty extras/gu140.idx) only loose files
// are taken: those are all the mod's.
//
// A menu texture holds icons and text together: a mod that drew new button icons still carries GU's English words
// in the same picture ("Sale!" over the Korean patch's "할인중!"). For those (names under GUI\, which the
// fingerprints have tile signatures of: runtime/nvn/textex.h) only the 16 x 16 squares the mod drew over (its
// signature off GU's) are taken: XX's picture as it is in the files now (the Korean patch's, say) with those tiles of
// the mod's over it, made here and given to the GPU in place of XX's when the game loads it, like a texture pack's
// picture (gumods_picture, texpack.c). A picture of another shape than GU's or XX's goes over whole.
//
// The archives made are kept in cache/gumods/<key> (key: the mod's GU files' paths, sizes and times, and the
// fingerprints' count), so a later start reads none of it again.
#include "gumods.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <zlib.h>

#include "nvn/textex.h"
#include "rt.h"

#define ARC_TEX 0x241f5debu
#define HEAD 12u
#define ENTRY 80u
#define ALIGN 0x8000u
#define MAX_PATH_LEN 1200

__asm__(".pushsection .rodata\n.balign 16\n.globl mhxx_gu140_idx\nmhxx_gu140_idx:\n"
        ".incbin \"extras/gu140.idx\"\n.globl mhxx_gu140_idx_end\nmhxx_gu140_idx_end:\n.popsection");
extern const unsigned char mhxx_gu140_idx[], mhxx_gu140_idx_end[];

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = v, p[1] = v >> 8, p[2] = v >> 16, p[3] = v >> 24; }

static void mkdirs(const char *path) {  // every folder of a file's path
    char p[MAX_PATH_LEN];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
}

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t *)p)[i]) * 1099511628211ull;
    return h;
}

static uint64_t fingerprint64(const void *b, size_t n) {
    uLong c = crc32(0, Z_NULL, 0), a = adler32(0, Z_NULL, 0);
    const Bytef *p = b;
    while (n) {  // (zlib takes a uInt at a time)
        uInt k = n > 0x40000000u ? 0x40000000u : (uInt)n;
        c = crc32(c, p, k), a = adler32(a, p, k);
        p += k, n -= k;
    }
    return (uint64_t)(c & 0xffffffffu) << 32 | (a & 0xffffffffu);
}

// ------------------------------------------------------------------ GU's fingerprints
static uint32_t g_nstock, g_nsig;
static const uint8_t *g_stock, *g_sigidx;
static uint8_t *g_sigs;  // (inflated when first needed)
static bool g_stock_loaded, g_sigs_loaded;
static const uint8_t *g_sigz;
static uint32_t g_sigraw, g_sigzn;

static void stock_load(void) {
    g_stock_loaded = true;
    const uint8_t *p = mhxx_gu140_idx;
    size_t n = (size_t)(mhxx_gu140_idx_end - mhxx_gu140_idx);
    if (n < 12 || memcmp(p, "GUIX", 4)) return;
    uint32_t version = rd32(p + 4), rows = rd32(p + 8);
    if (version == 1 && 12 + (uint64_t)rows * 16 <= n) { g_nstock = rows, g_stock = p + 12; return; }
    if (version != 2 || n < 16) return;
    uint32_t nsig = rd32(p + 12);
    uint64_t idx = 16 + (uint64_t)rows * 16, blob = idx + (uint64_t)nsig * 24;
    if (blob + 8 > n || blob + 8 + rd32(p + blob + 4) > n) return;
    g_nstock = rows, g_stock = p + 16;
    g_nsig = nsig, g_sigidx = p + idx;
    g_sigraw = rd32(p + blob), g_sigzn = rd32(p + blob + 4), g_sigz = p + blob + 8;
}

// GU's tile signatures of the texture `key`: NULL if none
static const uint8_t *sig_find(uint64_t key, uint32_t *w, uint32_t *h) {
    if (!g_nsig) return NULL;
    if (!g_sigs_loaded) {
        g_sigs_loaded = true;
        uLongf un = g_sigraw;
        g_sigs = malloc(un ? un : 1);
        if (g_sigs && uncompress(g_sigs, &un, g_sigz, g_sigzn) != Z_OK) free(g_sigs), g_sigs = NULL;
    }
    if (!g_sigs) return NULL;
    uint32_t lo = 0, hi = g_nsig;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const uint8_t *e = g_sigidx + mid * 24ull;
        uint64_t k = rd64(e);
        if (k == key) {
            *w = rd32(e + 8), *h = rd32(e + 12);
            uint32_t off = rd32(e + 16), len = ((*w + TEX_TILE - 1) / TEX_TILE) * ((*h + TEX_TILE - 1) / TEX_TILE) * TEX_SIG;
            return (uint64_t)off + len <= g_sigraw ? g_sigs + off : NULL;
        }
        if (k < key) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

// GU's own content of the file / entry `key`: false when GU has none (the mod added it)
static bool stock_find(uint64_t key, uint64_t *content) {
    uint32_t lo = 0, hi = g_nstock;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint64_t k = rd64(g_stock + mid * 16ull);
        if (k == key) { *content = rd64(g_stock + mid * 16ull + 8); return true; }
        if (k < key) lo = mid + 1;
        else hi = mid;
    }
    return false;
}

// ------------------------------------------------------------------ XX's archives: which hold an entry
typedef struct { char *name; uint32_t type; int arc; } XEnt;
static XEnt *g_x;
static int g_nx, g_cx;
static char **g_xarcs;  // paths under nativeNX/arc ("arc/resident.arc")
static int g_nxarcs, g_cxarcs;
static bool g_xloaded;

static int xent_cmp(const void *a, const void *b) {
    const XEnt *x = a, *y = b;
    int c = strcasecmp(x->name, y->name);
    return c ? c : (x->type > y->type) - (x->type < y->type);
}

static void x_scan(const char *dir, const char *rel, int depth) {
    if (depth > 8) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[MAX_PATH_LEN], r[MAX_PATH_LEN];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        snprintf(r, sizeof r, "%s/%s", rel, e->d_name);
        struct stat st;
        if (stat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) { x_scan(p, r, depth + 1); continue; }
        size_t nl = strlen(e->d_name);
        if (nl < 5 || strcasecmp(e->d_name + nl - 4, ".arc")) continue;
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        uint8_t head[HEAD];
        if (fread(head, 1, HEAD, f) == HEAD && !memcmp(head, "ARC", 4)) {
            int n = head[6] | head[7] << 8;
            uint8_t *t = malloc((size_t)n * ENTRY + 1);
            if (t && fread(t, ENTRY, (size_t)n, f) == (size_t)n) {
                if (g_nxarcs == g_cxarcs) g_xarcs = realloc(g_xarcs, sizeof *g_xarcs * (g_cxarcs = g_cxarcs ? g_cxarcs * 2 : 256));
                int ai = g_nxarcs++;
                g_xarcs[ai] = strdup(r + 1);  // (no leading '/')
                for (int i = 0; i < n; i++) {
                    if (g_nx == g_cx) g_x = realloc(g_x, sizeof *g_x * (g_cx = g_cx ? g_cx * 2 : 4096));
                    char nm[65];
                    memcpy(nm, t + (size_t)i * ENTRY, 64), nm[64] = 0;
                    g_x[g_nx++] = (XEnt){strdup(nm), rd32(t + (size_t)i * ENTRY + 64), ai};
                }
            }
            free(t);
        }
        fclose(f);
    }
    closedir(d);
}

static void x_load(const char *game_romfs) {
    g_xloaded = true;
    char dir[MAX_PATH_LEN];
    snprintf(dir, sizeof dir, "%s/nativeNX/arc", game_romfs);
    uint64_t t0 = plat_now_ns();
    x_scan(dir, "/arc", 0);
    if (g_nx) qsort(g_x, (size_t)g_nx, sizeof *g_x, xent_cmp);
    LOG("[gumods] XX's archives listed: %d, %d entries (%.2f s)\n", g_nxarcs, g_nx, (plat_now_ns() - t0) / 1e9);
}

// the first of XX's entries named so (case aside) of that type, or NULL; the rest follow it
static const XEnt *x_find(const char *name, uint32_t type) {
    XEnt k = {(char *)name, type, 0};
    const XEnt *e = bsearch(&k, g_x, (size_t)g_nx, sizeof *g_x, xent_cmp);
    while (e && e > g_x && !xent_cmp(e - 1, &k)) e--;
    return e;
}

// ------------------------------------------------------------------ pictures made (XX's texture key: its file)
typedef struct { uint64_t key; char *file; } Pic;
static Pic *g_pics;
static int g_npics, g_cpics;

static void pic_add(uint64_t key, const char *file) {
    for (int i = 0; i < g_npics; i++)
        if (g_pics[i].key == key) {  // (another mod's for the same texture: the later one's)
            free(g_pics[i].file), g_pics[i].file = strdup(file);
            return;
        }
    if (g_npics == g_cpics) g_pics = realloc(g_pics, sizeof *g_pics * (g_cpics = g_cpics ? g_cpics * 2 : 64));
    g_pics[g_npics++] = (Pic){key, strdup(file)};
}

bool gumods_pictures(void) { return g_npics > 0; }

uint8_t *gumods_picture(uint64_t key, uint32_t *w, uint32_t *h) {
    for (int i = 0; i < g_npics; i++) {
        if (g_pics[i].key != key) continue;
        FILE *f = fopen(g_pics[i].file, "rb");
        uint8_t head[12];
        uint8_t *px = NULL;
        if (f && fread(head, 1, 12, f) == 12 && !memcmp(head, "GUPX", 4)) {
            *w = rd32(head + 4), *h = rd32(head + 8);
            uint64_t n = (uint64_t)*w * *h * 4;
            px = *w && *h && *w <= 16384 && *h <= 16384 ? malloc(n) : NULL;
            if (px && fread(px, 1, n, f) != n) free(px), px = NULL;
        }
        if (f) fclose(f);
        if (px) LOG("[gumods] texture %016llx: the GU mod's picture over XX's (%ux%u)\n", (unsigned long long)key, *w, *h);
        return px;
    }
    return NULL;
}

// The XX archives as they are now (an earlier mod's, the Korean patch's...), read when a texture of one is merged
typedef struct { int arc; uint8_t *data; long len; char path[MAX_PATH_LEN]; int64_t size, mtime; } Cur;
static Cur *g_cur;
static int g_ncur, g_ccur;
static const char *(*g_current)(const char *rel);

static Cur *cur_arc(int arc) {
    for (int i = 0; i < g_ncur; i++)
        if (g_cur[i].arc == arc) return g_cur[i].data ? &g_cur[i] : NULL;
    if (g_ncur == g_ccur) g_cur = realloc(g_cur, sizeof *g_cur * (g_ccur = g_ccur ? g_ccur * 2 : 8));
    Cur *c = &g_cur[g_ncur++];
    memset(c, 0, sizeof *c);
    c->arc = arc;
    char rel[MAX_PATH_LEN];
    snprintf(rel, sizeof rel, "nativeNX/%s", g_xarcs[arc]);
    const char *path = g_current ? g_current(rel) : NULL;
    if (!path) return NULL;
    snprintf(c->path, sizeof c->path, "%s", path);
    struct stat st;
    FILE *f = stat(path, &st) ? NULL : fopen(path, "rb");
    if (!f) return NULL;
    c->size = st.st_size, c->mtime = (int64_t)st.st_mtime;
    c->data = malloc((size_t)st.st_size ? (size_t)st.st_size : 1);
    if (c->data && fread(c->data, 1, (size_t)st.st_size, f) == (size_t)st.st_size && st.st_size > (long)HEAD &&
        !memcmp(c->data, "ARC", 4))
        c->len = st.st_size;
    else free(c->data), c->data = NULL;
    fclose(f);
    return c->data ? c : NULL;
}

// An entry of an archive read whole, uncompressed (free it)
static uint8_t *cur_entry(const Cur *c, const char *name, uint32_t type, uLongf *n) {
    int count = c->data[6] | c->data[7] << 8;
    for (int i = 0; i < count && HEAD + (uint64_t)(i + 1) * ENTRY <= (uint64_t)c->len; i++) {
        const uint8_t *e = c->data + HEAD + (size_t)i * ENTRY;
        char nm[65];
        memcpy(nm, e, 64), nm[64] = 0;
        if (rd32(e + 64) != type || strcasecmp(nm, name)) continue;
        uint32_t csize = rd32(e + 68), usize = rd32(e + 72) & 0x1fffffff, off = rd32(e + 76);
        if ((uint64_t)off + csize > (uint64_t)c->len) return NULL;
        uint8_t *b = malloc(usize ? usize : 1);
        *n = usize;
        if (b && uncompress(b, n, c->data + off, csize) == Z_OK) return b;
        free(b);
        return NULL;
    }
    return NULL;
}

static uint8_t *resize(const uint8_t *src, uint32_t w, uint32_t h, uint32_t W, uint32_t H) {
    uint8_t *out = malloc((size_t)W * H * 4);
    if (!out) return NULL;
    if (w == W && h == H) { memcpy(out, src, (size_t)W * H * 4); return out; }
    for (uint32_t y = 0; y < H; y++) {
        float fy = (y + 0.5f) * h / H - 0.5f;
        int y0 = fy < 0 ? 0 : (int)fy, y1 = y0 + 1 < (int)h ? y0 + 1 : y0;
        float ty = fy < 0 ? 0 : fy - y0;
        for (uint32_t x = 0; x < W; x++) {
            float fx = (x + 0.5f) * w / W - 0.5f;
            int x0 = fx < 0 ? 0 : (int)fx, x1 = x0 + 1 < (int)w ? x0 + 1 : x0;
            float tx = fx < 0 ? 0 : fx - x0;
            for (int c = 0; c < 4; c++) {
                float a = src[((size_t)y0 * w + x0) * 4 + c], b = src[((size_t)y0 * w + x1) * 4 + c];
                float d = src[((size_t)y1 * w + x0) * 4 + c], e = src[((size_t)y1 * w + x1) * 4 + c];
                out[((size_t)y * W + x) * 4 + c] = (uint8_t)((a + (b - a) * tx) * (1 - ty) + (d + (e - d) * tx) * ty + 0.5f);
            }
        }
    }
    return out;
}

// ------------------------------------------------------------------ what a mod changed
typedef struct {
    char name[65];   // XX's spelling
    uint32_t type, csize, sf;
    uint8_t *blob;   // stored (compressed) bytes
    int arc;         // XX's archive
} Change;

typedef struct {
    Change *c;
    int n, cap;
    int same, other_type, not_in_xx, bad, unknown;
    int merged, merged_tiles, tiles;  // (pictures made of the mod's tiles over XX's)
    const char *out;                  // the folder made (cache/gumods/<key>)
    FILE *done;                       // its done.txt, being written
} Changes;

static void add_change(Changes *cs, const XEnt *x, uint32_t csize, uint32_t sf, const uint8_t *blob) {
    for (int i = 0; i < cs->n; i++)  // (a later GU file of the mod for the same entry: that one)
        if (cs->c[i].arc == x->arc && cs->c[i].type == x->type && !strcasecmp(cs->c[i].name, x->name)) {
            free(cs->c[i].blob);
            cs->c[i].blob = malloc(csize ? csize : 1), memcpy(cs->c[i].blob, blob, csize);
            cs->c[i].csize = csize, cs->c[i].sf = sf;
            return;
        }
    if (cs->n == cs->cap) cs->c = realloc(cs->c, sizeof *cs->c * (cs->cap = cs->cap ? cs->cap * 2 : 64));
    Change *c = &cs->c[cs->n++];
    snprintf(c->name, sizeof c->name, "%s", x->name);
    c->type = x->type, c->csize = csize, c->sf = sf, c->arc = x->arc;
    c->blob = malloc(csize ? csize : 1);
    memcpy(c->blob, blob, csize);
}

// GU archive base name without its language ("resident_eng.arc" -> "resident")
static void arc_base(const char *file, char *out, size_t n) {
    const char *s = strrchr(file, '/');
    snprintf(out, n, "%s", s ? s + 1 : file);
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
    size_t l = strlen(out);
    if (l > 4 && out[l - 4] == '_') out[l - 4] = 0;
}

// The tiles of the mod's texture `mpx` (mw x mh) whose signature is off GU's (sig, sw x sh), over XX's texture `x`
// (as the files are now): the picture made and put in the table. False: shapes that don't fit (taken whole then).
static bool tile_merge(Changes *cs, const uint8_t *sig, uint32_t sw, uint32_t sh, const uint8_t *mpx, uint32_t mw,
                       uint32_t mh, const XEnt *x) {
    if ((uint64_t)mw * sh != (uint64_t)mh * sw) return false;
    Cur *c = cur_arc(x->arc);
    uLongf xn = 0;
    uint8_t *xt = c ? cur_entry(c, x->name, x->type, &xn) : NULL, *xpx = NULL;
    TexInfo xi;
    bool ok = xt && tex_read(xt, xn, &xi, &xpx) && (uint64_t)xi.w * sh == (uint64_t)xi.h * sw;
    free(xt);
    if (!ok) { free(xpx); return false; }
    uint32_t tx = (sw + TEX_TILE - 1) / TEX_TILE, ty = (sh + TEX_TILE - 1) / TEX_TILE;
    uint8_t *spx = resize(mpx, mw, mh, sw, sh), *msig = malloc((size_t)tx * ty * TEX_SIG + 1);
    // (by quarter: each 16 x 16 quarter of a tile has its own two bytes, so a changed icon takes no more of the text
    // beside it than its own quarters)
    uint8_t *changed = calloc((size_t)tx * ty + 1, 1);  // bit q: quarter q (x half q & 1, y half q >> 1)
    int n = 0;
    if (spx && msig && changed) {
        tex_signatures(spx, sw, sh, msig);
        for (uint32_t i = 0; i < tx * ty; i++)
            for (uint32_t q = 0; q < 4; q++) {
                const uint8_t *m = msig + i * TEX_SIG, *g = sig + i * TEX_SIG;
                if (abs((int)m[q] - (int)g[q]) > TEX_SIG_SLACK || abs((int)m[4 + q] - (int)g[4 + q]) > TEX_SIG_SLACK)
                    changed[i] |= (uint8_t)(1u << q), n++;
            }
        // A changed square's neighbours that differ even a little go with it (TEX_SIG_NEAR): an icon drawn over two
        // squares, one of them a close match, would come out half and half
        uint32_t qw = tx * 2, qh = ty * 2;
        for (bool grew = true; grew;) {
            grew = false;
            for (uint32_t y = 0; y < qh; y++)
                for (uint32_t x = 0; x < qw; x++) {
                    uint32_t t = (y / 2) * tx + x / 2, q = (y & 1) * 2 + (x & 1);
                    if (changed[t] >> q & 1) continue;
                    bool near = false;
                    for (int dy = -1; dy <= 1 && !near; dy++)
                        for (int dx = -1; dx <= 1 && !near; dx++) {
                            int nx = (int)x + dx, ny = (int)y + dy;
                            if ((!dx && !dy) || nx < 0 || ny < 0 || nx >= (int)qw || ny >= (int)qh) continue;
                            uint32_t nt = (uint32_t)(ny / 2) * tx + (uint32_t)nx / 2, nq = (uint32_t)(ny & 1) * 2 + (uint32_t)(nx & 1);
                            near = changed[nt] >> nq & 1;
                        }
                    const uint8_t *m = msig + t * TEX_SIG, *g = sig + t * TEX_SIG;
                    if (near && (abs((int)m[q] - (int)g[q]) > TEX_SIG_NEAR || abs((int)m[4 + q] - (int)g[4 + q]) > TEX_SIG_NEAR))
                        changed[t] |= (uint8_t)(1u << q), n++, grew = true;
                }
        }
    }
    free(spx), free(msig);
    if (!n) {  // (the mod's picture is GU's: re-encoded, perhaps)
        free(changed), free(xpx);
        cs->same++;
        return true;
    }
    uint32_t ow = mw > xi.w ? mw : xi.w, oh = (uint32_t)((uint64_t)ow * sh / sw);
    uint8_t *opx = resize(xpx, xi.w, xi.h, ow, oh), *mo = resize(mpx, mw, mh, ow, oh);
    free(xpx);
    if (!opx || !mo) { free(opx), free(mo), free(changed); return false; }
    const uint32_t half = TEX_TILE / 2;
    for (uint32_t j = 0; j < ty; j++)
        for (uint32_t i = 0; i < tx; i++)
            for (uint32_t q = 0; q < 4; q++) {
                if (!(changed[j * tx + i] >> q & 1)) continue;
                uint64_t gx = (uint64_t)i * TEX_TILE + (q & 1) * half, gy = (uint64_t)j * TEX_TILE + (q >> 1) * half;
                uint32_t x0 = (uint32_t)(gx * ow / sw), x1 = (uint32_t)((gx + half) * ow / sw);
                uint32_t y0 = (uint32_t)(gy * oh / sh), y1 = (uint32_t)((gy + half) * oh / sh);
                if (x1 > ow) x1 = ow;
                if (y1 > oh) y1 = oh;
                for (uint32_t y = y0; y < y1; y++)
                    memcpy(opx + ((size_t)y * ow + x0) * 4, mo + ((size_t)y * ow + x0) * 4, (size_t)(x1 - x0) * 4);
            }
    free(mo), free(changed);
    char file[MAX_PATH_LEN];
    snprintf(file, sizeof file, "%s/tex/%016llx.rgba", cs->out, (unsigned long long)xi.key);
    mkdirs(file);
    FILE *f = fopen(file, "wb");
    uint8_t head[12] = {'G', 'U', 'P', 'X'};
    wr32(head + 4, ow), wr32(head + 8, oh);
    bool w = f && fwrite(head, 1, 12, f) == 12 && fwrite(opx, 1, (size_t)ow * oh * 4, f) == (size_t)ow * oh * 4;
    if (f) w = fclose(f) == 0 && w;
    free(opx);
    if (!w) return false;
    pic_add(xi.key, file);
    if (cs->done) fprintf(cs->done, "tex %016llx %s\n", (unsigned long long)xi.key, file);
    cs->merged++, cs->merged_tiles += n, cs->tiles += (int)(tx * ty * 4);
    LOG("[gumods] %s (%s): %d of %u 16 x 16 squares the mod's, over XX's picture\n", x->name, g_xarcs[x->arc], n, tx * ty * 4);
    return true;
}

// An entry (name, type, its bytes) the mod has: its XX entries, when the mod changed it against GU (`key_str`)
static void consider(Changes *cs, const char *key_str, const char *name, uint32_t type, const uint8_t *body,
                     size_t bsize, uint32_t csize, uint32_t sf, const uint8_t *stored, const char *gu_arc) {
    uint64_t stock, key = fingerprint64(key_str, strlen(key_str));
    if (stock_find(key, &stock) && stock == fingerprint64(body, bsize)) {
        cs->same++;
        return;
    }
    if (type != ARC_TEX) { cs->other_type++; return; }
    if (bsize < 16 || memcmp(body, "TEX", 4)) { cs->bad++; return; }
    const XEnt *x = x_find(name, type);
    if (!x) { cs->not_in_xx++; return; }
    // XX archives holding it: those of the same name as the mod's GU archive if any, else all
    char base[128] = "";
    if (gu_arc) arc_base(gu_arc, base, sizeof base);
    bool named = false;
    XEnt k = {(char *)name, type, 0};
    for (const XEnt *e = x; e < g_x + g_nx && !xent_cmp(e, &k); e++) {
        char xb[128];
        arc_base(g_xarcs[e->arc], xb, sizeof xb);
        if (base[0] && !strcasecmp(xb, base)) named = true;
    }
    uint32_t sw = 0, sh = 0, mw = 0, mh = 0;
    const uint8_t *sig = sig_find(key, &sw, &sh);
    uint8_t *mpx = NULL;
    TexInfo mi;
    if (sig && tex_read(body, bsize, &mi, &mpx)) mw = mi.w, mh = mi.h;
    for (const XEnt *e = x; e < g_x + g_nx && !xent_cmp(e, &k); e++) {
        char xb[128];
        arc_base(g_xarcs[e->arc], xb, sizeof xb);
        if (named && strcasecmp(xb, base)) continue;
        if (mpx && tile_merge(cs, sig, sw, sh, mpx, mw, mh, e)) continue;
        add_change(cs, e, csize, sf, stored);
    }
    free(mpx);
}

static void gu_arc(Changes *cs, const char *path, const char *rel) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *a = len > (long)HEAD ? malloc((size_t)len) : NULL;
    bool ok = a && fread(a, 1, (size_t)len, f) == (size_t)len && !memcmp(a, "ARC", 4);
    fclose(f);
    if (!ok) { free(a); return; }
    int n = a[6] | a[7] << 8;
    for (int i = 0; i < n && HEAD + (uint64_t)(i + 1) * ENTRY <= (uint64_t)len; i++) {
        const uint8_t *e = a + HEAD + (size_t)i * ENTRY;
        char name[65], key[MAX_PATH_LEN];
        memcpy(name, e, 64), name[64] = 0;
        uint32_t type = rd32(e + 64), csize = rd32(e + 68), sf = rd32(e + 72), off = rd32(e + 76);
        uint32_t usize = sf & 0x1fffffff;
        if ((uint64_t)off + csize > (uint64_t)len) { cs->bad++; continue; }
        if (!g_nstock) { cs->unknown++; continue; }  // (no fingerprints: can't tell the mod's changes)
        uint8_t *body = malloc(usize ? usize : 1);
        uLongf un = usize;
        if (!body || uncompress(body, &un, a + off, csize) != Z_OK) {
            free(body);
            cs->bad++;
            continue;
        }
        char lname[65];
        for (int c = 0; c < 65; c++) lname[c] = (char)(name[c] >= 'A' && name[c] <= 'Z' ? name[c] + 32 : name[c] == '/' ? '\\' : name[c]);
        snprintf(key, sizeof key, "%s|%s|%08x", rel, lname, type);
        consider(cs, key, name, type, body, un, csize, sf, a + off, rel);
        free(body);
    }
    free(a);
}

static void gu_loose(Changes *cs, const char *path, const char *rel) {
    // rel: "eng/gui/99_texture/hd_hud00_bm_nomip.tex" (lower case): the entry "gui\99_texture\hd_hud00_bm_nomip"
    size_t rl = strlen(rel);
    if (rl < 5 || strcasecmp(rel + rl - 4, ".tex")) { cs->other_type++; return; }
    const char *slash = strchr(rel, '/');
    if (!slash) return;
    char name[MAX_PATH_LEN];
    snprintf(name, sizeof name, "%.*s", (int)(rel + rl - 4 - (slash + 1)), slash + 1);
    for (char *p = name; *p; p++)
        if (*p == '/') *p = '\\';
    if (strlen(name) > 63) { cs->not_in_xx++; return; }
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = len > 0 ? malloc((size_t)len) : NULL;
    bool ok = b && fread(b, 1, (size_t)len, f) == (size_t)len;
    fclose(f);
    if (!ok) { free(b); return; }
    uLongf cn = compressBound((uLong)len);
    uint8_t *z = malloc(cn);
    if (z && compress2(z, &cn, b, (uLong)len, 6) == Z_OK)
        consider(cs, rel, name, ARC_TEX, b, (size_t)len, (uint32_t)cn, (uint32_t)len | 0x40000000u, z, NULL);
    free(z), free(b);
}

typedef struct { char **path, **rel; int n, cap; } Files;

static void list_gu(Files *fl, const char *dir, const char *rel, int depth) {
    if (depth > 12) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[MAX_PATH_LEN], r[MAX_PATH_LEN];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        snprintf(r, sizeof r, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        struct stat st;
        if (stat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) { list_gu(fl, p, r, depth + 1); continue; }
        if (fl->n == fl->cap) {
            fl->cap = fl->cap ? fl->cap * 2 : 64;
            fl->path = realloc(fl->path, sizeof *fl->path * fl->cap), fl->rel = realloc(fl->rel, sizeof *fl->rel * fl->cap);
        }
        for (char *c = r; *c; c++) *c = (char)(*c >= 'A' && *c <= 'Z' ? *c + 32 : *c);
        fl->path[fl->n] = strdup(p), fl->rel[fl->n++] = strdup(r);
    }
    closedir(d);
}

static bool write_arc(const char *path, uint16_t version, Change **c, int n) {
    uint64_t data = ((uint64_t)HEAD + (uint64_t)n * ENTRY + ALIGN - 1) / ALIGN * ALIGN, total = data;
    for (int i = 0; i < n; i++) total += c[i]->csize;
    if (total > 0x7fffffffu) return false;
    uint8_t *img = calloc(1, (size_t)total);
    if (!img) return false;
    memcpy(img, "ARC", 4);
    img[4] = (uint8_t)version, img[5] = (uint8_t)(version >> 8), img[6] = (uint8_t)n, img[7] = (uint8_t)(n >> 8);
    uint64_t off = data;
    for (int i = 0; i < n; i++) {
        uint8_t *e = img + HEAD + (size_t)i * ENTRY;
        memcpy(e, c[i]->name, 64);
        wr32(e + 64, c[i]->type), wr32(e + 68, c[i]->csize), wr32(e + 72, c[i]->sf), wr32(e + 76, (uint32_t)off);
        memcpy(img + off, c[i]->blob, c[i]->csize);
        off += c[i]->csize;
    }
    char tmp[MAX_PATH_LEN];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fwrite(img, 1, (size_t)total, f) == (size_t)total;
    if (f) ok = fclose(f) == 0 && ok;
    if (ok) ok = rename(tmp, path) == 0;
    if (!ok) remove(tmp);
    free(img);
    return ok;
}



// done.txt: "arc <path> <size> <mtime>" for each archive as it was read (a picture made of it is good while it is
// the same), "tex <key> <file>" for each picture, then "made" (archives in nativeNX) or "none"
static bool reuse(const char *done) {
    FILE *f = fopen(done, "r");
    if (!f) return false;
    char line[MAX_PATH_LEN + 64];
    bool ok = true;
    int npics = g_npics;
    while (ok && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "arc ", 4)) {
            char *sp2 = strrchr(line, ' ');
            if (!sp2 || sp2 == line + 3) { ok = false; break; }
            *sp2 = 0;
            char *sp1 = strrchr(line, ' ');
            if (!sp1 || sp1 <= line + 3) { ok = false; break; }
            *sp1 = 0;
            struct stat st;
            ok = !stat(line + 4, &st) && (long long)st.st_size == atoll(sp1 + 1) && (long long)st.st_mtime == atoll(sp2 + 1);
        } else if (!strncmp(line, "tex ", 4) && strlen(line) > 22) {
            line[20] = 0;
            pic_add(strtoull(line + 4, NULL, 16), line + 21);
        }
    }
    fclose(f);
    if (!ok) {  // (an archive changed: the Korean patch updated, say; made again)
        for (int i = npics; i < g_npics; i++) free(g_pics[i].file);
        g_npics = npics;
        return false;
    }
    return true;
}

bool gumods_build(const char *mod_romfs, const char *game_romfs, const char *(*current)(const char *rel), char *out,
                  size_t outsz) {
    g_current = current;
    Files fl = {0};
    for (const char *top = "eng"; top; top = strcmp(top, "eng") ? NULL : "loc") {
        char dir[MAX_PATH_LEN];
        snprintf(dir, sizeof dir, "%s/nativeNX/%s", mod_romfs, top);
        list_gu(&fl, dir, top, 0);
    }
    if (!fl.n) return false;
    // (in one order whatever the folder's, so a later GU file of the same entry is the same one each start)
    for (int i = 0; i < fl.n; i++)
        for (int k = i + 1; k < fl.n; k++)
            if (strcmp(fl.rel[k], fl.rel[i]) < 0) {
                char *t = fl.rel[i]; fl.rel[i] = fl.rel[k], fl.rel[k] = t;
                t = fl.path[i]; fl.path[i] = fl.path[k], fl.path[k] = t;
            }
    if (!g_stock_loaded) stock_load();
    uint64_t key = fnv(14695981039346656037ull, "gumods2", 7);
    key = fnv(key, &g_nstock, sizeof g_nstock);
    key = fnv(key, &g_nsig, sizeof g_nsig);
    key = fnv(key, game_romfs, strlen(game_romfs));
    for (int i = 0; i < fl.n; i++) {
        struct stat st;
        stat(fl.path[i], &st);
        int64_t sz = st.st_size, mt = (int64_t)st.st_mtime;
        key = fnv(fnv(fnv(key, fl.rel[i], strlen(fl.rel[i]) + 1), &sz, sizeof sz), &mt, sizeof mt);
    }
    snprintf(out, outsz, "cache/gumods/%016llx", (unsigned long long)key);
    char done[MAX_PATH_LEN], arcdir[MAX_PATH_LEN];
    snprintf(done, sizeof done, "%s/done.txt", out);
    snprintf(arcdir, sizeof arcdir, "%s/nativeNX", out);
    struct stat st;
    bool ok = false;
    if (reuse(done)) {  // (made on an earlier start)
        ok = !stat(arcdir, &st);
        LOG("[gumods] %s: its GU files were put into XX's on an earlier start (%s)\n", mod_romfs, out);
        goto free_list;
    }
    if (!g_xloaded) x_load(game_romfs);
    char donetmp[MAX_PATH_LEN];
    snprintf(donetmp, sizeof donetmp, "%s.tmp", done);
    mkdirs(donetmp);
    Changes cs = {.out = out, .done = fopen(donetmp, "w")};
    for (int i = 0; i < fl.n; i++) {
        size_t rl = strlen(fl.rel[i]);
        if (rl > 4 && !strcmp(fl.rel[i] + rl - 4, ".arc")) gu_arc(&cs, fl.path[i], fl.rel[i]);
        else gu_loose(&cs, fl.path[i], fl.rel[i]);
    }
    // one archive per XX archive changed whole
    int written = 0;
    Change **by = malloc(sizeof *by * (size_t)(cs.n + 1));
    for (int a = 0; by && a < g_nxarcs; a++) {
        int m = 0;
        for (int i = 0; i < cs.n; i++)
            if (cs.c[i].arc == a) by[m++] = &cs.c[i];
        if (!m) continue;
        char path[MAX_PATH_LEN], orig[MAX_PATH_LEN];
        snprintf(path, sizeof path, "%s/nativeNX/%s", out, g_xarcs[a]);
        snprintf(orig, sizeof orig, "%s/nativeNX/%s", game_romfs, g_xarcs[a]);
        uint16_t version = 17;
        FILE *f = fopen(orig, "rb");
        uint8_t head[HEAD];
        if (f && fread(head, 1, HEAD, f) == HEAD) version = (uint16_t)(head[4] | head[5] << 8);
        if (f) fclose(f);
        mkdirs(path);
        if (write_arc(path, version, by, m)) written++, LOG("[gumods] %s: %d texture(s) into %s\n", mod_romfs, m, g_xarcs[a]);
    }
    free(by);
    LOG("[gumods] %s: GU files: %d texture(s) the mod changed put in whole into %d of XX's archives, %d menu "
        "picture(s) of its squares over XX's (%d of %d 16 x 16 squares); left out: %d as GU's own, %d not textures (a GU menu or "
        "text could break XX's), %d not in XX, %d unreadable%s\n",
        mod_romfs, cs.n, written, cs.merged, cs.merged_tiles, cs.tiles, cs.same, cs.other_type, cs.not_in_xx, cs.bad,
        cs.unknown ? " (archives not read: this build has no GU fingerprints, extras/gu140.idx)" : "");
    for (int i = 0; i < cs.n; i++) free(cs.c[i].blob);
    free(cs.c);
    for (int i = 0; i < g_ncur; i++) {
        if (g_cur[i].data && cs.done)
            fprintf(cs.done, "arc %s %lld %lld\n", g_cur[i].path, (long long)g_cur[i].size, (long long)g_cur[i].mtime);
        free(g_cur[i].data);
    }
    g_ncur = 0;
    ok = written > 0;
    if (cs.done) {
        fputs(ok ? "made\n" : "none\n", cs.done);
        if (!fclose(cs.done)) rename(donetmp, done);
    }
free_list:
    for (int i = 0; i < fl.n; i++) free(fl.path[i]), free(fl.rel[i]);
    free(fl.path), free(fl.rel);
    return ok;
}

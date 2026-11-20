// mhxx-gumanifest: the MHGU 1.4.0 (English) fingerprints for GU data mods (runtime/gumods.c), from your own game's
// files. Writes numbers only, no game data:
//
//   mhxx-gumanifest <MHGU romfs folder> [<out file>]      (default out: extras/gu140.idx)
//
// For every archive entry and loose file under nativeNX/eng and nativeNX/loc: a fingerprint of its name and one of
// its (uncompressed) bytes, which tell the port what a GU mod left as GU's. For the menu textures (names under GUI\):
// also their tile signatures (runtime/nvn/textex.h: a few means a 32 x 32 tile), which tell the port which parts of
// a picture the mod drew over, so only those go over XX's (the rest of the picture keeps XX's own text: the Korean
// patch's, say).
//
// File: "GUIX", u32 2, u32 entries, u32 signatures; entries x (u64 key, u64 content) by key; signatures x (u64 key,
// u32 width, u32 height, u32 offset into the signature bytes, u32 0) by key; u32 the signature bytes' size, u32 their
// size compressed, the signature bytes zlib-compressed (most tiles of a menu texture are empty). The font pages
// (gui\02_common\font) are left out.
// fingerprint64: crc32 << 32 | adler32. The key's string: the file's path under nativeNX in lower case with '/'
// ("eng/arc/resident_eng.arc"), and for an archive entry "|" + its name in lower case with '\' + "|" + its type in 8
// hex digits ("eng/arc/resident_eng.arc|gui\99_texture\hd_lobby00_bm_nomip|241f5deb").
#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

#include "../../runtime/nvn/textex.h"

#define ARC_TEX 0x241f5debu

static uint64_t fingerprint64(const void *b, size_t n) {
    uLong c = crc32(0, Z_NULL, 0), a = adler32(0, Z_NULL, 0);
    const Bytef *p = b;
    while (n) {
        uInt k = n > 0x40000000u ? 0x40000000u : (uInt)n;
        c = crc32(c, p, k), a = adler32(a, p, k);
        p += k, n -= k;
    }
    return (uint64_t)(c & 0xffffffffu) << 32 | (a & 0xffffffffu);
}

typedef struct { uint64_t key, content; } Row;
typedef struct { uint64_t key; uint32_t w, h; uint8_t *sig; uint32_t len; } Sig;
static Row *rows;
static size_t nrows, crows;
static Sig *sigs;
static size_t nsigs, csigs;
static unsigned files, textures;

static void row(uint64_t key, uint64_t content) {
    if (nrows == crows) rows = realloc(rows, sizeof *rows * (crows = crows ? crows * 2 : 65536));
    rows[nrows++] = (Row){key, content};
}

static void signature(uint64_t key, const uint8_t *t, uint64_t n) {
    TexInfo ti;
    uint8_t *px;
    if (!tex_read(t, n, &ti, &px) || ti.w < TEX_TILE || ti.h < TEX_TILE) return;
    uint32_t tx = (ti.w + TEX_TILE - 1) / TEX_TILE, ty = (ti.h + TEX_TILE - 1) / TEX_TILE, len = tx * ty * TEX_SIG;
    if (nsigs == csigs) sigs = realloc(sigs, sizeof *sigs * (csigs = csigs ? csigs * 2 : 1024));
    Sig *s = &sigs[nsigs++];
    s->key = key, s->w = ti.w, s->h = ti.h, s->len = len, s->sig = malloc(len);
    tex_signatures(px, ti.w, ti.h, s->sig);
    free(px);
    textures++;
}

static void lower(char *s) {
    for (; *s; s++) *s = (char)(*s >= 'A' && *s <= 'Z' ? *s + 32 : *s);
}

static void arc_file(const char *path, const char *rel) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 12 ? malloc((size_t)n) : NULL;
    bool ok = d && fread(d, 1, (size_t)n, f) == (size_t)n && !memcmp(d, "ARC", 4);
    fclose(f);
    if (!ok) { free(d); return; }
    uint32_t count = d[6] | d[7] << 8;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = d + 12 + (uint64_t)i * 80;
        if (e + 80 > d + n) break;
        uint32_t type, csize, sf, off;
        memcpy(&type, e + 64, 4), memcpy(&csize, e + 68, 4), memcpy(&sf, e + 72, 4), memcpy(&off, e + 76, 4);
        if ((uint64_t)off + csize > (uint64_t)n) continue;
        uLongf un = sf & 0x1fffffff;
        uint8_t *body = malloc(un ? un : 1);
        if (!body) continue;
        if (uncompress(body, &un, d + off, csize) != Z_OK) memcpy(body, d + off, un = csize < un ? csize : un);
        char name[65], key[1400];
        memcpy(name, e, 64), name[64] = 0;
        lower(name);
        for (char *c = name; *c; c++)
            if (*c == '/') *c = '\\';
        snprintf(key, sizeof key, "%s|%s|%08x", rel, name, type);
        uint64_t k = fingerprint64(key, strlen(key));
        row(k, fingerprint64(body, un));
        if (type == ARC_TEX && !strncmp(name, "gui\\", 4) && !strstr(name, "\\font\\")) signature(k, body, un);
        free(body);
    }
    free(d);
}

static void loose(const char *path, const char *rel) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 0 ? malloc((size_t)n) : NULL;
    bool ok = d && fread(d, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!ok) { free(d); return; }
    uint64_t k = fingerprint64(rel, strlen(rel));
    row(k, fingerprint64(d, (size_t)n));
    size_t rl = strlen(rel);
    const char *s = strchr(rel, '/');
    if (rl > 4 && !strcmp(rel + rl - 4, ".tex") && s && !strncmp(s + 1, "gui/", 4)) signature(k, d, (uint64_t)n);
    free(d);
}

static void walk(const char *dir, const char *rel) {
    DIR *dd = opendir(dir);
    if (!dd) return;
    struct dirent *e;
    while ((e = readdir(dd))) {
        if (e->d_name[0] == '.') continue;
        char p[1200], r[1200];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        snprintf(r, sizeof r, "%s/%s", rel, e->d_name);
        lower(r);
        struct stat st;
        if (stat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) { walk(p, r); continue; }
        size_t l = strlen(r);
        if (l > 4 && !strcmp(r + l - 4, ".arc")) arc_file(p, r);
        else loose(p, r);
        if (++files % 200 == 0) fprintf(stderr, "  %u files, %zu entries, %u textures\n", files, nrows, textures);
    }
    closedir(dd);
}

static int by_row(const void *a, const void *b) {
    uint64_t x = ((const Row *)a)->key, y = ((const Row *)b)->key;
    return (x > y) - (x < y);
}
static int by_sig(const void *a, const void *b) {
    uint64_t x = ((const Sig *)a)->key, y = ((const Sig *)b)->key;
    return (x > y) - (x < y);
}
static void w32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void w64(FILE *f, uint64_t v) { fwrite(&v, 8, 1, f); }

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: mhxx-gumanifest <MHGU romfs folder> [<out file>]\n");
        return 2;
    }
    char root[1300], p[1400];
    snprintf(root, sizeof root, "%s", argv[1]);
    struct stat st;
    snprintf(p, sizeof p, "%s/nativeNX", root);
    if (!stat(p, &st)) snprintf(root, sizeof root, "%s", p);
    else {
        snprintf(p, sizeof p, "%s/romfs/nativeNX", argv[1]);
        if (!stat(p, &st)) snprintf(root, sizeof root, "%s", p);
    }
    char eng[1400], loc[1400];
    snprintf(eng, sizeof eng, "%s/eng", root), snprintf(loc, sizeof loc, "%s/loc", root);
    if (stat(eng, &st) || stat(loc, &st)) {
        fprintf(stderr, "%s: no eng and loc folders: not the MHGU (English) game files?\n", root);
        return 1;
    }
    walk(eng, "eng");
    walk(loc, "loc");
    qsort(rows, nrows, sizeof *rows, by_row);
    qsort(sigs, nsigs, sizeof *sigs, by_sig);
    const char *out = argc > 2 ? argv[2] : "extras/gu140.idx";
    FILE *f = fopen(out, "wb");
    if (!f) { perror(out); return 1; }
    fwrite("GUIX", 1, 4, f);
    w32(f, 2), w32(f, (uint32_t)nrows), w32(f, (uint32_t)nsigs);
    for (size_t i = 0; i < nrows; i++) w64(f, rows[i].key), w64(f, rows[i].content);
    uint32_t off = 0;
    for (size_t i = 0; i < nsigs; i++) w64(f, sigs[i].key), w32(f, sigs[i].w), w32(f, sigs[i].h), w32(f, off), w32(f, 0), off += sigs[i].len;
    uint8_t *blob = malloc(off ? off : 1);
    for (size_t i = 0, o = 0; i < nsigs; o += sigs[i].len, i++) memcpy(blob + o, sigs[i].sig, sigs[i].len);
    uLongf zn = compressBound(off);
    uint8_t *z = malloc(zn);
    if (compress2(z, &zn, blob, off, 9) != Z_OK) { fprintf(stderr, "compress failed\n"); return 1; }
    w32(f, off), w32(f, (uint32_t)zn);
    fwrite(z, 1, zn, f);
    long size = ftell(f);
    fclose(f);
    printf("%s: %zu entries, %zu menu textures' tile signatures, from %u files (%.1f MB; fingerprints only)\n", out,
           nrows, nsigs, files, size / 1048576.0);
    return 0;
}

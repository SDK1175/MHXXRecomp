// A game texture read on the host (textex.h).
#include "textex.h"

#include <stdlib.h>
#include <string.h>

#include "bcn.h"
#include "tegra.h"

typedef struct { uint8_t tex; uint32_t nvn; int bcn; } Fmt;
static const Fmt fmts[] = {  // (tools/texextract's list)
    {0x07, 0x25, BCN_NONE},  // RGBA8
    {0x13, 0x42, BCN_BC1},
    {0x17, 0x44, BCN_BC3},
    {0x19, 0x49, BCN_BC4U},
    {0x1f, 0x4b, BCN_BC5U},
};

uint64_t tex_pack_key(const uint8_t *p, uint64_t n, uint32_t w, uint32_t h, uint32_t nvn_format) {
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

bool tex_read(const uint8_t *t, uint64_t n, TexInfo *info, uint8_t **rgba) {
    if (rgba) *rgba = NULL;
    if (n < 0x18 || memcmp(t, "TEX", 4)) return false;
    uint32_t d1, d2, d3;
    memcpy(&d1, t + 4, 4), memcpy(&d2, t + 8, 4), memcpy(&d3, t + 12, 4);
    uint32_t levels = d2 & 0x3f, w = (d2 >> 6) & 0x1fff, h = (d2 >> 19) & 0x1fff, count = d3 & 0xff,
             code = (d3 >> 8) & 0xff, dim = d1 >> 28;
    if (dim != 2 || count != 1 || !w || !h) return false;
    const Fmt *f = NULL;
    for (unsigned i = 0; i < sizeof fmts / sizeof *fmts; i++)
        if (fmts[i].tex == code) f = &fmts[i];
    if (!f) return false;
    TexLayout lay = {.width = w, .height = h, .depth = 1, .layers = 1, .levels = levels ? levels : 1,
                     .fmt = nvn_format_info(f->nvn)};
    tegra_layout_init(&lay);
    LevelLayout l = tegra_level(&lay, 0);
    uint32_t bpb = lay.fmt->bpb, level0;
    uint64_t size = (uint64_t)l.wb * l.hb * bpb, data = 0x14 + 4ull * lay.levels;
    if (data > n) return false;
    memcpy(&level0, t + 0x14, 4);
    data += level0;
    if (data + size > n) return false;
    uint8_t *lin = malloc(size ? size : 1);
    if (!lin) return false;
    tegra_deswizzle(t + data, lin, l.wb, l.hb, 1, bpb, l.block_height_log2, l.block_depth_log2);
    info->w = w, info->h = h, info->nvn = f->nvn, info->key = tex_pack_key(lin, size, w, h, f->nvn);
    if (rgba) {
        uint8_t *px = malloc((uint64_t)w * h * 4);
        if (!px) { free(lin); return false; }
        if (!f->bcn) memcpy(px, lin, (uint64_t)w * h * 4);
        else {
            uint32_t ts = bcn_texel_size(f->bcn);
            uint8_t *d = ts == 4 ? px : malloc((uint64_t)w * h * ts);
            if (!d) { free(px), free(lin); return false; }
            bcn_decode(f->bcn, lin, w, h, d);
            if (ts != 4)
                for (uint64_t i = (uint64_t)w * h; i-- > 0;) {
                    uint8_t r = d[i * ts], g = ts > 1 ? d[i * ts + 1] : r, b = ts > 1 ? 0 : r;
                    px[i * 4] = r, px[i * 4 + 1] = g, px[i * 4 + 2] = b, px[i * 4 + 3] = 255;
                }
            if (d != px) free(d);
        }
        *rgba = px;
    }
    free(lin);
    return true;
}

void tex_signatures(const uint8_t *rgba, uint32_t w, uint32_t h, uint8_t *sig) {
    uint32_t tx = (w + TEX_TILE - 1) / TEX_TILE, ty = (h + TEX_TILE - 1) / TEX_TILE, half = TEX_TILE / 2;
    for (uint32_t j = 0; j < ty; j++)
        for (uint32_t i = 0; i < tx; i++) {
            uint8_t *s = sig + ((uint64_t)j * tx + i) * TEX_SIG;
            for (uint32_t q = 0; q < 4; q++) {
                uint32_t x0 = i * TEX_TILE + (q & 1) * half, y0 = j * TEX_TILE + (q >> 1) * half;
                uint64_t la = 0, a = 0, cnt = 0;
                for (uint32_t y = y0; y < y0 + half && y < h; y++)
                    for (uint32_t x = x0; x < x0 + half && x < w; x++) {
                        const uint8_t *p = rgba + ((uint64_t)y * w + x) * 4;
                        uint32_t luma = (77u * p[0] + 150u * p[1] + 29u * p[2]) >> 8;
                        la += luma * p[3] / 255u, a += p[3], cnt++;
                    }
                s[q] = (uint8_t)(cnt ? la / cnt : 0), s[4 + q] = (uint8_t)(cnt ? a / cnt : 0);
            }
        }
}

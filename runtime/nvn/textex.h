// A game texture (MT Framework TEX, as stored in an .arc) read on the host (textex.c): its size, the key the texture
// dump / texture packs name it by (runtime/nvn/vulkan/texpack.c texpack_key), and its picture. Used by gumods.c and
// tools/gumanifest. Also: the tile signatures the GU data mod merge compares pictures by.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t w, h;     // the full-size level's
    uint32_t nvn;      // NVN format
    uint64_t key;      // texture pack key
} TexInfo;

// t: the TEX file's bytes. With rgba, *rgba gets the full-size level as RGBA8 (free it). False: not a 2D texture of a
// known format (RGBA8, BC1, BC3, BC4, BC5), or cut short.
bool tex_read(const uint8_t *t, uint64_t n, TexInfo *info, uint8_t **rgba);

// The texture pack key of a level's bytes as the GPU reads them, deswizzled to block rows (texpack.c's own)
uint64_t tex_pack_key(const uint8_t *p, uint64_t n, uint32_t w, uint32_t h, uint32_t nvn_format);

// Tile signatures: a picture in TEX_TILE x TEX_TILE tiles (the last row / column short), TEX_SIG bytes a tile: the
// mean of luma x alpha and the mean alpha of each of its four quarters. sig: tiles_x * tiles_y * TEX_SIG bytes.
#define TEX_TILE 32u
#define TEX_SIG 8u
#define TEX_SIG_SLACK 12  // (a tile whose bytes all differ by no more than this: unchanged; BCn re-encoding moves them ~3)
#define TEX_SIG_NEAR 3    // (beside a changed one: more than this goes with it)
void tex_signatures(const uint8_t *rgba, uint32_t w, uint32_t h, uint8_t *sig);

// The Korean patch's Hangul, drawn again from Paperlogy ([Display] KoreanFont, hangulfont.h in vk.h).
//
// The Korean patch for MHXX draws its Hangul over kanji: the 2350 syllables of KS X 1001, in that order (가 각 간 ...
// 힝), take the glyphs of U+7A00 onwards in nativeNX/arc/resident.arc GUI\02_common\font\font_festa (GFD: "GFD\0",
// the glyph count at +0x1c, a name at +0x44 after its length, then 20 bytes a glyph: u32 code, u32 place = page
// (bits 0-7), x (8-19), y (20-31), u32 size = width (0-11), height (12-23), two more words) and paints them, 22 x 26
// apart from x 0, y 650 of font_festa_00_HQ on into font_festa_01_HQ (1024 x 1024 RGBA8, white, the glyph in alpha).
// The game's text names those kanji. The glyphs are small, so on a big screen they are drawn magnified and blurred.
//
// Here the pages holding them become pictures twice the size (vk_tex_replace, like a texture pack's): the page
// magnified, and each of those glyph boxes drawn again from Paperlogy Medium (PT&, Lee Juim; SIL Open Font License 1.1,
// third_party/paperlogy: the font the Korean patch of MHGU draws its Hangul from), the ink as tall as the patch's.
// Nothing changes in the game's files or its text; the font's box and spacing stay the patch's.
//
// It is only done for a font laid out exactly so (every one of the 2350 boxes where the next of that grid would be):
// without the patch the same glyphs are the game's kanji, packed some other way.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "vk.h"
#include "../tegra.h"
#include "../../config.h"
#include "../../romfs.h"
#include "hangulfont_ks.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imgui/imstb_truetype.h"
#pragma GCC diagnostic pop

__asm__(".pushsection .rodata\n.balign 16\n.globl mhxx_paperlogy_ttf\nmhxx_paperlogy_ttf:\n"
        ".incbin \"third_party/paperlogy/Paperlogy-5Medium.ttf\"\n.globl mhxx_paperlogy_ttf_end\nmhxx_paperlogy_ttf_end:\n"
        ".popsection");
extern const unsigned char mhxx_paperlogy_ttf[], mhxx_paperlogy_ttf_end[];

#define ARC_TEX 0x241f5debu
#define ARC_GFD 0x2d462600u
#define SYLLABLES 2350
#define FIRST_CODE 0x7a00u
#define PAGE_SIDE 1024u
#define SCALE 2u  // (the pictures' size: the page's x 2, as the MHGU patch's own pages)
#define MAX_PAGES 8

typedef struct { uint16_t x, y, w, h; uint8_t page; } Box;
static Box boxes[SYLLABLES];
static struct { uint64_t key; int page; } pages[MAX_PAGES];
static int npages;
static int state = -1;  // -1 not looked at, 0 not the patch's font (or off), 1 ready

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// an archive member, decompressed (free it), or NULL
static uint8_t *member(const uint8_t *arc, long n, const char *name, uint32_t type, uint32_t *size) {
    if (n < 12 || memcmp(arc, "ARC", 4)) return NULL;
    uint32_t count = arc[6] | arc[7] << 8;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = arc + 12 + (uint64_t)i * 80;
        if (e + 80 > arc + n) break;
        char nm[65];
        memcpy(nm, e, 64), nm[64] = 0;
        if (rd32(e + 64) != type || SDL_strcasecmp(nm, name)) continue;
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

// The font's Hangul boxes, if it is the Korean patch's grid
static bool read_boxes_into(const uint8_t *g, uint32_t n, Box *boxes, uint32_t *codes);
static bool read_boxes(const uint8_t *g, uint32_t n) { return read_boxes_into(g, n, boxes, NULL); }
static bool read_boxes_into(const uint8_t *g, uint32_t n, Box *boxes, uint32_t *codes) {
    if (n < 0x48 || memcmp(g, "GFD", 4)) return false;
    uint32_t count = rd32(g + 0x1c), name = rd32(g + 0x40);
    uint64_t base = 0x44ull + name + 1;
    if (base + (uint64_t)count * 20 != n) base = n - (uint64_t)count * 20;  // (another header: the table ends the file)
    if ((uint64_t)count * 20 > n) return false;
    // the first syllable's glyph: the first code at FIRST_CODE or above
    uint32_t first = count;
    for (uint32_t i = 0; i < count; i++)
        if (rd32(g + base + i * 20ull) >= FIRST_CODE) { first = i; break; }
    if (first + SYLLABLES > count || rd32(g + base + first * 20ull) != FIRST_CODE) return false;
    for (int i = 0; i < SYLLABLES; i++) {
        const uint8_t *e = g + base + (uint64_t)(first + i) * 20;
        uint32_t place = rd32(e + 4), size = rd32(e + 8);
        Box b = {(uint16_t)((place >> 8) & 0xfff), (uint16_t)(place >> 20), (uint16_t)(size & 0xfff),
                 (uint16_t)((size >> 12) & 0xfff), (uint8_t)(place & 0xff)};
        if (!b.w || !b.h || b.x + b.w > PAGE_SIDE || b.y + b.h > PAGE_SIDE || b.page >= MAX_PAGES) return false;
        if (i) {
            const Box *p = &boxes[i - 1];
            bool next = (b.page == p->page && b.y == p->y && b.x == p->x + 22) ||
                        (b.page == p->page && b.y > p->y && b.x == 0) || (b.page == p->page + 1 && b.x == 0);
            if (!next) return false;  // (the game's own kanji: not in this grid)
        }
        boxes[i] = b;
        if (codes) codes[i] = rd32(e);
    }
    return true;
}

// a font page's key as texpack_replace computes it (RGBA8 TEX: code 0x07, NVN format 0x25)
static bool page_key(const uint8_t *t, uint32_t n, uint64_t *key) {
    if (n < 0x18 || memcmp(t, "TEX", 4)) return false;
    uint32_t d2 = rd32(t + 8), d3 = rd32(t + 12);
    uint32_t levels = d2 & 0x3f, w = (d2 >> 6) & 0x1fff, h = (d2 >> 19) & 0x1fff;
    if (((d3 >> 8) & 0xff) != 0x07 || w != PAGE_SIDE || h != PAGE_SIDE) return false;
    TexLayout lay = {.width = w, .height = h, .depth = 1, .layers = 1, .levels = levels ? levels : 1,
                     .fmt = nvn_format_info(0x25)};
    tegra_layout_init(&lay);
    LevelLayout l = tegra_level(&lay, 0);
    uint64_t size = (uint64_t)l.wb * l.hb * lay.fmt->bpb, data = 0x14 + 4ull * lay.levels + rd32(t + 0x14);
    if (data + size > n) return false;
    uint8_t *lin = malloc(size);
    if (!lin) return false;
    tegra_deswizzle(t + data, lin, l.wb, l.hb, 1, lay.fmt->bpb, l.block_height_log2, l.block_depth_log2);
    *key = texpack_key(lin, size, w, h, 0x25);
    free(lin);
    return true;
}

static void look(void) {
    state = 0;
    if (g_config.korean_font != KOREAN_FONT_PAPERLOGY) return;
    const char *path = romfs_host_path("nativeNX/arc/resident.arc");
    SDL_IOStream *io = path ? SDL_IOFromFile(path, "rb") : NULL;
    Sint64 n = io ? SDL_GetIOSize(io) : -1;
    uint8_t *arc = n > 12 ? malloc((size_t)n) : NULL;
    bool ok = arc && SDL_ReadIO(io, arc, (size_t)n) == (size_t)n;
    if (io) SDL_CloseIO(io);
    uint32_t gn = 0;
    uint8_t *gfd = ok ? member(arc, (long)n, "GUI\\02_common\\font\\font_festa", ARC_GFD, &gn) : NULL;
    if (!gfd || !read_boxes(gfd, gn)) {
        free(gfd), free(arc);
        return;  // (no Korean patch: the game's own font)
    }
    free(gfd);
    bool used[MAX_PAGES] = {0};
    for (int i = 0; i < SYLLABLES; i++) used[boxes[i].page] = true;
    for (int p = 0; p < MAX_PAGES; p++) {
        if (!used[p]) continue;
        char name[64];
        SDL_snprintf(name, sizeof name, "GUI\\02_common\\font\\font_festa_%02d_HQ", p);
        uint32_t tn = 0;
        uint8_t *t = member(arc, (long)n, name, ARC_TEX, &tn);
        uint64_t key;
        if (t && page_key(t, tn, &key)) pages[npages].key = key, pages[npages++].page = p;
        free(t);
    }
    free(arc);
    if (npages) state = 1;
    LOG("[hangul] the Korean patch's font: its Hangul drawn from Paperlogy (%d page(s))\n", npages);
}

bool hangulfont_size(uint32_t w, uint32_t h) {
    if (w != PAGE_SIDE || h != PAGE_SIDE) return false;
    if (state < 0) look();
    return state == 1;
}

int hangulfont_page(uint64_t key) {
    for (int i = 0; i < npages; i++)
        if (pages[i].key == key) return pages[i].page;
    return -1;
}

bool hangulfont_apply(TexBackend *b, int page, const uint8_t *rgba, uint32_t w, uint32_t h, bool srgb) {
    static stbtt_fontinfo font;
    static int font_ok = -1;
    if (font_ok < 0) font_ok = stbtt_InitFont(&font, mhxx_paperlogy_ttf, 0) != 0;
    if (!font_ok || w != PAGE_SIDE || h != PAGE_SIDE) return false;
    const uint32_t W = w * SCALE, H = h * SCALE;
    uint8_t *px = malloc((size_t)W * H * 4);
    if (!px) return false;
    // the page magnified (bilinear: as it was drawn before)
    for (uint32_t y = 0; y < H; y++) {
        float fy = (y + 0.5f) / SCALE - 0.5f;
        int y0 = fy < 0 ? 0 : (int)fy, y1 = y0 + 1 < (int)h ? y0 + 1 : y0;
        float ty = fy < 0 ? 0 : fy - y0;
        for (uint32_t x = 0; x < W; x++) {
            float fx = (x + 0.5f) / SCALE - 0.5f;
            int x0 = fx < 0 ? 0 : (int)fx, x1 = x0 + 1 < (int)w ? x0 + 1 : x0;
            float tx = fx < 0 ? 0 : fx - x0;
            for (int c = 0; c < 4; c++) {
                float a = rgba[((size_t)y0 * w + x0) * 4 + c], bb = rgba[((size_t)y0 * w + x1) * 4 + c];
                float cc = rgba[((size_t)y1 * w + x0) * 4 + c], d = rgba[((size_t)y1 * w + x1) * 4 + c];
                float v = (a + (bb - a) * tx) * (1 - ty) + (cc + (d - cc) * tx) * ty;
                px[((size_t)y * W + x) * 4 + c] = (uint8_t)(v + 0.5f);
            }
        }
    }
    // the ink of the patch's syllables spans rows 1..23 of a 25-row box: '한' (as tall as any) that tall here
    int x0, y0, x1, y1;
    stbtt_GetCodepointBox(&font, 0xd55c, &x0, &y0, &x1, &y1);
    const float ink_top = 1.0f * SCALE, ink = 22.0f * SCALE;
    float sc = y1 > y0 ? ink / (float)(y1 - y0) : stbtt_ScaleForPixelHeight(&font, ink);
    float baseline = ink_top + y1 * sc;
    uint8_t *glyph = malloc((size_t)64 * SCALE * 64 * SCALE);
    int drawn = 0;
    for (int i = 0; glyph && i < SYLLABLES; i++) {
        const Box *bx = &boxes[i];
        if (bx->page != page) continue;
        uint32_t cp = ksx1001_hangul[i];
        uint32_t bxw = bx->w * SCALE, bxh = bx->h * SCALE, ox = bx->x * SCALE, oy = bx->y * SCALE;
        for (uint32_t y = 0; y < bxh; y++)  // (the patch's glyph out: white, clear)
            for (uint32_t x = 0; x < bxw; x++) {
                uint8_t *p = px + ((size_t)(oy + y) * W + ox + x) * 4;
                p[0] = p[1] = p[2] = 255, p[3] = 0;
            }
        int adv, lsb, gx0, gy0, gx1, gy1;
        stbtt_GetCodepointHMetrics(&font, (int)cp, &adv, &lsb);
        float left = (bxw - adv * sc) * 0.5f;
        float sx = left - (float)(int)left, sy = baseline - (float)(int)baseline;
        stbtt_GetCodepointBitmapBoxSubpixel(&font, (int)cp, sc, sc, sx, sy, &gx0, &gy0, &gx1, &gy1);
        int gw = gx1 - gx0, gh = gy1 - gy0;
        if (gw <= 0 || gh <= 0 || gw > 64 * (int)SCALE || gh > 64 * (int)SCALE) continue;
        stbtt_MakeCodepointBitmapSubpixel(&font, glyph, gw, gh, gw, sc, sc, sx, sy, (int)cp);
        int bx0 = (int)left + gx0, by0 = (int)baseline + gy0;
        for (int y = 0; y < gh; y++) {
            int ty = by0 + y;
            if (ty < 0 || ty >= (int)bxh) continue;
            for (int x = 0; x < gw; x++) {
                int tx = bx0 + x;
                if (tx < 0 || tx >= (int)bxw) continue;
                uint8_t *p = px + ((size_t)(oy + ty) * W + ox + tx) * 4;
                uint8_t a = glyph[y * gw + x];
                if (a > p[3]) p[3] = a;
            }
        }
        drawn++;
    }
    free(glyph);
    bool ok = vk_tex_replace(b, px, W, H, srgb, 0);
    free(px);
    LOG("[hangul] font page %d: %d syllables drawn from Paperlogy (%ux%u)%s\n", page, drawn, W, H, ok ? "" : ": no memory");
    return ok;
}

// ---- Hangul as the patch's codes, for a plugin's text in the game's font (forge.c: mhxx_hangul_encode). The codes are
// the patch font's: the glyphs from U+7A00 on, in its grid (read_boxes), whatever [Display] KoreanFont says.
static uint32_t enc_codes[SYLLABLES];
static SDL_AtomicInt enc_state;  // 0 not looked at, 1 the patch's font, 2 not

static bool enc_ready(void) {
    int st = SDL_GetAtomicInt(&enc_state);
    if (st) return st == 1;
    static SDL_SpinLock lock;
    SDL_LockSpinlock(&lock);
    if (!(st = SDL_GetAtomicInt(&enc_state))) {
        const char *path = romfs_host_path("nativeNX/arc/resident.arc");
        SDL_IOStream *io = path ? SDL_IOFromFile(path, "rb") : NULL;
        Sint64 n = io ? SDL_GetIOSize(io) : -1;
        uint8_t *arc = n > 12 ? malloc((size_t)n) : NULL;
        bool ok = arc && SDL_ReadIO(io, arc, (size_t)n) == (size_t)n;
        if (io) SDL_CloseIO(io);
        uint32_t gn = 0;
        uint8_t *gfd = ok ? member(arc, (long)n, "GUI\\02_common\\font\\font_festa", ARC_GFD, &gn) : NULL;
        static Box tmp[SYLLABLES];
        st = gfd && read_boxes_into(gfd, gn, tmp, enc_codes) ? 1 : 2;
        free(gfd), free(arc);
        LOG("[hangul] plugins' Hangul in the patch's codes: %s\n", st == 1 ? "yes" : "no (not the Korean patch's font)");
        SDL_SetAtomicInt(&enc_state, st);
    }
    SDL_UnlockSpinlock(&lock);
    return st == 1;
}

static uint32_t encode(const char *in, char *out, uint32_t size, bool typed) {
    if (!in || !out || !size || !enc_ready()) return 0;
    uint32_t n = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p;) {
        uint32_t c = *p, len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!len) return 0;
        for (uint32_t k = 1; k < len; k++) {
            if ((p[k] & 0xc0) != 0x80) return 0;
            c = (c << 6) | (p[k] & 0x3f);
        }
        if (len > 1) c &= len == 2 ? 0x7ff : len == 3 ? 0xffff : 0x1fffff;
        p += len;
        if (c >= 0xac00 && c <= 0xd7a3) {  // (a syllable outside KS X 1001's 2350: no glyph in the patch)
            int lo = 0, hi = SYLLABLES - 1, at = -1;
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                if (ksx1001_hangul[mid] == c) { at = mid; break; }
                if (ksx1001_hangul[mid] < c) lo = mid + 1;
                else hi = mid - 1;
            }
            if (at < 0 && !typed) return 0;
            c = at < 0 ? '?' : enc_codes[at];  // (typed: a syllable the patch can't show becomes ?)
        }
        char b[4];
        uint32_t k = c < 0x80 ? (b[0] = (char)c, 1)
                   : c < 0x800 ? (b[0] = (char)(0xc0 | c >> 6), b[1] = (char)(0x80 | (c & 0x3f)), 2)
                   : (b[0] = (char)(0xe0 | c >> 12), b[1] = (char)(0x80 | ((c >> 6) & 0x3f)), b[2] = (char)(0x80 | (c & 0x3f)), 3);
        if (c >= 0x10000) return 0;
        if (n + k + 1 > size) return 0;
        memcpy(out + n, b, k), n += k;
    }
    out[n] = 0;
    return n;
}

// Hangul for plugins (forge.c mhxx_hangul_encode): 0 when a syllable has no glyph or the patch isn't there.
uint32_t hangulfont_encode(const char *in, char *out, uint32_t size) { return encode(in, out, size, false); }

// Typed text for the game's text entries (hle/swkbd.c: chat, names): Hangul in the patch's codes, so the game shows
// it and other players with the patch read it; a syllable outside the patch's 2350 becomes ?. Without the Korean patch
// (or on any trouble) the text as typed.
void hangulfont_typed(const char *in, char *out, uint32_t size) {
    if (!size) return;
    if (!encode(in, out, size, true)) snprintf(out, size, "%s", in ? in : "");
}

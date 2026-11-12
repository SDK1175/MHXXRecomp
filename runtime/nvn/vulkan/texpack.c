// Texture packs, part one: the dump ([Graphics] TextureDump). Every 2D texture the game uploads the first time
// (not a render target, not one it rewrites each frame) is saved once as textures/dump/<key>.png: its full-size
// level, decoded to RGBA8. key is a hash of that level's data, its size and its format, the same each run and for
// everyone with the same game files, so a pack made from the dump (upscaled PNGs under the same names) finds its
// textures again (part two, the loader). textures/dump/index.txt lists each one's size and kind ("color",
// "normal" for the two-channel BC5 maps that hold surface bumps, ...), to pick what to upscale and how.
//
// The render thread only copies the level out (deswizzled); hashing, decoding and writing the PNG happen on a
// thread of their own, so dumping doesn't stall the game. At most QUEUE_MAX bytes wait; more are skipped this run
// and dumped another time they are uploaded (the next run).
#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vk.h"
#include "../astc.h"
#include "../bcn.h"
#include "../tegra.h"

#define DUMP_DIR "textures/dump"
#define QUEUE_MAX (512ull << 20)
#define MIN_SIDE 16  // (smaller ones: gradients, lookup tables, nothing to upscale)

enum { K_RGBA8, K_BGRA8, K_BC1, K_BC2, K_BC3, K_BC4, K_BC5, K_ASTC, K_NONE };
static const char *const kind_names[] = {"color", "color", "color", "color", "color", "mask", "normal", "color"};

typedef struct Job {
    struct Job *next;
    uint8_t *data;  // level 0, deswizzled: rows of blocks (texels for RGBA8)
    uint64_t size;
    uint32_t w, h, kind, nvn_format, astc_bw, astc_bh;
    bool srgb;
} Job;

static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cond = PTHREAD_COND_INITIALIZER;
static Job *q_head, *q_tail;
static uint64_t q_bytes;
static bool started;
static uint32_t dumped, skipped_full, unsupported[256];

// FNV-1a 64 in 8-byte steps: stable across runs and builds (the pack's file names depend on it)
uint64_t texpack_key(const uint8_t *p, uint64_t n, uint32_t w, uint32_t h, uint32_t nvn_format) {
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

// a small open-addressing set of the keys written (or found already there) this run
static uint64_t *seen;
static uint32_t seen_cap, seen_n;
static bool seen_add(uint64_t k) {
    if (!k) k = 1;
    if (seen_n * 2 >= seen_cap) {
        uint32_t cap = seen_cap ? seen_cap * 2 : 4096;
        uint64_t *n = calloc(cap, sizeof *n);
        if (!n) return false;
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

static uint8_t *decode(const Job *j) {
    uint64_t n = (uint64_t)j->w * j->h;
    uint8_t *out = malloc(n * 4);
    if (!out) return NULL;
    switch (j->kind) {
    case K_RGBA8: memcpy(out, j->data, n * 4); break;
    case K_BGRA8:
        for (uint64_t i = 0; i < n; i++)
            out[i * 4] = j->data[i * 4 + 2], out[i * 4 + 1] = j->data[i * 4 + 1], out[i * 4 + 2] = j->data[i * 4],
            out[i * 4 + 3] = j->data[i * 4 + 3];
        break;
    case K_BC1: bcn_decode(BCN_BC1, j->data, j->w, j->h, out); break;
    case K_BC2: bcn_decode(BCN_BC2, j->data, j->w, j->h, out); break;
    case K_BC3: bcn_decode(BCN_BC3, j->data, j->w, j->h, out); break;
    case K_BC4: case K_BC5: {  // R8 / RG8 -> RGBA8 (BC4 grey, BC5 as red / green with blue 0, alpha 255)
        int k = j->kind == K_BC4 ? BCN_BC4U : BCN_BC5U;
        uint32_t ts = bcn_texel_size(k);
        uint8_t *t = malloc(n * ts);
        if (!t) { free(out); return NULL; }
        bcn_decode(k, j->data, j->w, j->h, t);
        for (uint64_t i = 0; i < n; i++) {
            uint8_t r = t[i * ts], g = ts > 1 ? t[i * ts + 1] : r, b = ts > 1 ? 0 : r;
            out[i * 4] = r, out[i * 4 + 1] = g, out[i * 4 + 2] = b, out[i * 4 + 3] = 255;
        }
        free(t);
        break;
    }
    case K_ASTC: astc_decode_ldr(j->data, j->w, j->h, j->astc_bw, j->astc_bh, j->srgb, out); break;
    default: free(out); return NULL;
    }
    return out;
}

static void *dump_thread(void *arg) {
    (void)arg;
    SDL_CreateDirectory("textures");
    SDL_CreateDirectory(DUMP_DIR);
    for (;;) {
        pthread_mutex_lock(&q_lock);
        while (!q_head) pthread_cond_wait(&q_cond, &q_lock);
        Job *j = q_head;
        q_head = j->next;
        if (!q_head) q_tail = NULL;
        q_bytes -= j->size;
        pthread_mutex_unlock(&q_lock);
        uint64_t key = texpack_key(j->data, j->size, j->w, j->h, j->nvn_format);
        char path[96];
        snprintf(path, sizeof path, DUMP_DIR "/%016llx.png", (unsigned long long)key);
        bool blank = true;  // (not filled in yet when first uploaded: left for a later run)
        for (uint64_t i = 0; blank && i < j->size; i++) blank = !j->data[i];
        if (!blank && seen_add(key)) {
            SDL_PathInfo info;
            if (!SDL_GetPathInfo(path, &info)) {  // (already dumped in an earlier run: kept as it is)
                uint8_t *px = decode(j);
                if (px) {
                    vk_save_png(path, px, j->w, j->h);
                    free(px);
                    FILE *f = fopen(DUMP_DIR "/index.txt", "a");
                    if (f) {
                        fprintf(f, "%016llx %ux%u %s%s nvn=%#x\n", (unsigned long long)key, j->w, j->h,
                                kind_names[j->kind], j->srgb ? " srgb" : "", j->nvn_format);
                        fclose(f);
                    }
                    if (!(++dumped % 500)) LOG("[texpack] %u textures dumped\n", dumped);
                }
            }
        }
        free(j->data);
        free(j);
    }
    return NULL;
}

static int kind_of(VkFormat vf, bool *srgb) {
    *srgb = false;
    switch (vf) {
    case VK_FORMAT_R8G8B8A8_SRGB: *srgb = true; /* fallthrough */
    case VK_FORMAT_R8G8B8A8_UNORM: return K_RGBA8;
    case VK_FORMAT_B8G8R8A8_SRGB: *srgb = true; /* fallthrough */
    case VK_FORMAT_B8G8R8A8_UNORM: return K_BGRA8;
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: *srgb = true; /* fallthrough */
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return K_BC1;
    case VK_FORMAT_BC2_SRGB_BLOCK: *srgb = true; /* fallthrough */
    case VK_FORMAT_BC2_UNORM_BLOCK: return K_BC2;
    case VK_FORMAT_BC3_SRGB_BLOCK: *srgb = true; /* fallthrough */
    case VK_FORMAT_BC3_UNORM_BLOCK: return K_BC3;
    case VK_FORMAT_BC4_UNORM_BLOCK: return K_BC4;
    case VK_FORMAT_BC5_UNORM_BLOCK: return K_BC5;
    default: return K_NONE;
    }
}

// (textures.c upload_texture, on the render thread) t's contents are being uploaded for the first time. mem: its
// guest memory; vf: its Vulkan format (before any CPU decoding); linear: pitch layout (stride t->stride).
void texpack_first_upload(NvnTexture *t, const uint8_t *mem, VkFormat vf, bool linear) {
    if (!g_config.texture_dump) return;
    const FormatInfo *fi = t->layout.fmt;
    if (!fi || t->target != 1 || t->depth > 1 || (t->layout.layers && t->layout.layers > 1)) return;
    if (t->width < MIN_SIDE || t->height < MIN_SIDE || (t->flags & 0x10)) return;
    bool srgb;
    int kind = kind_of(vf, &srgb);
    bool astc = vf >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && vf <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
    if (astc) kind = K_ASTC, srgb = (vf - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) & 1;
    if (kind == K_NONE) {
        if (!unsupported[t->format & 0xff]++)
            LOG("[texpack] nvn format %#x (vk %d) not dumped: no decoder\n", t->format, vf);
        return;
    }
    LevelLayout l = tegra_level(&t->layout, 0);
    uint64_t size = (uint64_t)l.wb * l.hb * fi->bpb;
    pthread_mutex_lock(&q_lock);
    bool full = q_bytes + size > QUEUE_MAX;
    pthread_mutex_unlock(&q_lock);
    if (full) {
        if (!skipped_full++) LOG("[texpack] dump queue full: some textures are left for the next run\n");
        return;
    }
    Job *j = calloc(1, sizeof *j);
    uint8_t *data = j ? malloc(size) : NULL;
    if (!data) { free(j); return; }
    if (linear)
        for (uint32_t y = 0; y < l.hb; y++) memcpy(data + (uint64_t)y * l.wb * fi->bpb, mem + (uint64_t)y * t->stride, l.wb * fi->bpb);
    else
        tegra_deswizzle(mem + l.offset, data, l.wb, l.hb, 1, fi->bpb, l.block_height_log2, l.block_depth_log2);
    *j = (Job){.data = data, .size = size, .w = l.w, .h = l.h, .kind = (uint32_t)kind, .nvn_format = t->format,
               .astc_bw = fi->bw, .astc_bh = fi->bh, .srgb = srgb};
    pthread_mutex_lock(&q_lock);
    if (!started) {
        pthread_t th;
        started = pthread_create(&th, NULL, dump_thread, NULL) == 0;
        if (started) {
            pthread_detach(th);
            LOG("[texpack] dumping textures to " DUMP_DIR "/ ([Graphics] TextureDump)\n");
        }
    }
    if (q_tail) q_tail->next = j; else q_head = j;
    q_tail = j;
    q_bytes += size;
    pthread_cond_signal(&q_cond);
    pthread_mutex_unlock(&q_lock);
}

// ================================================================== part two: the loader
// textures/load/ (and its subfolders): <key>.png pictures, any size of the same shape as the texture (an upscale),
// each standing in for the texture whose key it is named after - the dump's names, or mhxx-texextract's
// (tools/texextract). Looked up when a texture is first uploaded; its picture is loaded then and becomes the image
// (vk_tex_replace), with its own mip levels. A texture the game rewrites later goes back to its own.
#define LOAD_DIR "textures/load"
typedef struct { uint64_t key; char *path; } PackEnt;
static PackEnt *pack;
static uint32_t pack_cap, pack_n;
static int pack_state = -1;  // -1 not scanned, 0 empty, 1 has pictures
static uint32_t replaced_n;

static bool hex16(const char *s, uint64_t *out) {
    uint64_t v = 0;
    for (int i = 0; i < 16; i++) {
        char c = s[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return false;
        v = v << 4 | (uint64_t)d;
    }
    *out = v;
    return true;
}

static void pack_add(uint64_t key, const char *path) {
    if (pack_n * 2 >= pack_cap) {
        uint32_t cap = pack_cap ? pack_cap * 2 : 4096;
        PackEnt *n = calloc(cap, sizeof *n);
        if (!n) return;
        for (uint32_t i = 0; i < pack_cap; i++)
            if (pack[i].path)
                for (uint32_t j = (uint32_t)pack[i].key & (cap - 1);; j = (j + 1) & (cap - 1))
                    if (!n[j].path) { n[j] = pack[i]; break; }
        free(pack);
        pack = n, pack_cap = cap;
    }
    for (uint32_t j = (uint32_t)key & (pack_cap - 1);; j = (j + 1) & (pack_cap - 1)) {
        if (pack[j].path && pack[j].key == key) return;  // (the same name twice: the first found)
        if (!pack[j].path) { pack[j].key = key, pack[j].path = SDL_strdup(path), pack_n++; return; }
    }
}

static const char *pack_find(uint64_t key) {
    if (!pack_n) return NULL;
    for (uint32_t j = (uint32_t)key & (pack_cap - 1);; j = (j + 1) & (pack_cap - 1)) {
        if (!pack[j].path) return NULL;
        if (pack[j].key == key) return pack[j].path;
    }
}

static void pack_scan(void) {
    pack_state = 0;
    // [Graphics] TexturePack: Off none of it; Gear all but monsters/; All everything (Auto: by the card's memory)
    int mode = texpack_effective();
    LOG("[texpack] texture pack: %s%s (graphics memory %.1f GB)\n",
        mode == TEXPACK_OFF ? "off" : mode == TEXPACK_GEAR ? "gear" : "gear and monsters",
        g_config.texture_pack == TEXPACK_AUTO ? " (auto)" : "", g_gpu_vram / 1073741824.0);
    if (mode == TEXPACK_OFF) return;
    int count = 0;
    char **files = SDL_GlobDirectory(LOAD_DIR, NULL, 0, &count);  // (everything, subfolders too: .png picked below)
    for (int i = 0; files && i < count; i++) {
        const char *base = strrchr(files[i], '/');
        const char *bs = strrchr(files[i], '\\');
        if (bs && (!base || bs > base)) base = bs;
        base = base ? base + 1 : files[i];
        uint64_t key;
        if (mode != TEXPACK_ALL && !SDL_strncasecmp(files[i], "monsters", 8) && (files[i][8] == '/' || files[i][8] == '\\'))
            continue;
        if (strlen(base) == 20 && SDL_strcasecmp(base + 16, ".png") == 0 && hex16(base, &key)) {
            char path[1024];
            snprintf(path, sizeof path, LOAD_DIR "/%s", files[i]);
            pack_add(key, path);
        }
    }
    SDL_free(files);
    if (pack_n) {
        pack_state = 1;
        LOG("[texpack] texture pack: %u pictures in " LOAD_DIR "/\n", pack_n);
    }
}

bool texpack_replace(NvnTexture *t, TexBackend *b, const uint8_t *mem, VkFormat vf, bool linear) {
    if (pack_state < 0) pack_scan();
    bool buttons = !linear && buttonicons_size(t->width, t->height);  // (the button prompts: buttonicons.c)
    if ((pack_state != 1 && !buttons) || linear) return false;
    const FormatInfo *fi = t->layout.fmt;
    if (!fi || t->target != 1 || t->depth > 1 || (t->layout.layers && t->layout.layers > 1)) return false;
    if (t->width < MIN_SIDE || t->height < MIN_SIDE || t->samples > 1) return false;
    bool srgb;
    if (kind_of(vf, &srgb) == K_NONE && !(vf >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && vf <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK))
        return false;
    if (vf >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && vf <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK)
        srgb = (vf - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) & 1;
    LevelLayout l = tegra_level(&t->layout, 0);
    uint64_t size = (uint64_t)l.wb * l.hb * fi->bpb;
    uint8_t *lin = malloc(size);
    if (!lin) return false;
    tegra_deswizzle(mem + l.offset, lin, l.wb, l.hb, 1, fi->bpb, l.block_height_log2, l.block_depth_log2);
    uint64_t key = texpack_key(lin, size, l.w, l.h, t->format);
    const char *path = pack_state == 1 ? pack_find(key) : NULL;
    int button_target = -1;
    if (buttons) {  // the game's picture, decoded: one with button pictures? (then painted with the controller's)
        Job j = {.data = lin, .size = size, .w = l.w, .h = l.h, .kind = (uint32_t)kind_of(vf, &srgb), .nvn_format = t->format,
                 .srgb = srgb};
        uint8_t *px = j.kind != K_NONE && j.kind != K_ASTC ? decode(&j) : NULL;
        button_target = buttonicons_target(key, px, l.w, l.h);
        if (button_target >= 0 && !path) {
            bool ok = buttonicons_apply(b, button_target, px, l.w, l.h, srgb);
            free(px), free(lin);
            return ok;
        }
        free(px);
    }
    free(lin);
    if (!path) return false;
    SDL_Surface *s = SDL_LoadPNG(path);
    SDL_Surface *c = s ? SDL_ConvertSurface(s, SDL_PIXELFORMAT_ABGR8888) : NULL;  // (bytes R, G, B, A)
    SDL_DestroySurface(s);
    if (!c) {
        LOG("[texpack] %s: can't be read (%s)\n", path, SDL_GetError());
        return false;
    }
    uint32_t w = (uint32_t)c->w, h = (uint32_t)c->h;
    bool ok = false;
    if ((uint64_t)w * l.h != (uint64_t)h * l.w || w < l.w || w > 16384 || h > 16384) {
        LOG("[texpack] %s: %ux%u, not the shape of the texture (%ux%u): left out\n", path, w, h, l.w, l.h);
    } else {
        uint8_t *px = c->pixels;
        uint8_t *tight = NULL;
        if (c->pitch != (int)w * 4) {
            tight = malloc((uint64_t)w * h * 4);
            if (tight)
                for (uint32_t y = 0; y < h; y++) memcpy(tight + (uint64_t)y * w * 4, px + (uint64_t)y * c->pitch, (size_t)w * 4);
            px = tight;
        }
        ok = px && ((button_target >= 0 && buttonicons_apply(b, button_target, px, w, h, srgb)) || vk_tex_replace(b, px, w, h, srgb));  // (the
        // button prompts are painted on a pack's picture too)
        free(tight);
        if (ok && replaced_n++ < 20) LOG("[texpack] %016llx %ux%u -> %ux%u\n", (unsigned long long)key, l.w, l.h, w, h);
        if (ok && !(replaced_n % 500)) LOG("[texpack] %u textures replaced\n", replaced_n);
        if (!ok && px) LOG("[texpack] %s: no graphics memory for %ux%u: the game's own kept\n", path, w, h);
    }
    SDL_DestroySurface(c);
    return ok;
}

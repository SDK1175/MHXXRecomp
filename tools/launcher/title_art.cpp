// The launcher's pictures, taken from the player's own game files when it starts (nothing of the game's is in the
// launcher): the title screen (GUI\99_texture\HD_title01, 1920x1080 in a 2048 texture: the logo on its blue) and the
// stone emblem behind the title menu (HD_title_down), out of nativeNX/arc/title.arc - a mod's copy first (the Korean
// patch has one), else the game's. Decoded as tools/texextract does: .arc entries (zlib), TEX header, the block-
// linear level 0 deswizzled, BC1 / BC3 / RGBA8 to RGBA.
#include "title_art.h"

#include <SDL3/SDL.h>
#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "nvn/bcn.h"
#include "nvn/tegra.h"
}

namespace {

bool read_file(const std::string &path, std::vector<uint8_t> &out) {
    size_t n = 0;
    void *d = SDL_LoadFile(path.c_str(), &n);
    if (!d) return false;
    out.assign((uint8_t *)d, (uint8_t *)d + n);
    SDL_free(d);
    return true;
}

// title.arc: the mods' (any layout the game reads: romfs/, <title id>/romfs/, atmosphere/contents/<title id>/romfs/)
// before the game's own
std::string find_title_arc(const char *game_dir) {
    const char *rel = "nativeNX/arc/title.arc";
    int n = 0;
    char **mods = SDL_GlobDirectory("mods", nullptr, 0, &n);
    std::string found;
    for (int i = 0; mods && i < n && found.empty(); i++) {
        std::string p = mods[i];
        for (char &c : p)
            if (c == '\\') c = '/';
        if (p.size() > strlen(rel) && p.compare(p.size() - strlen(rel), std::string::npos, rel) == 0 &&
            p.find("romfs/") != std::string::npos)
            found = std::string("mods/") + p;
    }
    SDL_free(mods);
    if (!found.empty()) return found;
    return std::string(game_dir) + "/romfs/" + rel;
}

// One texture of an .arc (its name ending in `name`) as RGBA: true with w x h pixels
bool arc_texture(const std::vector<uint8_t> &arc, const char *name, std::vector<uint8_t> &rgba, uint32_t &w,
                 uint32_t &h) {
    if (arc.size() < 12 || memcmp(arc.data(), "ARC", 4)) return false;
    uint16_t count;
    memcpy(&count, arc.data() + 6, 2);
    for (uint32_t i = 0; i < count; i++) {
        size_t e = 12 + (size_t)i * 0x50;
        if (e + 0x50 > arc.size()) break;
        char ename[65];
        memcpy(ename, arc.data() + e, 64), ename[64] = 0;
        size_t nl = strlen(ename), ql = strlen(name);
        if (nl < ql || strcmp(ename + nl - ql, name)) continue;
        uint32_t type, csize, usize, off;
        memcpy(&type, arc.data() + e + 64, 4), memcpy(&csize, arc.data() + e + 68, 4);
        memcpy(&usize, arc.data() + e + 72, 4), memcpy(&off, arc.data() + e + 76, 4);
        if (type != 0x241f5debu || (uint64_t)off + csize > arc.size()) return false;
        std::vector<uint8_t> t(usize & 0x1fffffff);
        uLongf un = t.size();
        if (uncompress(t.data(), &un, arc.data() + off, csize) != Z_OK || un < 0x18 || memcmp(t.data(), "TEX", 4))
            return false;
        uint32_t d2, d3;
        memcpy(&d2, t.data() + 8, 4), memcpy(&d3, t.data() + 12, 4);
        uint32_t levels = d2 & 0x3f, code = (d3 >> 8) & 0xff;
        w = (d2 >> 6) & 0x1fff, h = (d2 >> 19) & 0x1fff;
        uint32_t nvn;
        int bcn;
        switch (code) {
        case 0x07: nvn = 0x25, bcn = 0; break;
        case 0x13: nvn = 0x42, bcn = BCN_BC1; break;
        case 0x17: nvn = 0x44, bcn = BCN_BC3; break;
        default: return false;
        }
        TexLayout lay = {};
        lay.width = w, lay.height = h, lay.depth = 1, lay.layers = 1, lay.levels = levels ? levels : 1;
        lay.fmt = nvn_format_info(nvn);
        if (!lay.fmt) return false;
        tegra_layout_init(&lay);
        LevelLayout l = tegra_level(&lay, 0);
        uint64_t size = (uint64_t)l.wb * l.hb * lay.fmt->bpb;
        uint64_t data = 0x14 + 4ull * lay.levels;
        uint32_t level0 = 0;
        if (data + 4 <= un) memcpy(&level0, t.data() + 0x14, 4);
        data += level0;
        if (data + size > un) return false;
        std::vector<uint8_t> lin(size);
        tegra_deswizzle(t.data() + data, lin.data(), l.wb, l.hb, 1, lay.fmt->bpb, l.block_height_log2,
                        l.block_depth_log2);
        rgba.resize((size_t)w * h * 4);
        if (!bcn) memcpy(rgba.data(), lin.data(), rgba.size());
        else bcn_decode(bcn, lin.data(), w, h, rgba.data());
        return true;
    }
    return false;
}

SDL_Texture *make_texture(SDL_Renderer *rd, const uint8_t *px, int w, int h, int pitch, SDL_ScaleMode mode) {
    SDL_Texture *t = SDL_CreateTexture(rd, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t) return nullptr;
    SDL_UpdateTexture(t, nullptr, px, pitch);
    SDL_SetTextureScaleMode(t, mode);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

}  // namespace

bool title_art_load(SDL_Renderer *rd, const char *game_dir, TitleArt *art) {
    *art = TitleArt{};
    std::vector<uint8_t> arc;
    std::string path = find_title_arc(game_dir);
    if (!read_file(path, arc)) return false;
    std::vector<uint8_t> px;
    uint32_t w, h;
    if (arc_texture(arc, "HD_title01_BM_NOMIP", px, w, h) && w >= 1920 && h >= 1080) {
        int pitch = (int)w * 4;
        // the logo, crisp (its part of the 1920x1080 picture, without the copyright line under it)
        SDL_Rect logo = {300, 120, 1320, 840};
        std::vector<uint8_t> lp((size_t)logo.w * logo.h * 4);
        const float feather = 110;  // (its blue fading out at the edges, into the panel behind it)
        for (int y = 0; y < logo.h; y++)
            for (int x = 0; x < logo.w; x++) {
                const uint8_t *src = px.data() + (size_t)(logo.y + y) * pitch + (size_t)(logo.x + x) * 4;
                uint8_t *o = lp.data() + ((size_t)y * logo.w + x) * 4;
                memcpy(o, src, 3);
                float e = std::min(std::min((float)x, (float)(logo.w - 1 - x)), std::min((float)y, (float)(logo.h - 1 - y)));
                float a = std::clamp(e / feather, 0.f, 1.f);
                o[3] = (uint8_t)(255 * a * a * (3 - 2 * a));
            }
        art->logo = make_texture(rd, lp.data(), logo.w, logo.h, logo.w * 4, SDL_SCALEMODE_LINEAR);
        art->logo_w = logo.w, art->logo_h = logo.h;
        // the background: the whole picture at a quarter, blurred (three box passes) and darker
        const int f = 4, bw = 1920 / f, bh = 1080 / f;
        std::vector<float> img((size_t)bw * bh * 3), tmp(img.size());
        for (int y = 0; y < bh; y++)
            for (int x = 0; x < bw; x++)
                for (int c = 0; c < 3; c++) {
                    uint32_t sum = 0;
                    for (int yy = 0; yy < f; yy++)
                        for (int xx = 0; xx < f; xx++) sum += px[(size_t)(y * f + yy) * pitch + (size_t)(x * f + xx) * 4 + c];
                    img[((size_t)y * bw + x) * 3 + c] = sum / float(f * f);
                }
        const int r = 6;
        for (int pass = 0; pass < 3; pass++) {
            for (int y = 0; y < bh; y++)  // horizontal
                for (int x = 0; x < bw; x++)
                    for (int c = 0; c < 3; c++) {
                        float sum = 0;
                        for (int k = -r; k <= r; k++) sum += img[((size_t)y * bw + std::clamp(x + k, 0, bw - 1)) * 3 + c];
                        tmp[((size_t)y * bw + x) * 3 + c] = sum / (2 * r + 1);
                    }
            for (int y = 0; y < bh; y++)  // vertical
                for (int x = 0; x < bw; x++)
                    for (int c = 0; c < 3; c++) {
                        float sum = 0;
                        for (int k = -r; k <= r; k++) sum += tmp[((size_t)std::clamp(y + k, 0, bh - 1) * bw + x) * 3 + c];
                        img[((size_t)y * bw + x) * 3 + c] = sum / (2 * r + 1);
                    }
        }
        std::vector<uint8_t> small((size_t)bw * bh * 4);
        for (size_t i = 0; i < (size_t)bw * bh; i++) {
            for (int c = 0; c < 3; c++) small[i * 4 + c] = (uint8_t)std::min(255.f, img[i * 3 + c] * .7f);
            small[i * 4 + 3] = 255;
        }
        art->background = make_texture(rd, small.data(), bw, bh, bw * 4, SDL_SCALEMODE_LINEAR);
    }
    if (arc_texture(arc, "HD_title_down_BM_NOMIP", px, w, h))
        art->emblem = make_texture(rd, px.data(), (int)w, (int)h, (int)w * 4, SDL_SCALEMODE_LINEAR),
        art->emblem_w = (int)w, art->emblem_h = (int)h;
    return art->background || art->emblem;
}

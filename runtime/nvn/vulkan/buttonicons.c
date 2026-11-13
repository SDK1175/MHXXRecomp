// The game's button prompts as an Xbox or a PlayStation controller's ([Controls] GameButtons): the pictures of the
// Switch's buttons in two of the game's textures are painted over, as each is uploaded, with the port's own
// (tools/buttonicons.py draws them: buttonicons_png.h). The text's prompts come from the button font
// (button_festa_00_ID, 24-pixel cells), the HUD's from two sheets (HD_hud00, HD_custom_pnl2: 32-pixel cells). Found by the textures'
// keys (texpack.c), so a texture pack's picture of them is painted the same, at its size. Each Switch button shows the
// controller button it is pressed with ([Controller] in F1, FaceButtons), painted again when they change.
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#include "vk.h"
#include "../../config.h"
#include "../../overlay.h"
#include "buttonicons_png.h"

enum { G_A, G_B, G_X, G_Y, G_L, G_R, G_ZL, G_ZR, G_PLUS, G_MINUS, G_N };
typedef struct { int g; uint16_t x, y; } Cell;
typedef struct {
    uint64_t key;
    uint16_t w, h, cell;  // cell: its size in the texture (a picture of the sheet's 24 or, else, 32 drawn that size)
    int n;
    Cell cells[G_N];
    uint16_t fx, fy, fw, fh;  // the buttons' strip, and its FNV-1a of RGBA8: the same picture with other game files
    uint64_t fnv;              // around it (the Korean patch's) has another key
} Target;
static const Target targets[] = {
    {0x473308d6d9f3b584ull, 256, 256, 24, 10,  // GUI/02_common/font/button_festa_00_ID
     {{G_Y, 0, 0}, {G_X, 24, 0}, {G_B, 48, 0}, {G_A, 72, 0}, {G_L, 96, 0}, {G_R, 0, 24}, {G_ZL, 120, 24},
      {G_ZR, 144, 24}, {G_PLUS, 0, 72}, {G_MINUS, 24, 72}},
     0, 0, 120, 24, 0xc6a5df3096b1a843ull},
    {0xd58014a25649b59dull, 512, 512, 32, 6,  // GUI/99_texture/HD_hud00_BM_NOMIP (quest.arc)
     {{G_L, 0, 408}, {G_Y, 32, 408}, {G_X, 64, 408}, {G_B, 96, 408}, {G_A, 128, 408}, {G_R, 160, 408}},
     0, 408, 192, 32, 0xc01d4435a5d40cd8ull},
    {0x8be885b4b732c2dbull, 512, 512, 32, 10,  // GUI/99_texture/HD_custom_pnl2_BM_NOMIP (resident.arc): the HUD's small
     {{G_L, 4, 244}, {G_Y, 36, 244}, {G_X, 68, 244}, {G_B, 100, 244}, {G_A, 132, 244}, {G_R, 164, 244},  // ones (the
      {G_ZL, 196, 244}, {G_ZR, 228, 244}, {G_MINUS, 388, 244}, {G_PLUS, 420, 244}},  // item bar's, the map's ZL)
     4, 244, 256, 32, 0x343664272dbc9285ull},
    {0x216e71907819e692ull, 512, 512, 28, 4,  // GUI/99_texture/HD_lobby00_BM_NOMIP (resident.arc): the lobby's
     {{G_Y, 160, 64}, {G_X, 188, 64}, {G_A, 216, 64}, {G_B, 244, 64}},
     160, 64, 112, 28, 0xcd087e84035ec4b9ull},
};

// The sheet's row: 0 Xbox, 1 PlayStation; 2 the keyboard (keys with their names); -1 the Switch's own pictures
enum { STYLE_KEYS = 2 };
int buttonicons_style(void) {
    int v = g_config.game_buttons, layout = host_pad_layout();
    if (v == GAME_BUTTONS_AUTO)
        v = layout == 0 ? GAME_BUTTONS_XBOX : layout == 2 ? GAME_BUTTONS_PS : layout == 1 ? GAME_BUTTONS_SWITCH : GAME_BUTTONS_KEYS;
    return v == GAME_BUTTONS_XBOX ? 0 : v == GAME_BUTTONS_PS ? 1 : v == GAME_BUTTONS_KEYS ? STYLE_KEYS : -1;
}

// ---- the keyboard: a blank key with the bound key's name, up to three letters (the sheet's letters)
#define SHEET_KEYS_Y 112  // the blank keys: 24 at x 0, 32 at x 24; the letters below them
#define SHEET_CHARS_Y 144
#define CW 12
#define CH 16
static const char CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789^v<>";

static int key_of(int g) {  // the Switch button's first key in [Keybinds] (BIND_A.. in the same order)
    int k = config_live_bind(BIND_A + g, 0);
    return k ? k : config_live_bind(BIND_A + g, 1);
}

static void short_name(int code, char out[4]) {
    out[0] = 0;
    if (!code) return;
    static const struct { const char *name, *s; } names[] = {
        {"Space", "SPC"}, {"Left Shift", "SFT"}, {"Right Shift", "SFT"}, {"Left Ctrl", "CTL"}, {"Right Ctrl", "CTL"},
        {"Left Alt", "ALT"}, {"Right Alt", "ALT"}, {"Tab", "TAB"}, {"Return", "ENT"}, {"Keypad Enter", "ENT"},
        {"Escape", "ESC"}, {"Backspace", "BS"}, {"CapsLock", "CAP"}, {"Up", "^"}, {"Down", "v"}, {"Left", "<"},
        {"Right", ">"}, {"Mouse Left", "LMB"}, {"Mouse Right", "RMB"}, {"Mouse Middle", "MMB"}, {"Mouse X1", "M4"},
        {"Mouse X2", "M5"}, {"Wheel Up", "WU"}, {"Wheel Down", "WD"}, {"Delete", "DEL"}, {"Insert", "INS"},
        {"Home", "HOM"}, {"End", "END"}, {"PageUp", "PGU"}, {"PageDown", "PGD"},
    };
    const char *n = config_key_name(code);
    if (code == KEY_WHEEL_UP) n = "Wheel Up";
    if (code == KEY_WHEEL_DOWN) n = "Wheel Down";
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++)
        if (!SDL_strcasecmp(n, names[i].name)) { SDL_strlcpy(out, names[i].s, 4); return; }
    if (!SDL_strncasecmp(n, "Keypad ", 7)) n += 7;
    int k = 0;
    for (const char *p = n; *p && k < 3; p++)  // (the first letters and digits: "F11", "E", "Mouse 6" -> "MOU")
        if (SDL_isalnum((unsigned char)*p)) out[k++] = (char)SDL_toupper((unsigned char)*p);
    out[k] = 0;
}

// The key's picture, 32 x 32, from the blank key and the letters (into a surface: paint() draws it to the texture)
static SDL_Surface *key_cell(const SDL_Surface *sh, int code) {
    SDL_Surface *c = SDL_CreateSurface(32, 32, SDL_PIXELFORMAT_ABGR8888);
    if (!c) return NULL;
    for (int y = 0; y < 32; y++)
        memcpy((uint8_t *)c->pixels + (size_t)y * c->pitch, (const uint8_t *)sh->pixels + (size_t)(SHEET_KEYS_Y + y) * sh->pitch + 24 * 4, 32 * 4);
    char t[4];
    short_name(code, t);
    int len = (int)strlen(t);
    if (!len) return c;
    float k = SDL_min(23.f / (len * CW), 14.f / CH);  // (the key's face: about 24 x 15, from y 7)
    float x0 = 16 - len * CW * k / 2, y0 = 16.5f - CH * k / 2;
    for (int i = 0; i < len; i++) {
        const char *at = strchr(CHARS, t[i]);
        if (!at) continue;
        int sx = (int)(at - CHARS) * CW;
        for (int y = 0; y < 32; y++)
            for (int x = 0; x < 32; x++) {
                float fx = (x + .5f - (x0 + i * CW * k)) / k - .5f, fy = (y + .5f - y0) / k - .5f;
                if (fx < -1 || fy < -1 || fx > CW || fy > CH) continue;
                float a = 0;  // (the letter's coverage, bilinear)
                for (int j = 0; j < 4; j++) {
                    int cx = (int)SDL_floorf(fx) + (j & 1), cy = (int)SDL_floorf(fy) + (j >> 1);
                    if (cx < 0 || cy < 0 || cx >= CW || cy >= CH) continue;
                    float w = ((j & 1) ? fx - SDL_floorf(fx) : 1 - (fx - SDL_floorf(fx))) *
                              ((j >> 1) ? fy - SDL_floorf(fy) : 1 - (fy - SDL_floorf(fy)));
                    a += ((const uint8_t *)sh->pixels)[(size_t)(SHEET_CHARS_Y + cy) * sh->pitch + (size_t)(sx + cx) * 4 + 3] / 255.f * w;
                }
                uint8_t *o = (uint8_t *)c->pixels + (size_t)y * c->pitch + (size_t)x * 4;
                for (int ch = 0; ch < 3; ch++) o[ch] = (uint8_t)(o[ch] * (1 - a) + 34 * a);
            }
    }
    return c;
}

// The controller button a Switch button (PAD_A .. PAD_MINUS) is pressed with: [Controller], else the usual one
// (host_input.c: the face buttons by position, or by label on an Xbox controller with FaceButtons = Label). The
// sheet's column (PAD_SRC_* from south), -1: none (the game's picture is left)
static int source_of(int g, int style) {
    int src = config_live_pad_bind(g);
    if (src == PAD_SRC_NONE) return -1;
    if (src == PAD_SRC_DEFAULT) {
        static const int pos[G_N] = {PAD_SRC_EAST, PAD_SRC_SOUTH, PAD_SRC_NORTH, PAD_SRC_WEST, PAD_SRC_LB, PAD_SRC_RB,
                                     PAD_SRC_LT, PAD_SRC_RT, PAD_SRC_START, PAD_SRC_BACK};
        static const int label[4] = {PAD_SRC_SOUTH, PAD_SRC_EAST, PAD_SRC_WEST, PAD_SRC_NORTH};
        src = g < 4 && style == 0 && config_live_face_buttons() == FACE_LABEL ? label[g] : pos[g];
    }
    return src >= PAD_SRC_SOUTH && src < PAD_SRCS ? src - PAD_SRC_SOUTH : -1;
}

bool buttonicons_size(uint32_t w, uint32_t h) {
    for (unsigned i = 0; i < sizeof targets / sizeof *targets; i++)
        if (targets[i].w == w && targets[i].h == h) return true;
    return false;
}

static SDL_Surface *sheet(void) {
    static SDL_Surface *s;
    static bool tried;
    if (tried) return s;
    tried = true;
    SDL_Surface *p = SDL_LoadPNG_IO(SDL_IOFromConstMem(BUTTON_ICONS_PNG, sizeof BUTTON_ICONS_PNG), true);
    s = p ? SDL_ConvertSurface(p, SDL_PIXELFORMAT_ABGR8888) : NULL;  // (bytes R, G, B, A)
    SDL_DestroySurface(p);
    if (!s) LOG("[buttons] the button pictures can't be read (%s)\n", SDL_GetError());
    return s;
}

// The cell's picture (n pixels, from the sheet's row for this size) at (dx, dy), scale k (a texture pack's bigger
// picture): bilinear, over a cleared cell
static void paint(uint8_t *px, uint32_t pw, uint32_t ph, const SDL_Surface *s, int sx, int sy, int n, float dx, float dy, float k,
                  float ck) {
    int x0 = (int)(dx * k), y0 = (int)(dy * k), m = (int)(n * ck * k + .5f);
    k *= ck;  // (from here: the sheet's pixels to the texture's)
    for (int y = 0; y < m; y++) {
        if (y0 + y < 0 || (uint32_t)(y0 + y) >= ph) continue;
        for (int x = 0; x < m; x++) {
            if (x0 + x < 0 || (uint32_t)(x0 + x) >= pw) continue;
            float fx = (x + .5f) / k - .5f, fy = (y + .5f) / k - .5f;
            int ix = (int)SDL_floorf(fx), iy = (int)SDL_floorf(fy);
            float ax = fx - ix, ay = fy - iy;
            float acc[4] = {0};
            for (int j = 0; j < 4; j++) {
                int cx = ix + (j & 1), cy = iy + (j >> 1);
                if (cx < 0) cx = 0;
                if (cy < 0) cy = 0;
                if (cx >= n) cx = n - 1;
                if (cy >= n) cy = n - 1;
                float wgt = ((j & 1) ? ax : 1 - ax) * ((j >> 1) ? ay : 1 - ay);
                const uint8_t *p = (const uint8_t *)s->pixels + (size_t)(sy + cy) * s->pitch + (size_t)(sx + cx) * 4;
                float a = p[3] / 255.f * wgt;  // (premultiplied while mixing)
                acc[0] += p[0] * a, acc[1] += p[1] * a, acc[2] += p[2] * a, acc[3] += a;
            }
            uint8_t *o = px + ((size_t)(y0 + y) * pw + (x0 + x)) * 4;
            if (acc[3] > 0) o[0] = (uint8_t)(acc[0] / acc[3]), o[1] = (uint8_t)(acc[1] / acc[3]), o[2] = (uint8_t)(acc[2] / acc[3]);
            else o[0] = o[1] = o[2] = 0;
            o[3] = (uint8_t)(acc[3] * 255.f + .5f);
        }
    }
}

// Each target's texture as it is now (the game's or a pack's picture, kept unpainted), to paint again when the setting
// or a button's binding changes: from the render thread, each frame (buttonicons_frame)
typedef struct {
    TexBackend *b;
    uint8_t *base;
    uint32_t w, h;
    bool srgb;
    uint32_t levels;  // (the game's own count for its picture: a HUD atlas has one, and made-up ones blur its pieces
                      // into their neighbours - a line beside the map, the clock, the item bar)
    uint64_t sig;  // what it was painted for (0: not yet)
} Live;
static Live live[sizeof targets / sizeof *targets];

static uint64_t signature(void) {
    int style = buttonicons_style();
    uint64_t sig = (uint64_t)(style + 2);
    for (int g = 0; g < G_N; g++) sig = sig * 31 + (uint64_t)(style == STYLE_KEYS ? key_of(g) : source_of(g, style) + 2);
    return sig;
}

static void paint_all(const Target *t, uint8_t *rgba, uint32_t w, uint32_t h, int style) {
    const SDL_Surface *s = sheet();
    if (!s || style < 0) return;
    float k = (float)w / t->w;
    int n = t->cell == 24 ? 24 : 32, row_y = n == 24 ? style * 24 : 2 * 24 + style * 32;
    for (int i = 0; i < t->n; i++) {
        const Cell *c = &t->cells[i];
        if (style == STYLE_KEYS) {
            int code = key_of(c->g);
            SDL_Surface *kc = code ? key_cell(s, code) : NULL;  // (no key: the game's picture is left)
            if (kc) paint(rgba, w, h, kc, 0, 0, 32, c->x, c->y, k, (float)t->cell / 32);
            SDL_DestroySurface(kc);
            continue;
        }
        int col = source_of(c->g, style);
        if (col >= 0) paint(rgba, w, h, s, col * n, row_y, n, c->x, c->y, k, (float)t->cell / n);
    }
}

int buttonicons_target(uint64_t key, const uint8_t *rgba, uint32_t w, uint32_t h) {
    for (unsigned i = 0; i < sizeof targets / sizeof *targets; i++) {
        const Target *t = &targets[i];
        if (t->w != w || t->h != h) continue;
        if (t->key == key) return (int)i;
        uint64_t f = 0xcbf29ce484222325ull;
        for (uint32_t y = t->fy; rgba && y < (uint32_t)t->fy + t->fh; y++)
            for (uint32_t k = 0; k < (uint32_t)t->fw * 4; k++) f = (f ^ rgba[((size_t)y * w + t->fx) * 4 + k]) * 0x100000001b3ull;
        if (rgba && f == t->fnv) {
            LOG("[buttons] %016llx (%ux%u): its button pictures are the known ones (another key)\n", (unsigned long long)key, w, h);
            return (int)i;
        }
    }
    return -1;
}

bool buttonicons_apply(TexBackend *b, int ti, const uint8_t *rgba, uint32_t w, uint32_t h, bool srgb, uint32_t max_levels) {
    if (ti < 0 || ti >= (int)(sizeof targets / sizeof *targets)) return false;
    Live *l = &live[ti];
    uint8_t *base = malloc((size_t)w * h * 4), *px = malloc((size_t)w * h * 4);
    if (!base || !px) { free(base), free(px); return false; }
    memcpy(base, rgba, (size_t)w * h * 4), memcpy(px, rgba, (size_t)w * h * 4);
    free(l->base);
    *l = (Live){b, base, w, h, srgb, max_levels, signature()};
    paint_all(&targets[ti], px, w, h, buttonicons_style());
    bool ok = vk_tex_replace(b, px, w, h, srgb, max_levels);
    free(px);
    LOG("[buttons] texture %d (%ux%u): the button pictures as %s\n", ti, w, h,
        buttonicons_style() == 0 ? "Xbox" : buttonicons_style() == 1 ? "PlayStation" : buttonicons_style() == 2 ? "keys" : "Switch");
    return ok;
}

void buttonicons_forget(TexBackend *b) {
    for (unsigned i = 0; i < sizeof live / sizeof *live; i++)
        if (live[i].b == b) { free(live[i].base); live[i] = (Live){0}; }
}

void buttonicons_frame(void) {
    static uint64_t frames;
    if (++frames % 15) return;  // (four times a second at 60)
    uint64_t sig = 0;
    for (unsigned i = 0; i < sizeof live / sizeof *live; i++) {
        Live *l = &live[i];
        if (!l->b || !l->base) continue;
        if (!sig) sig = signature();
        if (sig == l->sig) continue;
        uint8_t *px = malloc((size_t)l->w * l->h * 4);
        if (!px) continue;
        memcpy(px, l->base, (size_t)l->w * l->h * 4);
        paint_all(&targets[i], px, l->w, l->h, buttonicons_style());
        if (vk_tex_replace(l->b, px, l->w, l->h, l->srgb, l->levels)) l->sig = sig;
        free(px);
        LOG("[buttons] the button pictures painted again (setting or binding changed)\n");
    }
}

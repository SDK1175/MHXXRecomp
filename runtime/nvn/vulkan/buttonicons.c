// The game's button prompts as an Xbox or a PlayStation controller's ([Controls] GameButtons): the pictures of the
// Switch's buttons in two of the game's textures are painted over, as each is uploaded, with the port's own
// (tools/buttonicons.py draws them: buttonicons_png.h). The text's prompts come from the button font
// (button_festa_00_ID, 24-pixel cells), the HUD's from its sheet (HD_hud00, 32-pixel cells). Found by the textures'
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
    uint16_t w, h, cell;
    int n;
    Cell cells[G_N];
} Target;
static const Target targets[] = {
    {0x473308d6d9f3b584ull, 256, 256, 24, 10,  // GUI/02_common/font/button_festa_00_ID
     {{G_Y, 0, 0}, {G_X, 24, 0}, {G_B, 48, 0}, {G_A, 72, 0}, {G_L, 96, 0}, {G_R, 0, 24}, {G_ZL, 120, 24},
      {G_ZR, 144, 24}, {G_PLUS, 0, 72}, {G_MINUS, 24, 72}}},
    {0xd58014a25649b59dull, 512, 512, 32, 6,  // GUI/99_texture/HD_hud00_BM_NOMIP (quest.arc)
     {{G_L, 0, 408}, {G_Y, 32, 408}, {G_X, 64, 408}, {G_B, 96, 408}, {G_A, 128, 408}, {G_R, 160, 408}}},
};

// The sheet's row: 0 Xbox, 1 PlayStation; -1 the Switch's own pictures
int buttonicons_style(void) {
    int v = g_config.game_buttons, layout = host_pad_layout();
    if (v == GAME_BUTTONS_AUTO) v = layout == 0 ? GAME_BUTTONS_XBOX : layout == 2 ? GAME_BUTTONS_PS : GAME_BUTTONS_SWITCH;
    return v == GAME_BUTTONS_XBOX ? 0 : v == GAME_BUTTONS_PS ? 1 : -1;
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
static void paint(uint8_t *px, uint32_t pw, uint32_t ph, const SDL_Surface *s, int sx, int sy, int n, float dx, float dy, float k) {
    int x0 = (int)(dx * k), y0 = (int)(dy * k), m = (int)(n * k + .5f);
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
    uint64_t sig;  // what it was painted for (0: not yet)
} Live;
static Live live[sizeof targets / sizeof *targets];

static uint64_t signature(void) {
    int style = buttonicons_style();
    uint64_t sig = (uint64_t)(style + 2);
    for (int g = 0; g < G_N; g++) sig = sig * 31 + (uint64_t)(source_of(g, style) + 2);
    return sig;
}

static void paint_all(const Target *t, uint8_t *rgba, uint32_t w, uint32_t h, int style) {
    const SDL_Surface *s = sheet();
    if (!s || style < 0) return;
    float k = (float)w / t->w;
    int row_y = t->cell == 24 ? style * 24 : 2 * 24 + style * 32;
    for (int i = 0; i < t->n; i++) {
        const Cell *c = &t->cells[i];
        int col = source_of(c->g, style);
        if (col >= 0) paint(rgba, w, h, s, col * t->cell, row_y, t->cell, c->x, c->y, k);
    }
}

bool buttonicons_apply(TexBackend *b, uint64_t key, const uint8_t *rgba, uint32_t w, uint32_t h, bool srgb) {
    int ti = -1;
    for (unsigned i = 0; i < sizeof targets / sizeof *targets; i++)
        if (targets[i].key == key) ti = (int)i;
    if (ti < 0) {  // (a texture of the same size: logged, to find the button textures of other game files)
        static int n;
        if (n++ < 40) LOG("[buttons] %016llx (%ux%u): not a texture with button pictures\n", (unsigned long long)key, w, h);
        return false;
    }
    Live *l = &live[ti];
    uint8_t *base = malloc((size_t)w * h * 4), *px = malloc((size_t)w * h * 4);
    if (!base || !px) { free(base), free(px); return false; }
    memcpy(base, rgba, (size_t)w * h * 4), memcpy(px, rgba, (size_t)w * h * 4);
    free(l->base);
    *l = (Live){b, base, w, h, srgb, signature()};
    paint_all(&targets[ti], px, w, h, buttonicons_style());
    bool ok = vk_tex_replace(b, px, w, h, srgb);
    free(px);
    LOG("[buttons] %016llx (%ux%u): the button pictures as %s\n", (unsigned long long)key, w, h,
        buttonicons_style() == 0 ? "Xbox" : buttonicons_style() == 1 ? "PlayStation" : "Switch");
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
        if (vk_tex_replace(l->b, px, l->w, l->h, l->srgb)) l->sig = sig;
        free(px);
        LOG("[buttons] the button pictures painted again (setting or binding changed)\n");
    }
}

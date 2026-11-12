// While the game runs: a newer release on GitHub shown as a small note at the bottom left ([Application]
// UpdateNotice, F1). The same question the launcher asks at its start (GitHub's latest release, not a draft or a
// pre-release, a version above this one): first a minute after the start, then every 30 minutes; each new version
// noted once. Nothing is downloaded or installed here (the launcher does that). Not for a development build.
#include "updatecheck.h"
#ifdef __ANDROID__  // (the Android build has no HTTPS of its own: no note)
void update_check_start(void) {}
bool update_notice(uint64_t now, char *version, size_t size, float *age) { (void)now, (void)version, (void)size, (void)age; return false; }
#else
#include "config.h"
#include "rt.h"
#include "../updater/updater.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#define FIRST_MS (60 * 1000)
#define EVERY_MS (30 * 60 * 1000)
#define SHOW_NS (20ull * 1000000000ull)  // (the note's time on screen)

extern const char *g_port_version;  // (main.c)
static SDL_Mutex *g_lock;
static char g_latest[64];
static uint64_t g_since;  // when it was noted (0: nothing to show)

static void version(const char *v, int out[4]) {  // "0.10.1" -> 0, 10, 1, 0 (the rest after a non-digit ignored)
    memset(out, 0, 4 * sizeof *out);
    for (int i = 0; i < 4 && *v >= '0' && *v <= '9'; i++) {
        char *e;
        out[i] = (int)strtol(v, &e, 10), v = e;
        if (*v != '.') break;
        v++;
    }
}
static bool newer(const char *a, const char *b) {
    int x[4], y[4];
    version(a, x), version(b, y);
    for (int i = 0; i < 4; i++)
        if (x[i] != y[i]) return x[i] > y[i];
    return false;
}

typedef struct { char *data; size_t size, cap; } Body;
static bool to_body(void *ctx, const void *data, size_t size) {
    Body *b = ctx;
    if (b->size + size + 1 > 1u << 20) return false;
    if (b->size + size + 1 > b->cap) {
        size_t cap = (b->size + size + 1) * 2;
        char *d = realloc(b->data, cap);
        if (!d) return false;
        b->data = d, b->cap = cap;
    }
    memcpy(b->data + b->size, data, size), b->size += size, b->data[b->size] = 0;
    return true;
}

static void check(void) {
    const char *url = getenv("MHXX_UPDATE_API");
    Body b = {0};
    HttpResult r;
    bool ok = http_get(url && *url ? url : "https://api.github.com/repos/SDK1175/MHXXRecomp/releases/latest",
                       "application/vnd.github+json", NULL, 5, to_body, &b, &r);
    Release *rel = ok && r.status == 200 ? calloc(1, sizeof *rel) : NULL;
    if (rel && json_release(b.data, b.size, rel) && !rel->draft && !rel->prerelease && newer(rel->tag, g_port_version)) {
        SDL_LockMutex(g_lock);
        if (strcmp(g_latest, rel->tag)) {
            SDL_strlcpy(g_latest, rel->tag, sizeof g_latest);
            g_since = SDL_GetTicksNS();
            LOG("[update] a newer release on GitHub: %s (this one %s)\n", rel->tag, g_port_version);
        }
        SDL_UnlockMutex(g_lock);
    } else if (!ok || r.status != 200)
        LOG("[update] GitHub not asked or no answer (%s, %d)\n", ok ? "" : r.error, r.status);
    free(rel), free(b.data);
}

static int SDLCALL run(void *unused) {
    (void)unused;
    SDL_Delay(FIRST_MS);
    for (;;) {
        if (config_update_notice()) check();
        SDL_Delay(EVERY_MS);
    }
    return 0;
}

void update_check_start(void) {
    const char *v = g_port_version;
    if (!*v || strstr(v, "-g") || !strcmp(v, "unknown")) return;  // (a development build: no release to compare)
    g_lock = SDL_CreateMutex();
    SDL_Thread *t = g_lock ? SDL_CreateThread(run, "update check", NULL) : NULL;
    if (t) SDL_DetachThread(t);
}

bool update_notice(uint64_t now, char *version, size_t size, float *age) {
    if (!g_lock || !config_update_notice()) return false;
    SDL_LockMutex(g_lock);
    bool show = g_since && now - g_since < SHOW_NS;
    if (show) SDL_strlcpy(version, g_latest, size), *age = (now - g_since) / 1e9f;
    SDL_UnlockMutex(g_lock);
    return show;
}
#endif

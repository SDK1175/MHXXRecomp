// The launcher's update (update.h): what MHGU Recomp's updater (updater/updater.c, removed) did, in the launcher.
// - Only GitHub's "latest" release: never a draft, a pre-release or a version not newer than this one (version.txt,
//   written by tools/dist.py into the release; a build without it is a development one and is left alone).
// - The archive must match its asset's size and SHA-256 (the digest GitHub keeps for every upload).
// - Never written: data/, mods/, textures/, save/, cheats/, ... and the settings and logs (KEEP), only the files the
//   release carries besides those. Each file replaced is kept in update/backup/ until the next start, and put back if
//   the install stops halfway. The running launcher's own files too: Windows lets a running program's file be moved
//   (not deleted), so the new one takes its place and the next start removes the old.
#include "update.h"

#include <SDL3/SDL.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <sys/stat.h>
#endif

extern "C" {
#include "../../updater/updater.h"
#include "config.h"
}

#ifdef _WIN32
#define GAME_EXE "MHXX Recomp.exe"
#define GAME_LIB "mhgugame.dll"
#define LAUNCHER_EXE "mhxx-launcher.exe"
#define ASSET_PREFIX "Windows-"
// (what this port called its programs before: removed once the new ones are in place)
static const char *const OBSOLETE[] = {"mhgurecomp.exe", "mhgurecomp-updater.exe"};
#else
#define GAME_EXE "mhgurecomp"
#define GAME_LIB "mhgugame.so"
#define LAUNCHER_EXE "mhxx-launcher"
#define ASSET_PREFIX "Linux-"
static const char *const OBSOLETE[] = {"mhgurecomp-updater"};
#endif
// The GitHub repository whose releases are this port's (MHXX_UPDATE_REPO at build time to change it)
#ifndef MHXX_UPDATE_REPO
#define MHXX_UPDATE_REPO "SDK1175/MHXXRecomp"
#endif

// what an update never writes (first path component, any case): the player's
static const char *const KEEP[] = {"data", "mods", "engine_modules", "cheats", "bonus", "cache", "shots", "save",
                                   "update", "textures", "realesrgan", "mhgurecomp.ini", "launcher.ini", "log.txt",
                                   "install.log", "imgui.ini"};

static std::string root;  // the game folder (the launcher's), with the separator at its end
static SDL_IOStream *g_log;
static std::mutex g_log_lock;

extern "C" void ulog(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> g(g_log_lock);
    SDL_Log("%s", buf);
    if (g_log) SDL_WriteIO(g_log, buf, strlen(buf)), SDL_FlushIO(g_log);
}

const char *update_game_exe() { return GAME_EXE; }

// ---- versions: "0.10.3" (or v0.10.3) as numbers; clean: nothing after them
static bool version(const char *s, int v[4], bool *clean) {
    memset(v, 0, 4 * sizeof *v);
    if (*s == 'v' || *s == 'V') s++;
    int n = 0;
    while (n < 4 && *s >= '0' && *s <= '9') {
        v[n++] = (int)strtol(s, (char **)&s, 10);
        if (*s != '.') break;
        s++;
    }
    if (clean) *clean = n > 0 && !*s;
    return n > 0;
}
static int version_cmp(const int a[4], const int b[4]) {
    for (int i = 0; i < 4; i++)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

// ---- files
static std::string here(const std::string &rel) { return root + rel; }
static bool exists(const std::string &p) { return SDL_GetPathInfo(p.c_str(), nullptr); }
static SDL_EnumerationResult SDLCALL rm_entry(void *arg, const char *dir, const char *name) {
    std::string p = std::string(dir) + name;
    SDL_PathInfo info;
    if (SDL_GetPathInfo(p.c_str(), &info) && info.type == SDL_PATHTYPE_DIRECTORY) SDL_EnumerateDirectory(p.c_str(), rm_entry, arg);
    if (!SDL_RemovePath(p.c_str())) *(bool *)arg = false;
    return SDL_ENUM_CONTINUE;
}
static bool rm_tree(const std::string &p) {
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(p.c_str(), &info)) return true;
    bool ok = true;
    if (info.type == SDL_PATHTYPE_DIRECTORY) SDL_EnumerateDirectory(p.c_str(), rm_entry, &ok);
    return SDL_RemovePath(p.c_str()) && ok;
}
static bool make_parent(const std::string &p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos || SDL_CreateDirectory(p.substr(0, s).c_str());
}

static void set_error(Update *u, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_error(Update *u, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ulog("[update] failed: %s\n", buf);
    {
        std::lock_guard<std::mutex> g(u->lock);
        u->error = buf;
    }
    u->state = UPD_FAILED;
}
static void nothing(Update *u, const char *why) {
    ulog("[update] %s\n", why);
    u->state = UPD_NONE;
}

// ---- the check
static Release g_rel;  // (the latest release, from the check; read by the install)
static const Asset *g_asset;

static bool to_string(void *ctx, const void *data, size_t n) {
    std::string *s = (std::string *)ctx;
    if (s->size() + n > (4u << 20)) return false;
    s->append((const char *)data, n);
    return true;
}
static const Asset *pick_asset(const Release *r) {
    for (int i = 0; i < r->nassets; i++) {
        const Asset *a = &r->assets[i];
        size_t len = strlen(a->name);
        bool archive = (len > 4 && !strcmp(a->name + len - 4, ".zip")) || (len > 7 && !strcmp(a->name + len - 7, ".tar.gz"));
        if (!strncmp(a->name, ASSET_PREFIX, strlen(ASSET_PREFIX)) && archive) return a;
    }
    return nullptr;
}
static void check(Update *u, bool force) {
    const char *url = SDL_getenv("MHXX_UPDATE_API");
    std::string latest_url = url && *url ? url : "https://api.github.com/repos/" MHXX_UPDATE_REPO "/releases/latest";
    std::string body;
    HttpResult r;
    bool ok = http_get(latest_url.c_str(), "application/vnd.github+json", nullptr, 5, to_string, &body, &r);
    if (!ok) return nothing(u, r.error[0] ? r.error : "no answer");
    ulog("[update] %s: %d (rate limit: %ld left)\n", latest_url.c_str(), r.status, r.rate_remaining);
    if (r.status == 404) return nothing(u, "no release yet");
    if (r.status != 200) return nothing(u, "GitHub didn't answer with the release");
    if (!json_release(body.data(), body.size(), &g_rel)) return nothing(u, "can't read the release");
    if (g_rel.draft || g_rel.prerelease) return nothing(u, "the latest release is a draft or pre-release");
    int have[4], want[4];
    bool clean;
    if (!version(g_rel.tag, want, &clean) || !clean) return nothing(u, "the latest release's tag is not a version");
    version(u->version.empty() ? "0" : u->version.c_str(), have, nullptr);
    ulog("[update] this %s, latest release %s\n", u->version.empty() ? "(development)" : u->version.c_str(), g_rel.tag);
    if (version_cmp(want, have) <= 0 && !force) return nothing(u, "up to date");
    g_asset = pick_asset(&g_rel);
    if (!g_asset) return nothing(u, "no download for this system in the release");
    if (strncmp(g_asset->digest, "sha256:", 7) || strlen(g_asset->digest) != 7 + 64)
        return nothing(u, "the release's download has no SHA-256 digest: not offered");
    if (g_asset->size < (1 << 20) || g_asset->size > ((int64_t)2 << 30)) return nothing(u, "the release's download has an odd size");
    {
        std::lock_guard<std::mutex> g(u->lock);
        u->latest = g_rel.tag, u->title = g_rel.title, u->notes = g_rel.body;
    }
    u->total = g_asset->size;
    u->state = UPD_AVAILABLE;
}

// ---- the install
struct Download {
    SDL_IOStream *out;
    Sha256 sha;
    Update *u;
};
static bool to_file(void *ctx, const void *data, size_t n) {
    Download *d = (Download *)ctx;
    if (d->u->cancel) return false;
    if (SDL_WriteIO(d->out, data, n) != n) return false;
    sha256_update(&d->sha, data, n);
    d->u->got += (int64_t)n;
    return true;
}
struct Item {
    std::string rel;
    unsigned mode;
};
struct Extract {
    std::vector<Item> items;
    std::string top, why;
    bool bad = false;
};
static bool safe_rel(const char *rel) {
    if (!*rel || *rel == '/' || strchr(rel, ':') || strchr(rel, '\\') || strlen(rel) >= 500) return false;
    for (const char *p = rel; *p;) {
        size_t n = strcspn(p, "/");
        if (!n || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) return false;
        p += n + (p[n] == '/');
    }
    return true;
}
static bool kept(const char *rel) {
    size_t n = strcspn(rel, "/");
    for (const char *k : KEEP)
        if (strlen(k) == n && !SDL_strncasecmp(rel, k, n)) return true;
    return false;
}
static SDL_IOStream *on_entry(void *ctx, const char *name, bool dir, unsigned mode) {
    Extract *x = (Extract *)ctx;
    if (x->bad) return nullptr;
    size_t tn = strcspn(name, "/");
    if (!name[tn]) {  // (a file at the top: not the layout of a release)
        if (dir) return nullptr;
        x->why = std::string("unexpected archive layout (") + name + ")", x->bad = true;
        return nullptr;
    }
    if (x->top.empty()) x->top.assign(name, tn);
    else if (x->top.size() != tn || x->top.compare(0, tn, name, tn)) {
        x->why = std::string("unexpected archive layout (") + name + ")", x->bad = true;
        return nullptr;
    }
    std::string rel = name + tn + 1;
    while (!rel.empty() && rel.back() == '/') rel.pop_back();
    if (rel.empty()) return nullptr;
    if (!safe_rel(rel.c_str())) {
        x->why = std::string("unsafe path in the archive (") + name + ")", x->bad = true;
        return nullptr;
    }
    if (kept(rel.c_str())) return nullptr;
    std::string p = here("update/new/" + rel);
    if (dir) {
        SDL_CreateDirectory(p.c_str());
        return nullptr;
    }
    make_parent(p);
    SDL_IOStream *out = SDL_IOFromFile(p.c_str(), "wb");
    if (!out) {
        x->why = "can't write " + p + ": " + SDL_GetError(), x->bad = true;
        return nullptr;
    }
    x->items.push_back({rel, mode});
    return out;
}
// The extracted files put in place, each one replaced kept in update/backup/; on a failure, all put back
static bool install_files(const Extract &x, std::string &why) {
    std::vector<char> moved(x.items.size()), placed(x.items.size());
    bool ok = true;
    for (size_t i = 0; ok && i < x.items.size(); i++) {
        const std::string &rel = x.items[i].rel;
        std::string src = here("update/new/" + rel), dst = here(rel), bak = here("update/backup/" + rel);
#ifndef _WIN32
        const char *base = strrchr(rel.c_str(), '/') ? strrchr(rel.c_str(), '/') + 1 : rel.c_str();
        bool program = !strcmp(base, GAME_EXE) || !strcmp(base, LAUNCHER_EXE) || !strcmp(base, "mhxx-texextract") ||
                       strstr(base, ".so");
        unsigned m = x.items[i].mode & 0777;
        if (!m || program) m = program ? 0755 : 0644;
        chmod(src.c_str(), m);
#endif
        if (exists(dst)) {
            if (!make_parent(bak) || !SDL_RenamePath(dst.c_str(), bak.c_str())) {
                why = "can't move " + dst + " aside: " + SDL_GetError();
                ok = false;
                break;
            }
            moved[i] = 1;
        }
        if (!make_parent(dst) || !SDL_RenamePath(src.c_str(), dst.c_str())) {
            why = "can't write " + dst + ": " + SDL_GetError();
            ok = false;
            break;
        }
        placed[i] = 1;
    }
    if (!ok) {
        ulog("[update] putting the old files back\n");
        for (size_t i = x.items.size(); i-- > 0;) {
            std::string dst = here(x.items[i].rel), bak = here("update/backup/" + x.items[i].rel);
            if (placed[i]) SDL_RemovePath(dst.c_str());
            if (moved[i] && !SDL_RenamePath(bak.c_str(), dst.c_str())) ulog("[update] can't put back %s: %s\n", dst.c_str(), SDL_GetError());
        }
    }
    return ok;
}
// Is the game running from this folder? (Its files can't be replaced under it.)
static bool game_running_here() {
#ifdef _WIN32
    if (void *m = SDL_LoadObject("kernel32.dll")) {  // (the game holds this while it runs: runtime/ui.c)
        typedef void *(__stdcall * Open)(unsigned long, int, const wchar_t *);
        typedef int(__stdcall * Close)(void *);
        Open open = (Open)SDL_LoadFunction((SDL_SharedObject *)m, "OpenMutexW");
        Close close = (Close)SDL_LoadFunction((SDL_SharedObject *)m, "CloseHandle");
        void *h = open ? open(0x00100000L /* SYNCHRONIZE */, 0, L"Local\\MHXXRecompRunning") : nullptr;
        if (h && close) close(h);
        SDL_UnloadObject((SDL_SharedObject *)m);
        return h != nullptr;
    }
#endif
    return false;
}
static void install(Update *u) {
    u->state = UPD_DOWNLOADING, u->got = 0;
    const Asset *a = g_asset;
    std::string part = here(std::string("update/") + a->name + ".part");
    SDL_CreateDirectory(here("update").c_str());
    Download d = {SDL_IOFromFile(part.c_str(), "wb"), {}, u};
    if (!d.out) return set_error(u, "can't write %s: %s", part.c_str(), SDL_GetError());
    sha256_init(&d.sha);
    ulog("[update] downloading %s (%lld bytes)\n", a->url, (long long)a->size);
    HttpResult r;
    bool ok = http_get(a->url, "application/octet-stream", nullptr, 15, to_file, &d, &r);
    if (!SDL_CloseIO(d.out)) ok = false;
    if (u->cancel) {
        SDL_RemovePath(part.c_str());
        u->cancel = false;
        u->state = UPD_AVAILABLE;  // (cancelled: asked again)
        ulog("[update] download cancelled\n");
        return;
    }
    if (!ok || r.status != 200) {
        SDL_RemovePath(part.c_str());
        if (r.status && r.status != 200) return set_error(u, "download: GitHub answered %d", r.status);
        return set_error(u, "download: %s", r.error[0] ? r.error : "no answer");
    }
    char hex[65];
    sha256_hex(&d.sha, hex);
    if (u->got != a->size || strcmp(hex, a->digest + 7)) {
        SDL_RemovePath(part.c_str());
        return set_error(u, "the download is damaged (%lld of %lld bytes, SHA-256 %.12s..., expected %.12s...)",
                         (long long)u->got.load(), (long long)a->size, hex, a->digest + 7);
    }
    ulog("[update] download checked: %lld bytes, sha256 %s\n", (long long)u->got.load(), hex);

    u->state = UPD_INSTALLING;
    if (game_running_here()) {
        SDL_RemovePath(part.c_str());
        return set_error(u, "the game is running: close it, then update");
    }
    std::string newdir = here("update/new");
    rm_tree(newdir);
    SDL_CreateDirectory(newdir.c_str());
    Extract x;
    char why[300] = "";
    ok = archive_extract(part.c_str(), on_entry, &x, why, sizeof why);
    std::string reason = why;
    if (x.bad) ok = false, reason = x.why;
    bool has_exe = false, has_lib = false, has_launcher = false;
    for (const Item &it : x.items)
        has_exe |= it.rel == GAME_EXE, has_lib |= it.rel == GAME_LIB, has_launcher |= it.rel == LAUNCHER_EXE;
    if (ok && !(has_exe && has_lib && has_launcher)) ok = false, reason = "the release has no " GAME_EXE ", " GAME_LIB " or " LAUNCHER_EXE;
    if (ok) {
        ulog("[update] installing %d files\n", (int)x.items.size());
        ok = install_files(x, reason);
    }
    rm_tree(newdir);
    SDL_RemovePath(part.c_str());
    if (!ok) return set_error(u, "%s", reason.c_str());
    for (const char *o : OBSOLETE)  // (the programs' old names: the new ones are in place)
        if (exists(here(o)) && !SDL_RemovePath(here(o).c_str())) {
            std::string bak = here(std::string("update/backup/") + o);
            make_parent(bak), SDL_RenamePath(here(o).c_str(), bak.c_str());
        }
    ulog("[update] updated to %s\n", g_rel.tag);
    u->state = UPD_DONE;
}

// ---- the launcher's
void update_begin(Update *u, int setting) {
    const char *base = SDL_GetBasePath();
    root = base ? base : "";
    SDL_CreateDirectory(here("update").c_str());
    g_log = SDL_IOFromFile(here("update/update.log").c_str(), "w");
    rm_tree(here("update/backup"));  // (the last update's: the old launcher's files could only go now)
    rm_tree(here("update/new"));
    if (exists(here(GAME_EXE)))
        for (const char *o : OBSOLETE)
            if (exists(here(o)) && SDL_RemovePath(here(o).c_str())) ulog("[update] removed the old %s\n", o);
    size_t n = 0;
    if (char *v = (char *)SDL_LoadFile(here("version.txt").c_str(), &n)) {
        std::string s(v, n);
        SDL_free(v);
        while (!s.empty() && strchr(" \r\n\t", s.back())) s.pop_back();
        int parts[4];
        bool clean;
        if (version(s.c_str(), parts, &clean) && clean) u->version = s;
    }
    bool force = SDL_getenv("MHXX_UPDATE_FORCE") != nullptr;
    ulog("[update] MHXX Recomp %s in %s\n", u->version.empty() ? "(development build)" : u->version.c_str(), root.c_str());
    if (setting == UPDATES_OFF && !force) return ulog("[update] off ([Application] Updates)\n");
    if (u->version.empty() && !force) return ulog("[update] a development build (no version.txt): not checked\n");
    u->state = UPD_CHECKING;
    std::thread([u, force]() { check(u, force); }).detach();
}
void update_install(Update *u) {
    if (u->state != UPD_AVAILABLE) return;
    u->state = UPD_DOWNLOADING;
    std::thread([u]() { install(u); }).detach();
}
bool update_restart() {
    std::string exe = here(LAUNCHER_EXE);
    const char *args[] = {exe.c_str(), nullptr};
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, root.c_str());
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    SDL_Process *p = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    ulog("[update] starting the new launcher: %s\n", p ? "ok" : SDL_GetError());
    if (g_log) SDL_CloseIO(g_log), g_log = nullptr;
    if (p) SDL_DestroyProcess(p);
    return p != nullptr;
}

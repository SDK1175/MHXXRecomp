// High-resolution textures made on the player's computer (texmake.h): what tools/texpack/upscale.py did, in the
// launcher, without Python.
#include "texmake.h"

#include <SDL3/SDL.h>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

extern "C" {
#include "../../updater/updater.h"  // (http_get, archive_extract: the updater's HTTPS and .zip)
}

namespace {

const char *TOOL_URL =
    "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip";
const char *MODEL = "realesrgan-x4plus-anime";
const char *EXTRACTED = "textures/extracted", *PACK = "textures/load", *WORK = "textures/_work", *TOOL = "realesrgan";
const size_t CHUNK = 200;  // textures per realesrgan run (the progress moves, and a stop loses at most this many)
#ifdef _WIN32
const char *EXE = ".exe";
#else
const char *EXE = "";
#endif

void fail(TexJob *job, const std::string &why) {
    SDL_Log("[texmake] %s", why.c_str());
    std::lock_guard<std::mutex> g(job->lock);
    job->error = why;
}
bool exists(const std::string &p) { return SDL_GetPathInfo(p.c_str(), nullptr); }
std::string absolute(const std::string &rel) {
    char *cwd = SDL_GetCurrentDirectory();
    std::string a = std::string(cwd ? cwd : "") + rel;
    SDL_free(cwd);
    return a;
}
SDL_EnumerationResult SDLCALL list_cb(void *u, const char *, const char *name) {
    ((std::vector<std::string> *)u)->push_back(name);
    return SDL_ENUM_CONTINUE;
}
std::vector<std::string> list(const std::string &dir) {
    std::vector<std::string> v;
    SDL_EnumerateDirectory(dir.c_str(), list_cb, &v);
    return v;
}
bool is_dir(const std::string &p) {
    SDL_PathInfo i;
    return SDL_GetPathInfo(p.c_str(), &i) && i.type == SDL_PATHTYPE_DIRECTORY;
}
void remove_tree(const std::string &p) {
    if (is_dir(p))
        for (const std::string &n : list(p)) remove_tree(p + "/" + n);
    SDL_RemovePath(p.c_str());
}

// ---------------------------------------------------------------- a program, waited for (stopped on cancel)
bool run(TexJob *job, const std::vector<std::string> &args, const std::string &cwd, const std::function<void()> &tick) {
    std::vector<const char *> a;
    for (const std::string &s : args) a.push_back(s.c_str());
    a.push_back(nullptr);
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)a.data());
    if (!cwd.empty()) SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, cwd.c_str());
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);  // (no console window)
    SDL_Process *p = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!p) return fail(job, "can't start " + args[0] + ": " + SDL_GetError()), false;
    for (;;) {
        if (SDL_WaitProcess(p, false, nullptr)) break;
        if (job->cancel) {
            SDL_KillProcess(p, true);
            SDL_WaitProcess(p, true, nullptr);
            SDL_DestroyProcess(p);
            return fail(job, "cancelled"), false;
        }
        if (tick) tick();
        SDL_Delay(250);
    }
    SDL_DestroyProcess(p);
    return true;
}

// ---------------------------------------------------------------- pictures
struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px;  // RGBA
};
bool load_png(const std::string &path, Image &im) {
    SDL_Surface *s = SDL_LoadPNG(path.c_str());
    if (!s) return false;
    SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(s);
    if (!c) return false;
    im.w = c->w, im.h = c->h;
    im.px.resize((size_t)im.w * im.h * 4);
    for (int y = 0; y < im.h; y++) memcpy(im.px.data() + (size_t)y * im.w * 4, (uint8_t *)c->pixels + (size_t)y * c->pitch, (size_t)im.w * 4);
    SDL_DestroySurface(c);
    return true;
}
// a PNG of RGB (channels 3: the model gets no alpha) or RGBA
bool save_png(const std::string &path, const uint8_t *rgba, int w, int h, int channels) {
    std::vector<uint8_t> raw((size_t)(w * channels + 1) * h);
    for (int y = 0; y < h; y++) {
        uint8_t *o = raw.data() + (size_t)(w * channels + 1) * y;
        *o++ = 0;
        for (int x = 0; x < w; x++)
            for (int c = 0; c < channels; c++) *o++ = rgba[((size_t)y * w + x) * 4 + c];
    }
    uLongf zn = compressBound((uLong)raw.size());
    std::vector<uint8_t> z(zn);
    if (compress2(z.data(), &zn, raw.data(), (uLong)raw.size(), 6) != Z_OK) return false;
    SDL_IOStream *f = SDL_IOFromFile(path.c_str(), "wb");
    if (!f) return false;
    auto be = [](uint8_t *p, uint32_t v) { p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v; };
    auto chunk = [&](const char *type, const uint8_t *d, uint32_t n) {
        uint8_t h8[8];
        be(h8, n), memcpy(h8 + 4, type, 4);
        SDL_WriteIO(f, h8, 8);
        if (n) SDL_WriteIO(f, d, n);
        uint32_t crc = (uint32_t)crc32(0, (const Bytef *)type, 4);
        if (n) crc = (uint32_t)crc32(crc, d, n);
        uint8_t c4[4];
        be(c4, crc);
        SDL_WriteIO(f, c4, 4);
    };
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    SDL_WriteIO(f, sig, 8);
    uint8_t ihdr[13];
    be(ihdr, w), be(ihdr + 4, h);
    ihdr[8] = 8, ihdr[9] = channels == 4 ? 6 : 2, ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk("IHDR", ihdr, 13);
    chunk("IDAT", z.data(), (uint32_t)zn);
    chunk("IEND", nullptr, 0);
    return SDL_CloseIO(f);
}
// Lanczos (a = 3), separable: channel c of 4 (or all four when c < 0) of src at dw x dh
void lanczos(const Image &src, int dw, int dh, Image &out, int only = -1) {
    auto kernel = [](double x) {
        x = std::fabs(x);
        if (x < 1e-9) return 1.0;
        if (x >= 3) return 0.0;
        double px = 3.14159265358979323846 * x;
        return 3 * std::sin(px) * std::sin(px / 3) / (px * px);
    };
    struct Tap { std::vector<int> i; std::vector<float> w; };
    auto taps = [&](int sn, int dn) {
        std::vector<Tap> t(dn);
        double scale = (double)sn / dn, support = 3 * std::max(1.0, scale), k = std::max(1.0, scale);
        for (int o = 0; o < dn; o++) {
            double c = (o + .5) * scale - .5;
            double sum = 0;
            for (int i = (int)std::floor(c - support) + 1; i <= (int)std::floor(c + support); i++) {
                double wt = kernel((i - c) / k);
                if (wt == 0) continue;
                t[o].i.push_back(std::clamp(i, 0, sn - 1)), t[o].w.push_back((float)wt), sum += wt;
            }
            for (float &wt : t[o].w) wt = (float)(wt / sum);
        }
        return t;
    };
    std::vector<Tap> tx = taps(src.w, dw), ty = taps(src.h, dh);
    std::vector<float> mid((size_t)dw * src.h * 4);
    for (int y = 0; y < src.h; y++)
        for (int x = 0; x < dw; x++)
            for (int c = 0; c < 4; c++) {
                if (only >= 0 && c != only) continue;
                float s = 0;
                for (size_t k = 0; k < tx[x].i.size(); k++) s += tx[x].w[k] * src.px[((size_t)y * src.w + tx[x].i[k]) * 4 + c];
                mid[((size_t)y * dw + x) * 4 + c] = s;
            }
    out.w = dw, out.h = dh;
    out.px.assign((size_t)dw * dh * 4, 255);
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++)
            for (int c = 0; c < 4; c++) {
                if (only >= 0 && c != only) continue;
                float s = 0;
                for (size_t k = 0; k < ty[y].i.size(); k++) s += ty[y].w[k] * mid[((size_t)ty[y].i[k] * dw + x) * 4 + c];
                out.px[((size_t)y * dw + x) * 4 + c] = (uint8_t)std::clamp((int)std::lround(s), 0, 255);
            }
}

// ---------------------------------------------------------------- the steps
struct Tex {
    std::string key;
    int w, h;
    bool color;
    int scale;
    const char *sub;  // its folder in the pack: gear or monsters ([Graphics] TexturePack picks by them)
    std::string out() const { return std::string(PACK) + "/" + sub + "/" + key + ".png"; }
};
// index.txt of mhxx-texextract: "<key> <w>x<h> <kind> <arc> <name>"; the textures of the groups asked for
std::vector<Tex> todo(TexJob *job) {
    std::vector<Tex> v;
    size_t n = 0;
    char *d = (char *)SDL_LoadFile((std::string(EXTRACTED) + "/index.txt").c_str(), &n);
    if (!d) return v;
    std::string s(d, n);
    SDL_free(d);
    size_t pos = 0;
    while (pos < s.size()) {
        size_t e = s.find('\n', pos);
        if (e == std::string::npos) e = s.size();
        std::string line = s.substr(pos, e - pos);
        pos = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> f;
        size_t a = 0;
        for (int i = 0; i < 4 && a != std::string::npos; i++) {
            size_t b = line.find(' ', a);
            f.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
            a = b == std::string::npos ? b : b + 1;
        }
        if (f.size() < 4 || a == std::string::npos) continue;
        std::string name = line.substr(a), group = name.substr(0, name.find_first_of("\\/"));
        for (char &c : group) c = (char)tolower((unsigned char)c);
        int w = 0, h = 0;
        if (sscanf(f[1].c_str(), "%dx%d", &w, &h) != 2) continue;
        int scale = 0, max_side = 0;
        if (job->gear && (group == "player" || group == "weapon" || group == "otomo")) scale = 4, max_side = 256;
        if (job->monsters && group == "enemy") scale = 2, max_side = 1024;
        if (!scale || std::max(w, h) > max_side) continue;
        Tex t = {f[0], w, h, f[2] == "color", scale, scale == 4 ? "gear" : "monsters"};
        if (exists(t.out())) continue;  // (made already)
        std::string old = std::string(PACK) + "/" + f[0] + ".png";  // (a pack from before the folders: put in its own)
        if (exists(old) && SDL_RenamePath(old.c_str(), t.out().c_str())) continue;
        v.push_back(t);
    }
    return v;
}
// the folders of the mods' romfs (the Korean patch's textures are its own)
void mod_romfs(const std::string &dir, int depth, std::vector<std::string> &out) {
    if (depth > 5) return;
    for (const std::string &n : list(dir)) {
        std::string p = dir + "/" + n;
        if (!is_dir(p)) continue;
        if (SDL_strcasecmp(n.c_str(), "romfs") == 0 && is_dir(p + "/nativeNX/arc")) out.push_back(p);
        else mod_romfs(p, depth + 1, out);
    }
}
bool extract(TexJob *job) {
    std::string done_mark = std::string(EXTRACTED) + "/.complete";
    if (exists(done_mark)) return true;  // (taken out before: the game's files don't change)
    if (!is_dir(job->game_dir + "/romfs/nativeNX/arc")) return fail(job, "no game files (data/romfs)"), false;
    std::string exe = absolute(std::string("mhxx-texextract") + EXE);
    if (!exists(exe)) return fail(job, "mhxx-texextract is missing beside the launcher"), false;
    remove_tree(EXTRACTED);
    SDL_CreateDirectory(EXTRACTED);
    std::vector<std::string> args = {exe, absolute(job->game_dir + "/romfs"), absolute(EXTRACTED)};
    std::vector<std::string> mods;
    mod_romfs(job->mods_dir, 0, mods);
    for (const std::string &m : mods) args.push_back(absolute(m));
    job->total = 0, job->done = 0, job->started = SDL_GetTicks();
    std::string index = std::string(EXTRACTED) + "/index.txt";
    auto tick = [&]() {  // (the textures saved so far: the lines of its index)
        size_t n = 0;
        if (char *d = (char *)SDL_LoadFile(index.c_str(), &n)) {
            job->done = (uint64_t)std::count(d, d + n, '\n');
            SDL_free(d);
        }
    };
    if (!run(job, args, "", tick)) return false;
    tick();
    if (job->done == 0) return fail(job, "mhxx-texextract took out no textures"), false;
    SDL_IOStream *m = SDL_IOFromFile(done_mark.c_str(), "w");
    if (m) SDL_CloseIO(m);
    return true;
}
SDL_IOStream *SDLCALL unpack_entry(void *ctx, const char *name, bool dir, unsigned) {
    std::string p = std::string((const char *)ctx) + "/" + name;
    if (strstr(name, "..")) return nullptr;
    if (dir) {
        SDL_CreateDirectory(p.c_str());
        return nullptr;
    }
    size_t s = p.rfind('/');
    if (s != std::string::npos) SDL_CreateDirectory(p.substr(0, s).c_str());
    return SDL_IOFromFile(p.c_str(), "wb");
}
struct Download { SDL_IOStream *f; TexJob *job; };
bool SDLCALL download_sink(void *ctx, const void *data, size_t size) {
    Download *d = (Download *)ctx;
    if (d->job->cancel) return false;
    d->job->done += size;
    return SDL_WriteIO(d->f, data, size) == size;
}
// realesrgan-ncnn-vulkan: in realesrgan/ (or a folder in it), downloaded the first time
std::string tool(TexJob *job) {
    std::string name = std::string("realesrgan-ncnn-vulkan") + EXE;
    auto find = [&]() -> std::string {
        if (exists(std::string(TOOL) + "/" + name)) return std::string(TOOL);
        for (const std::string &n : list(TOOL))
            if (exists(std::string(TOOL) + "/" + n + "/" + name)) return std::string(TOOL) + "/" + n;
        return "";
    };
    std::string dir = find();
    if (!dir.empty()) return dir;
    job->stage = TEX_DOWNLOAD, job->done = 0, job->total = 0, job->started = SDL_GetTicks();
    SDL_CreateDirectory(TOOL);
    std::string zip = std::string(TOOL) + "/realesrgan.zip";
    Download d = {SDL_IOFromFile(zip.c_str(), "wb"), job};
    if (!d.f) return fail(job, "can't write " + zip), "";
    HttpResult r = {};
    bool ok = http_get(TOOL_URL, "application/octet-stream", nullptr, 30, download_sink, &d, &r);
    SDL_CloseIO(d.f);
    if (!ok || r.status != 200) {
        SDL_RemovePath(zip.c_str());
        if (job->cancel) return fail(job, "cancelled"), "";
        return fail(job, std::string("can't download realesrgan-ncnn-vulkan (") + (r.status ? std::to_string(r.status) : r.error) + ")"), "";
    }
    char err[256] = "";
    bool un = archive_extract(zip.c_str(), unpack_entry, (void *)TOOL, err, sizeof err);
    SDL_RemovePath(zip.c_str());
    if (!un) return fail(job, std::string("can't unpack realesrgan-ncnn-vulkan: ") + err), "";
    dir = find();
    if (dir.empty()) return fail(job, "realesrgan-ncnn-vulkan not found after unpacking"), "";
#ifndef _WIN32
    SDL_Log("[texmake] (not Windows: make %s/%s executable if it isn't)", dir.c_str(), name.c_str());
#endif
    return dir;
}
// the normal maps and masks: resized smoothly
bool resize_others(TexJob *job, const std::vector<Tex> &v) {
    job->stage = TEX_RESIZE, job->done = 0, job->total = v.size(), job->started = SDL_GetTicks();
    for (const Tex &t : v) {
        if (job->cancel) return fail(job, "cancelled"), false;
        Image src, out;
        if (load_png(std::string(EXTRACTED) + "/" + t.key + ".png", src)) {
            lanczos(src, src.w * t.scale, src.h * t.scale, out);
            save_png(t.out(), out.px.data(), out.w, out.h, 4);
        }
        job->done++;
    }
    return true;
}
// the colour ones: by the model, in chunks; their alpha resized on its own and put back
bool upscale(TexJob *job, const std::vector<Tex> &v, const std::string &tooldir) {
    job->stage = TEX_UPSCALE, job->done = 0, job->total = v.size(), job->started = SDL_GetTicks();
    std::string win = std::string(WORK) + "/in", wout = std::string(WORK) + "/out";
    for (size_t c0 = 0; c0 < v.size(); c0 += CHUNK) {
        size_t c1 = std::min(v.size(), c0 + CHUNK);
        remove_tree(WORK);
        SDL_CreateDirectory(win.c_str()), SDL_CreateDirectory(wout.c_str());
        for (size_t i = c0; i < c1; i++) {
            Image src;
            if (load_png(std::string(EXTRACTED) + "/" + v[i].key + ".png", src))
                save_png(win + "/" + v[i].key + ".png", src.px.data(), src.w, src.h, 3);
        }
        auto tick = [&]() { job->done = c0 + list(wout).size(); };
        std::string exe = absolute(tooldir + "/realesrgan-ncnn-vulkan" + EXE);
        // (-t 128: small tiles, each a short piece of graphics card work. Its own choice by the card's memory let one
        // large monster texture run longer than Windows waits for the card (about 2 s) and the driver was reset -
        // a black screen, and the launcher left without its buttons)
        if (!run(job, {exe, "-i", absolute(win), "-o", absolute(wout), "-n", MODEL, "-s", "4", "-t", "128", "-f", "png"},
                 absolute(tooldir), tick))
            return false;
        size_t made = 0;
        for (size_t i = c0; i < c1; i++) {
            const Tex &t = v[i];
            Image src, up, out, alpha;
            if (!load_png(wout + "/" + t.key + ".png", up) || !load_png(std::string(EXTRACTED) + "/" + t.key + ".png", src))
                continue;
            int dw = t.w * t.scale, dh = t.h * t.scale;
            if (up.w != dw || up.h != dh) lanczos(up, dw, dh, out);
            else out = std::move(up);
            lanczos(src, dw, dh, alpha, 3);
            for (size_t p = 0; p < (size_t)dw * dh; p++) out.px[p * 4 + 3] = alpha.px[p * 4 + 3];
            if (save_png(t.out(), out.px.data(), dw, dh, 4)) made++;
        }
        if (!made && c1 > c0)  // (the graphics card can't run it: no Vulkan, or too little memory)
            return fail(job, "realesrgan-ncnn-vulkan made no pictures (does the graphics card support Vulkan?)"), false;
        job->done = c1;
    }
    remove_tree(WORK);
    return true;
}

// ---------------------------------------------------------------- the pack of another folder
std::string norm(std::string p) {
    for (char &c : p)
        if (c == '\\') c = '/';
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}
bool is_key_png(const std::string &n) {
    if (n.size() != 20 || SDL_strcasecmp(n.c_str() + 16, ".png")) return false;
    for (int i = 0; i < 16; i++)
        if (!isxdigit((unsigned char)n[i])) return false;
    return true;
}
// index.txt of mhxx-texextract: key -> its pack folder (gear / monsters), for a pack from before the folders
void read_groups(const std::string &index, std::map<std::string, const char *> &out) {
    size_t n = 0;
    char *d = (char *)SDL_LoadFile(index.c_str(), &n);
    if (!d) return;
    std::string s(d, n);
    SDL_free(d);
    size_t pos = 0;
    while (pos < s.size()) {
        size_t e = s.find('\n', pos);
        if (e == std::string::npos) e = s.size();
        std::string line = s.substr(pos, e - pos);
        pos = e + 1;
        size_t a = 0;
        for (int i = 0; i < 4 && a != std::string::npos; i++) a = line.find(' ', a), a = a == std::string::npos ? a : a + 1;
        if (a == std::string::npos || line.size() < 16) continue;
        std::string group = line.substr(a, line.find_first_of("\\/", a) - a);
        for (char &c : group) c = (char)tolower((unsigned char)c);
        if (group == "player" || group == "weapon" || group == "otomo") out[line.substr(0, 16)] = "gear";
        else if (group == "enemy") out[line.substr(0, 16)] = "monsters";
    }
}
// mhxx-texextract's index.txt? (its first line: "<16 hex> <w>x<h> <kind> ...")
bool is_texextract_index(const std::string &path) {
    SDL_IOStream *f = SDL_IOFromFile(path.c_str(), "rb");
    if (!f) return false;
    char b[48] = {};
    SDL_ReadIO(f, b, sizeof b - 1);
    SDL_CloseIO(f);
    for (int i = 0; i < 16; i++)
        if (!isxdigit((unsigned char)b[i])) return false;
    int w, h;
    return b[16] == ' ' && sscanf(b + 17, "%dx%d", &w, &h) == 2;
}
struct Copy {
    std::string from, to;
};
void collect(const std::string &dir, const std::string &rel, int depth, std::vector<Copy> &out, const std::string &dest,
             bool pngs_only) {
    if (depth > 4) return;
    for (const std::string &n : list(dir)) {
        std::string p = dir + "/" + n, r = rel.empty() ? n : rel + "/" + n;
        if (is_dir(p)) {
            if (n != "_work") collect(p, r, depth + 1, out, dest, pngs_only);
        } else if (!pngs_only || is_key_png(n))
            out.push_back({p, dest + "/" + r});
    }
}
bool import_pack(TexJob *job) {
    job->stage = TEX_IMPORT, job->done = 0, job->total = 0, job->started = SDL_GetTicks();
    std::string src = norm(job->import_from), pack;
    for (const std::string &c : {src + "/textures/load", src + "/load", src})  // (the game's folder, its textures, the pack)
        if (is_dir(c)) {
            std::vector<Copy> probe;
            collect(c, "", 0, probe, "", true);
            if (!probe.empty()) { pack = c; break; }
        }
    if (pack.empty()) return fail(job, "no texture pack in " + src), false;
    auto same = [](std::string a, std::string b) {
        a = norm(a), b = norm(b);
#ifdef _WIN32
        for (char &c : a) c = (char)tolower((unsigned char)c);
        for (char &c : b) c = (char)tolower((unsigned char)c);
#endif
        return a == b;
    };
    if (same(pack, absolute(PACK)) || same(pack, PACK)) return fail(job, "that is this folder's own pack"), false;
    std::string root = pack.size() > 14 && pack.compare(pack.size() - 14, 14, "/textures/load") == 0 ? pack.substr(0, pack.size() - 14) : "";
    std::map<std::string, const char *> groups;  // (pictures not in a folder yet: which one, from any index found)
    if (!root.empty()) read_groups(root + "/" + EXTRACTED + "/index.txt", groups);
    read_groups(std::string(EXTRACTED) + "/index.txt", groups);
    // (and mhxx-texextract's run by hand, as the manual's tools/texpack/upscale.py way has it: a folder with its
    // index.txt beside that game folder or up to two above it - ../textures_all2 next to the source folder)
    std::string up = root.empty() ? pack : root;
    for (int level = 0; level < 3 && !up.empty(); level++) {
        int seen = 0;
        for (const std::string &n : list(up)) {
            std::string ix = up + "/" + n + "/index.txt";
            if (++seen > 300) break;
            if (is_dir(up + "/" + n) && is_texextract_index(ix)) {
                size_t before = groups.size();
                read_groups(ix, groups);
                SDL_Log("[texmake] sorting by %s (%d)", ix.c_str(), (int)(groups.size() - before));
            }
        }
        size_t sl = up.rfind('/');
        up = sl == std::string::npos || sl == 0 ? "" : up.substr(0, sl);
        if (up.size() == 2 && up[1] == ':') up += "/";  // (a drive's root: F:/)
    }
    std::vector<Copy> files;
    collect(pack, "", 0, files, PACK, true);
    for (Copy &c : files) {
        std::string name = c.to.substr(c.to.rfind('/') + 1), rel = c.to.substr(strlen(PACK) + 1);
        auto g = groups.find(name.substr(0, 16));
        if (rel == name && g != groups.end()) c.to = std::string(PACK) + "/" + g->second + "/" + name;
    }
    // (the textures that one took out of the game files too, when this folder hasn't: the next Make skips that step)
    if (!root.empty() && exists(root + "/" + EXTRACTED + "/.complete") && !exists(std::string(EXTRACTED) + "/.complete")) {
        remove_tree(EXTRACTED);
        std::vector<Copy> ex;
        collect(root + "/" + EXTRACTED, "", 0, ex, EXTRACTED, false);
        std::stable_partition(ex.begin(), ex.end(), [](const Copy &c) { return c.to.find(".complete") == std::string::npos; });
        files.insert(files.end(), ex.begin(), ex.end());  // (.complete last: only when all of it is here)
    }
    job->total = files.size();
    SDL_Log("[texmake] import from %s: %d files", pack.c_str(), (int)files.size());
    for (const Copy &c : files) {
        if (job->cancel) return fail(job, "cancelled"), false;
        SDL_PathInfo a, b;
        bool have = SDL_GetPathInfo(c.to.c_str(), &b) && SDL_GetPathInfo(c.from.c_str(), &a) && a.size == b.size;
        if (!have) {
            size_t s = c.to.rfind('/');
            SDL_CreateDirectory(c.to.substr(0, s).c_str());
            if (!SDL_CopyFile(c.from.c_str(), c.to.c_str())) return fail(job, "can't copy " + c.from + ": " + SDL_GetError()), false;
        }
        job->done++;
    }
    return true;
}

}  // namespace

int texmake_pack_count() {
    int n = 0;
    for (const char *d : {"", "/gear", "/monsters"})
        for (const std::string &f : list(std::string(PACK) + d))
            if (f.size() > 4 && SDL_strcasecmp(f.c_str() + f.size() - 4, ".png") == 0) n++;
    return n;
}

void texmake_run(TexJob *job) {
    auto finish = [&](bool ok) {
        remove_tree(WORK);
        job->ok = ok;
        if (ok) job->stage = TEX_DONE;
        job->finished = true;
    };
    SDL_CreateDirectory((std::string(PACK) + "/gear").c_str());
    SDL_CreateDirectory((std::string(PACK) + "/monsters").c_str());
    if (!job->import_from.empty()) return finish(import_pack(job));
    job->stage = TEX_EXTRACT;
    if (!extract(job)) return finish(false);
    std::vector<Tex> v = todo(job), color, other;
    for (const Tex &t : v) (t.color ? color : other).push_back(t);
    SDL_Log("[texmake] %d to do: %d colour, %d normal maps / masks", (int)v.size(), (int)color.size(), (int)other.size());
    std::string dir;
    if (!color.empty() && (dir = tool(job)).empty()) return finish(false);
    if (!resize_others(job, other)) return finish(false);
    if (!color.empty() && !upscale(job, color, dir)) return finish(false);
    finish(true);
}

// The launcher's Tools (tools.h): the save's backups, a problem report, the PC's check and the shader caches.
#include "tools.h"

#include <SDL3/SDL.h>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/statvfs.h>
#include <sys/utsname.h>
#endif
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64)
#include <cpuid.h>
#define HAVE_CPUID 1
#endif
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "config.h"
extern "C" {
#include "../../updater/updater.h"  // (archive_extract)
}

#define BACKUPS "backups"
#define REPORTS "reports"
#define CACHE "cache"
#define AUTO_KEEP 10  // automatic backups kept (manual ones: all)

// ------------------------------------------------------------------ files
namespace {
SDL_EnumerationResult SDLCALL list_cb(void *u, const char *, const char *name) {
    ((std::vector<std::string> *)u)->push_back(name);
    return SDL_ENUM_CONTINUE;
}
std::vector<std::string> list(const std::string &dir) {
    std::vector<std::string> names;
    SDL_EnumerateDirectory(dir.c_str(), list_cb, &names);
    std::sort(names.begin(), names.end());
    return names;
}
bool is_dir(const std::string &p) {
    SDL_PathInfo i;
    return SDL_GetPathInfo(p.c_str(), &i) && i.type == SDL_PATHTYPE_DIRECTORY;
}
bool exists(const std::string &p) { return SDL_GetPathInfo(p.c_str(), nullptr); }
uint64_t tree_size(const std::string &p) {
    SDL_PathInfo i;
    if (!SDL_GetPathInfo(p.c_str(), &i)) return 0;
    if (i.type != SDL_PATHTYPE_DIRECTORY) return i.size;
    uint64_t n = 0;
    for (const std::string &c : list(p)) n += tree_size(p + "/" + c);
    return n;
}
bool remove_tree(const std::string &p) {
    SDL_PathInfo i;
    if (!SDL_GetPathInfo(p.c_str(), &i)) return true;
    if (i.type == SDL_PATHTYPE_DIRECTORY)
        for (const std::string &c : list(p)) remove_tree(p + "/" + c);
    return SDL_RemovePath(p.c_str());
}
// every file under dir: (its path, its path from dir with '/')
void walk(const std::string &dir, const std::string &rel, std::vector<std::pair<std::string, std::string>> &out, int depth = 0) {
    if (depth > 16) return;
    for (const std::string &c : list(dir)) {
        std::string p = dir + "/" + c, r = rel.empty() ? c : rel + "/" + c;
        if (is_dir(p)) walk(p, r, out, depth + 1);
        else out.push_back({p, r});
    }
}
std::string load(const std::string &path) {
    size_t n = 0;
    void *d = SDL_LoadFile(path.c_str(), &n);
    std::string s = d ? std::string((const char *)d, n) : std::string();
    SDL_free(d);
    return s;
}
SDL_DateTime now_local() {
    SDL_Time t = 0;
    SDL_GetCurrentTime(&t);
    SDL_DateTime dt = {};
    SDL_TimeToDateTime(t, &dt, true);
    return dt;
}
std::string stamp() {  // 20261109-183012
    SDL_DateTime d = now_local();
    char b[32];
    SDL_snprintf(b, sizeof b, "%04d%02d%02d-%02d%02d%02d", d.year, d.month, d.day, d.hour, d.minute, d.second);
    return b;
}

// ------------------------------------------------------------------ a .zip, written (deflated, UTF-8 names)
struct ZipOut {
    struct Ent {
        std::string name;
        uint32_t crc, cs, us, off;
        uint16_t method;
    };
    SDL_IOStream *o = nullptr;
    std::vector<Ent> es;
    uint16_t time = 0, date = 0;
    uint32_t at = 0;
    bool ok = true;
    explicit ZipOut(const std::string &path) {
        o = SDL_IOFromFile(path.c_str(), "wb");
        ok = o != nullptr;
        SDL_DateTime d = now_local();
        time = (uint16_t)(d.hour << 11 | d.minute << 5 | d.second / 2);
        date = (uint16_t)((d.year - 1980) << 9 | d.month << 5 | d.day);
    }
    void put(const void *p, size_t n) {
        if (ok && SDL_WriteIO(o, p, n) != n) ok = false;
        at += (uint32_t)n;
    }
    void u16(uint32_t v) {
        uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
        put(b, 2);
    }
    void u32(uint32_t v) {
        uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
        put(b, 4);
    }
    void add(const std::string &name, const std::string &data) {
        Ent e{name, (uint32_t)crc32(0, (const Bytef *)data.data(), (uInt)data.size()), 0, (uint32_t)data.size(), at, 8};
        std::string packed(compressBound((uLong)data.size()) + 64, '\0');
        z_stream z = {};
        bool deflated = deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK;
        if (deflated) {
            z.next_in = (Bytef *)data.data(), z.avail_in = (uInt)data.size();
            z.next_out = (Bytef *)&packed[0], z.avail_out = (uInt)packed.size();
            deflated = deflate(&z, Z_FINISH) == Z_STREAM_END;
            packed.resize(z.total_out);
            deflateEnd(&z);
        }
        if (!deflated || packed.size() >= data.size()) e.method = 0, packed = data;
        e.cs = (uint32_t)packed.size();
        u32(0x04034b50), u16(20), u16(0x800), u16(e.method), u16(time), u16(date);
        u32(e.crc), u32(e.cs), u32(e.us), u16((uint32_t)name.size()), u16(0);
        put(name.data(), name.size());
        put(packed.data(), packed.size());
        es.push_back(e);
    }
    bool finish() {
        uint32_t cd = at;
        for (const Ent &e : es) {
            u32(0x02014b50), u16(20), u16(20), u16(0x800), u16(e.method), u16(time), u16(date);
            u32(e.crc), u32(e.cs), u32(e.us), u16((uint32_t)e.name.size()), u16(0), u16(0), u16(0), u16(0), u32(0),
                u32(e.off);
            put(e.name.data(), e.name.size());
        }
        uint32_t cd_size = at - cd;
        u32(0x06054b50), u16(0), u16(0), u16((uint32_t)es.size()), u16((uint32_t)es.size()), u32(cd_size), u32(cd), u16(0);
        if (o && !SDL_CloseIO(o)) ok = false;
        o = nullptr;
        return ok;
    }
    ~ZipOut() {
        if (o) SDL_CloseIO(o);
    }
};

bool safe_rel(const std::string &p) {
    if (p.empty() || p[0] == '/' || p.find(':') != std::string::npos || p.find('\\') != std::string::npos) return false;
    for (size_t s = 0; s <= p.size();) {
        size_t e = p.find('/', s);
        if (e == std::string::npos) e = p.size();
        if (p.compare(s, e - s, "..") == 0 && e - s == 2) return false;
        s = e + 1;
    }
    return true;
}
void make_parents(const std::string &path) {
    size_t s = path.rfind('/');
    if (s != std::string::npos) SDL_CreateDirectory(path.substr(0, s).c_str());
}
bool starts(const std::string &s, const char *p) { return s.compare(0, strlen(p), p) == 0; }
}  // namespace

// ------------------------------------------------------------------ launcher.ini
std::string launcher_ini_get(const char *key, const char *fallback) {
    std::string d = load("launcher.ini"), k = std::string(key) + " =";
    for (size_t s = 0; s < d.size();) {
        size_t e = d.find('\n', s);
        if (e == std::string::npos) e = d.size();
        std::string line = d.substr(s, e - s);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (starts(line, k.c_str())) {
            std::string v = line.substr(k.size());
            while (!v.empty() && v[0] == ' ') v.erase(0, 1);
            return v;
        }
        s = e + 1;
    }
    return fallback;
}
void launcher_ini_set(const char *key, const char *value) {
    std::string d = load("launcher.ini"), k = std::string(key) + " =", out;
    bool done = false;
    for (size_t s = 0; s < d.size();) {
        size_t e = d.find('\n', s);
        if (e == std::string::npos) e = d.size();
        std::string line = d.substr(s, e - s);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (starts(line, k.c_str())) line = k + " " + value, done = true;
        if (!line.empty() || e < d.size()) out += line + "\n";
        s = e + 1;
    }
    if (out.empty()) out = "; mhxx-launcher\n";
    if (!done) out += k + " " + value + "\n";
    SDL_SaveFile("launcher.ini", out.data(), out.size());
}

// ------------------------------------------------------------------ the save
std::string save_dir() {
    const char *env = SDL_getenv("MHGU_SAVE_DIR");
    if (env && *env) return env;
    if (g_config.save_location[0]) return g_config.save_location;
    char *pref = SDL_GetPrefPath(nullptr, "MHXXRecomp");  // (%APPDATA%\MHXXRecomp\ , ~/.local/share/MHXXRecomp/)
    if (!pref) return "save";
    std::string d = std::string(pref) + "save";
    SDL_free(pref);
    return d;
}
namespace {
// the save's own folders in it (save_0 ...: SaveLocation may be a folder with other things in it, those are not ours)
std::vector<std::string> save_folders(const std::string &dir) {
    std::vector<std::string> out;
    for (const std::string &c : list(dir))
        if (starts(c, "save_") && is_dir(dir + "/" + c)) out.push_back(c);
    return out;
}
}  // namespace
bool save_exists() {
    std::string d = save_dir();
    for (const std::string &f : save_folders(d)) {
        std::vector<std::pair<std::string, std::string>> files;
        walk(d + "/" + f, f, files);
        if (!files.empty()) return true;
    }
    return false;
}
bool auto_backup_on() { return launcher_ini_get("AutoBackup", "1") != "0"; }

std::vector<Backup> backup_list() {
    std::vector<Backup> out;
    for (const std::string &n : list(BACKUPS)) {
        // save-YYYYMMDD-HHMMSS-kind.zip
        if (!starts(n, "save-") || n.size() < 25 || n.compare(n.size() - 4, 4, ".zip") != 0) continue;
        Backup b;
        b.file = n;
        b.when = n.substr(5, 4) + "-" + n.substr(9, 2) + "-" + n.substr(11, 2) + " " + n.substr(14, 2) + ":" + n.substr(16, 2);
        b.kind = n.substr(21, n.size() - 25);
        size_t dash = b.kind.find('-');  // (a second in the same second: -2)
        if (dash != std::string::npos) b.seq = SDL_atoi(b.kind.c_str() + dash + 1), b.kind.resize(dash);
        SDL_PathInfo i;
        b.size = SDL_GetPathInfo((std::string(BACKUPS "/") + n).c_str(), &i) ? (int64_t)i.size : 0;
        out.push_back(b);
    }
    std::sort(out.begin(), out.end(), [](const Backup &a, const Backup &b) {
        int c = a.file.compare(5, 15, b.file, 5, 15);
        return c != 0 ? c > 0 : a.seq > b.seq;
    });
    return out;
}

bool backup_make(const char *kind, std::string &made, std::string &err) {
    made.clear();
    std::string dir = save_dir();
    std::vector<std::pair<std::string, std::string>> files;
    for (const std::string &f : save_folders(dir)) walk(dir + "/" + f, f, files);
    if (files.empty()) return true;  // (no save yet)
    SDL_CreateDirectory(BACKUPS);
    std::string base = "save-" + stamp() + "-" + kind, name = base + ".zip";
    int seq = 0;  // (more in the same second: after the last one, even when older ones are gone)
    for (const Backup &b : backup_list())
        if (starts(b.file, base.c_str())) seq = std::max(seq, b.seq);
    if (seq) name = base + "-" + std::to_string(seq + 1) + ".zip";
    std::string tmp = std::string(BACKUPS "/") + name + ".part";
    {
        ZipOut z(tmp);
        if (!z.ok) return err = "can't write " + tmp + ": " + SDL_GetError(), false;
        for (auto &f : files) {
            SDL_PathInfo i;
            if (!SDL_GetPathInfo(f.first.c_str(), &i)) continue;
            size_t n = 0;
            void *d = SDL_LoadFile(f.first.c_str(), &n);
            if (!d) {
                z.finish();
                SDL_RemovePath(tmp.c_str());
                return err = "can't read " + f.first + ": " + SDL_GetError(), false;
            }
            z.add(f.second, std::string((const char *)d, n));
            SDL_free(d);
        }
        if (!z.finish()) {
            SDL_RemovePath(tmp.c_str());
            return err = "can't write " + tmp + " (disk full?)", false;
        }
    }
    if (!SDL_RenamePath(tmp.c_str(), (std::string(BACKUPS "/") + name).c_str())) {
        SDL_RemovePath(tmp.c_str());
        return err = std::string("can't name the backup: ") + SDL_GetError(), false;
    }
    made = name;
    int autos = 0;  // (the automatic ones beyond the newest AUTO_KEEP: gone)
    for (const Backup &b : backup_list())
        if (b.kind != "manual" && ++autos > AUTO_KEEP) backup_delete(b.file);
    return true;
}

bool backup_delete(const std::string &file) {
    if (file.find('/') != std::string::npos || file.find('\\') != std::string::npos) return false;
    return SDL_RemovePath((std::string(BACKUPS "/") + file).c_str());
}

namespace {
const size_t SAVE_3DS = 4726152, SAVE_HEAD = 36;
// The Switch save's header (the same in every MHXX Switch save looked at; this port's own save's when there is one)
const uint8_t SAVE_SWITCH_HEAD[SAVE_HEAD] = {0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0x14, 0, 0, 0,
                                             0x74, 0xee, 0xb2, 0x36, 0x0c, 0, 0, 0, 0x88, 0x1d, 0x48, 0, 0, 0, 0, 0};
uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
// MHXX's save: its table of the parts' places (the common data, then the three hunters)
bool mhxx_save_body(const uint8_t *p) {
    return le32(p + 12) == 0xb280 && le32(p + 16) == 0x126474 && le32(p + 20) == 0x244c34 && le32(p + 24) == 0x3633f4;
}
}  // namespace

bool save_import(const std::string &file, bool &was_3ds, std::string &err) {
    size_t n = 0;
    uint8_t *d = (uint8_t *)SDL_LoadFile(file.c_str(), &n);
    if (!d) return err = "can't read " + file + ": " + SDL_GetError(), false;
    std::string out;
    if (n == SAVE_3DS && mhxx_save_body(d)) {
        was_3ds = true;
        uint8_t head[SAVE_HEAD];
        memcpy(head, SAVE_SWITCH_HEAD, SAVE_HEAD);
        size_t m = 0;  // (this port's own save's header, when there is one)
        uint8_t *own = (uint8_t *)SDL_LoadFile((save_dir() + "/save_0/system").c_str(), &m);
        if (own && m == SAVE_3DS + SAVE_HEAD && mhxx_save_body(own + SAVE_HEAD)) memcpy(head, own, SAVE_HEAD);
        SDL_free(own);
        out.assign((const char *)head, SAVE_HEAD);
        out.append((const char *)d, n);
    } else if (n == SAVE_3DS + SAVE_HEAD && mhxx_save_body(d + SAVE_HEAD)) {
        was_3ds = false;
        out.assign((const char *)d, n);
    } else {
        SDL_free(d);
        return err = n == SAVE_3DS || n == SAVE_3DS + SAVE_HEAD ? "this file isn't a Monster Hunter XX save"
                                                               : "not a Monster Hunter XX save (pick the file named system)",
               false;
    }
    SDL_free(d);
    std::string made;  // (the save as it is now: a backup first)
    if (save_exists() && !backup_make("import", made, err)) return false;
    std::string dir = save_dir() + "/save_0";
    SDL_CreateDirectory(save_dir().c_str());
    SDL_CreateDirectory(dir.c_str());
    for (const char *name : {"system", "system_backup"}) {
        std::string at = dir + "/" + name, tmp = at + ".part";
        if (!SDL_SaveFile(tmp.c_str(), out.data(), out.size())) {
            SDL_RemovePath(tmp.c_str());
            return err = "can't write " + tmp + ": " + SDL_GetError(), false;
        }
        SDL_RemovePath(at.c_str());
        if (!SDL_RenamePath(tmp.c_str(), at.c_str())) return err = "can't write " + at + ": " + SDL_GetError(), false;
    }
    return true;
}

namespace {
struct Unpack {
    std::string root;
    int files = 0, saves = 0;
    bool bad = false;
};
SDL_IOStream *unpack_entry(void *ctx, const char *name, bool dir, unsigned) {
    Unpack *u = (Unpack *)ctx;
    std::string n = name;
    if (!safe_rel(n)) {
        u->bad = true;
        return nullptr;
    }
    std::string p = u->root + "/" + n;
    if (dir) {
        SDL_CreateDirectory(p.c_str());
        return nullptr;
    }
    make_parents(p);
    u->files++;
    if (starts(n, "save_")) u->saves++;
    return SDL_IOFromFile(p.c_str(), "wb");
}
}  // namespace

bool backup_restore(const std::string &file, std::string &err) {
    if (file.find('/') != std::string::npos || file.find('\\') != std::string::npos) return err = "bad name", false;
    std::string dir = save_dir(), zip = std::string(BACKUPS "/") + file;
    SDL_CreateDirectory(dir.c_str());
    Unpack u;
    u.root = dir + "/.restoring";
    remove_tree(u.root);
    if (!SDL_CreateDirectory(u.root.c_str())) return err = "can't write in " + dir + ": " + SDL_GetError(), false;
    char e[300] = "";
    bool ok = archive_extract(zip.c_str(), unpack_entry, &u, e, sizeof e);
    if (!ok || u.bad || !u.saves) {
        remove_tree(u.root);
        return err = ok ? (u.bad ? "the backup has a bad path in it" : "the backup has no save in it") : std::string(e), false;
    }
    std::string made;  // (the save as it is now: a backup first)
    if (save_exists() && !backup_make("restore", made, err)) {
        remove_tree(u.root);
        return false;
    }
    // each save_N of the backup in place of the one there (the old one aside until all moved)
    std::vector<std::string> moved;
    for (const std::string &f : save_folders(u.root)) {
        std::string at = dir + "/" + f, old = dir + "/.old_" + f;
        remove_tree(old);
        if (exists(at) && !SDL_RenamePath(at.c_str(), old.c_str())) {
            ok = false, err = "can't move " + at + " aside: " + SDL_GetError();
            break;
        }
        if (!SDL_RenamePath((u.root + "/" + f).c_str(), at.c_str())) {
            ok = false, err = "can't put the save in " + at + ": " + SDL_GetError();
            if (exists(old)) SDL_RenamePath(old.c_str(), at.c_str());
            break;
        }
        moved.push_back(f);
    }
    if (!ok)  // (put back the ones already swapped)
        for (const std::string &f : moved) {
            std::string at = dir + "/" + f, old = dir + "/.old_" + f;
            remove_tree(at);
            if (exists(old)) SDL_RenamePath(old.c_str(), at.c_str());
        }
    for (const std::string &f : moved) remove_tree(dir + "/.old_" + f);
    remove_tree(u.root);
    return ok;
}

// ------------------------------------------------------------------ the PC
void spec_check(Spec &s) {
#ifdef HAVE_CPUID
    unsigned a, b, c, d;
    char brand[49] = {};
    if (__get_cpuid(0x80000000, &a, &b, &c, &d) && a >= 0x80000004)
        for (unsigned i = 0; i < 3; i++) {
            __get_cpuid(0x80000002 + i, &a, &b, &c, &d);
            memcpy(brand + i * 16, &a, 4), memcpy(brand + i * 16 + 4, &b, 4);
            memcpy(brand + i * 16 + 8, &c, 4), memcpy(brand + i * 16 + 12, &d, 4);
        }
    s.cpu = brand;
    while (!s.cpu.empty() && s.cpu[0] == ' ') s.cpu.erase(0, 1);
    // AVX2: the processor has it (leaf 7 EBX bit 5), and the system keeps the AVX registers (OSXSAVE, XCR0 bits 1-2)
    bool osx = __get_cpuid(1, &a, &b, &c, &d) && (c & (1u << 27)) && (c & (1u << 28));
    if (osx) {
        unsigned lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        osx = (lo & 6) == 6;
    }
    s.avx2 = osx && __get_cpuid_count(7, 0, &a, &b, &c, &d) && (b & (1u << 5));
#else
    s.avx2 = true;  // (not an x86 processor: the question doesn't apply)
#endif
    s.threads = SDL_GetNumLogicalCPUCores();
    s.ram_mb = SDL_GetSystemRAM();
#ifdef _WIN32
    ULARGE_INTEGER freeb;
    if (GetDiskFreeSpaceExW(L".", &freeb, nullptr, nullptr)) s.disk_free = freeb.QuadPart;
    typedef LONG(WINAPI * GetVer)(OSVERSIONINFOW *);
    OSVERSIONINFOW v = {sizeof v};
    GetVer gv = (GetVer)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (gv && gv(&v) == 0) {
        char b[64];
        SDL_snprintf(b, sizeof b, "Windows %s (%lu.%lu.%lu)", v.dwBuildNumber >= 22000 ? "11" : "10",
                     (unsigned long)v.dwMajorVersion, (unsigned long)v.dwMinorVersion, (unsigned long)v.dwBuildNumber);
        s.os = b;
    } else s.os = "Windows";
#else
    struct statvfs st;
    if (statvfs(".", &st) == 0) s.disk_free = (uint64_t)st.f_bavail * st.f_frsize;
    struct utsname un;
    s.os = uname(&un) == 0 ? std::string(un.sysname) + " " + un.release : SDL_GetPlatform();
#endif
}

void spec_vulkan(Spec &s) {
    // the loader, an instance, the devices: the best one's name and the Vulkan it does (the game wants 1.2)
#ifdef _WIN32
    SDL_SharedObject *lib = SDL_LoadObject("vulkan-1.dll");
#else
    SDL_SharedObject *lib = SDL_LoadObject("libvulkan.so.1");
#endif
    PFN_vkGetInstanceProcAddr gipa = lib ? (PFN_vkGetInstanceProcAddr)SDL_LoadFunction(lib, "vkGetInstanceProcAddr") : nullptr;
    PFN_vkCreateInstance create = gipa ? (PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance") : nullptr;
    VkInstance inst = VK_NULL_HANDLE;
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "mhxx-launcher";
    app.apiVersion = VK_MAKE_API_VERSION(0, 1, 2, 0);
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    if (create && create(&ci, nullptr, &inst) == VK_SUCCESS) {
        auto enumerate = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
        auto props = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
        auto destroy = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
        uint32_t n = 0;
        if (enumerate && props && enumerate(inst, &n, nullptr) == VK_SUCCESS && n) {
            std::vector<VkPhysicalDevice> devs(n);
            enumerate(inst, &n, devs.data());
            int best = -1;
            for (uint32_t i = 0; i < n; i++) {
                VkPhysicalDeviceProperties p;
                props(devs[i], &p);
                int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3
                            : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                            : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU            ? 0
                                                                                      : 1;
                if (score <= best) continue;
                best = score;
                s.vulkan = true;
                s.vulkan_gpu = p.deviceName;
                s.vk_major = (int)VK_API_VERSION_MAJOR(p.apiVersion), s.vk_minor = (int)VK_API_VERSION_MINOR(p.apiVersion);
            }
        }
        if (destroy) destroy(inst, nullptr);
    }
    if (lib) SDL_UnloadObject(lib);
    s.vulkan_done = true;
}

std::vector<std::string> spec_problems(const Spec &s, bool hard) {
    std::vector<std::string> p;
    if (hard) {
        if (!s.avx2) p.push_back("This processor has no AVX2: the game can't run on it.");
        if (s.vulkan_done && !s.vulkan)
            p.push_back("No graphics card with Vulkan was found: install (or update) the graphics driver.");
        else if (s.vulkan_done && (s.vk_major < 1 || (s.vk_major == 1 && s.vk_minor < 2)))
            p.push_back("The graphics card (or its driver) only does Vulkan 1.0/1.1: the game needs 1.2. Update the driver.");
    } else {
        if (s.ram_mb && s.ram_mb < 6000) p.push_back("Less than 6 GB of memory: close other programs while playing.");
        if (s.disk_free && s.disk_free < 2ull << 30)
            p.push_back("Less than 2 GB free on this drive: the save, caches and textures may not fit.");
    }
    return p;
}

// ------------------------------------------------------------------ the problem report
namespace {
void replace_all(std::string &s, const std::string &from, const std::string &to) {
    if (from.size() < 3) return;
    for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size()) s.replace(at, from.size(), to);
}
#ifdef _WIN32
std::string to_acp(const std::string &utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), &w[0], n);
    int m = WideCharToMultiByte(CP_ACP, 0, w.data(), n, nullptr, 0, nullptr, nullptr);
    std::string out(m, 0);
    WideCharToMultiByte(CP_ACP, 0, w.data(), n, &out[0], m, nullptr, nullptr);
    return out;
}
#endif
// the user's own folder and name, and the end of addresses, hidden
std::string hide_private(std::string s) {
    std::vector<std::pair<std::string, std::string>> names;
    if (const char *h = SDL_getenv("USERPROFILE")) names.push_back({h, "%USERPROFILE%"});
    if (const char *h = SDL_getenv("HOME")) names.push_back({h, "~"});
    if (const char *u = SDL_getenv("USERNAME")) names.push_back({u, "<user>"});
    if (const char *u = SDL_getenv("USER")) names.push_back({u, "<user>"});
    for (auto &n : names) {
        std::string slash = n.first;
        std::replace(slash.begin(), slash.end(), '\\', '/');
        replace_all(s, n.first, n.second);
        replace_all(s, slash, n.second);
#ifdef _WIN32
        replace_all(s, to_acp(n.first), n.second);
#endif
    }
    // a.b.c.d -> a.b.x.x
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (isdigit((unsigned char)s[i]) && (i == 0 || (!isdigit((unsigned char)s[i - 1]) && s[i - 1] != '.'))) {
            size_t j = i;
            int parts = 0;
            size_t cut = 0;
            while (parts < 4) {
                size_t k = j;
                while (k < s.size() && isdigit((unsigned char)s[k]) && k - j < 3) k++;
                if (k == j) break;
                parts++;
                if (parts == 2) cut = k;
                j = k;
                if (parts < 4 && j < s.size() && s[j] == '.' && j + 1 < s.size() && isdigit((unsigned char)s[j + 1])) j++;
                else break;
            }
            if (parts == 4 && (j >= s.size() || (!isdigit((unsigned char)s[j]) && s[j] != '.'))) {
                out += s.substr(i, cut - i) + ".x.x";
                i = j;
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}
// the settings without the multiplayer address and key
std::string hide_ini(const std::string &ini) {
    std::string out;
    for (size_t s = 0; s < ini.size();) {
        size_t e = ini.find('\n', s);
        if (e == std::string::npos) e = ini.size();
        std::string line = ini.substr(s, e - s);
        if (starts(line, "Join =") || starts(line, "Join=")) line = "Join = (hidden)";
        if (starts(line, "Key =") || starts(line, "Key=")) line = "Key = (hidden)";
        out += line + "\n";
        s = e + 1;
    }
    return out;
}
std::string log_part(const std::string &path) {  // (a big log: its start and its end)
    std::string d = load(path);
    const size_t head = 512 << 10, tail = 4 << 20;
    if (d.size() > head + tail)
        d = d.substr(0, head) + "\n\n[... " + std::to_string(d.size() - head - tail) + " bytes left out ...]\n\n" +
            d.substr(d.size() - tail);
    return d;
}
int count_png(const std::string &dir) {
    int n = 0;
    char **e = SDL_GlobDirectory(dir.c_str(), "*.png", SDL_GLOB_CASEINSENSITIVE, &n);
    SDL_free(e);
    return e ? n : 0;
}
}  // namespace

namespace {
void addf(std::string &t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void addf(std::string &t, const char *fmt, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    t += b;
    t += "\n";
}
}  // namespace

bool report_make(const Spec &spec, const std::string &version, std::string &made, std::string &err) {
    std::string t;
    SDL_DateTime d = now_local();
    addf(t, "MHXX Recomp problem report, %04d-%02d-%02d %02d:%02d", d.year, d.month, d.day, d.hour, d.minute);
    addf(t, "version: %s", version.empty() ? "development build" : version.c_str());
    addf(t, "os: %s", spec.os.c_str());
    addf(t, "cpu: %s, %d threads, AVX2 %s", spec.cpu.c_str(), spec.threads, spec.avx2 ? "yes" : "NO");
    addf(t, "memory: %d MB", spec.ram_mb);
    addf(t, "disk free (game's drive): %s", size_text(spec.disk_free).c_str());
    addf(t, "gpu (dxgi): %s, %s%s", g_gpu_name[0] ? g_gpu_name : "?", size_text(g_gpu_vram).c_str(), g_gpu_integrated ? " (integrated)" : "");
    if (!spec.vulkan_done) addf(t, "vulkan: not checked yet");
    else if (!spec.vulkan) addf(t, "vulkan: NONE");
    else addf(t, "vulkan: %d.%d, %s", spec.vk_major, spec.vk_minor, spec.vulkan_gpu.c_str());
    addf(t, "%s", "");
    addf(t, "game files: exefs/main %s, romfs %s", exists("data/exefs/main") ? "found" : "MISSING",
         is_dir("data/romfs") ? "found" : "MISSING");
    std::string mods;
    for (const std::string &m : list("mods"))
        if (is_dir("mods/" + m)) mods += (mods.empty() ? "" : ", ") + m;
    addf(t, "mods: %s", mods.empty() ? "none" : mods.c_str());
    addf(t, "texture pack: gear %d, monsters %d, other %d", count_png("textures/load/gear"), count_png("textures/load/monsters"),
         count_png("textures/load"));
    addf(t, "cache: %s", size_text(cache_size()).c_str());
    addf(t, "save: %s (%s)", save_dir().c_str(), save_exists() ? "has a save" : "no save");
    addf(t, "backups: %d", (int)backup_list().size());
    for (bool hard : {true, false})
        for (const std::string &p : spec_problems(spec, hard)) addf(t, "problem: %s", p.c_str());

    SDL_CreateDirectory(REPORTS);
    std::string name = "report-" + stamp() + ".zip", path = std::string(REPORTS "/") + name;
    ZipOut z(path);
    if (!z.ok) return err = "can't write " + path + ": " + SDL_GetError(), false;
    z.add("system.txt", hide_private(t));
    static const char *const logs[][2] = {{"log.txt", "log.txt"}, {"install.log", "install.log"},
                                          {"update/update.log", "update.log"}, {"launcher.log", "launcher.log"}, {"launcher.prev.log", "launcher.prev.log"},
                                          {"launcher.ini", "launcher.ini"}};
    for (auto &l : logs)
        if (exists(l[0])) z.add(l[1], hide_private(log_part(l[0])));
    if (exists("mhgurecomp.ini")) z.add("mhgurecomp.ini", hide_private(hide_ini(load("mhgurecomp.ini"))));
    if (!z.finish()) {
        SDL_RemovePath(path.c_str());
        return err = "can't write " + path + " (disk full?)", false;
    }
    made = path;
    return true;
}

void open_folder(const char *dir) {
    const char *base = SDL_GetBasePath();
    std::string p = std::string(base ? base : "") + dir;
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, p.c_str(), -1, nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, p.c_str(), -1, &w[0], n);
    ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    SDL_OpenURL(("file://" + p).c_str());
#endif
}

// ------------------------------------------------------------------ the caches
uint64_t cache_size() { return tree_size(CACHE); }
bool cache_clear(uint64_t &freed, std::string &err) {
    freed = cache_size();
    if (!exists(CACHE)) return true;
    bool ok = remove_tree(CACHE);
    uint64_t left = cache_size();
    freed -= std::min(freed, left);
    if (!ok && left) return err = std::string("some of cache\\ couldn't be removed (in use?): ") + SDL_GetError(), false;
    return true;
}

std::string size_text(uint64_t n) {
    char b[32];
    if (n >= 1ull << 30) SDL_snprintf(b, sizeof b, "%.1f GB", n / 1073741824.0);
    else if (n >= 1ull << 20) SDL_snprintf(b, sizeof b, "%.1f MB", n / 1048576.0);
    else if (n >= 1024) SDL_snprintf(b, sizeof b, "%.0f KB", n / 1024.0);
    else SDL_snprintf(b, sizeof b, "%d B", (int)n);
    return b;
}

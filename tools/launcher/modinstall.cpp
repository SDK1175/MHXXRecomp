// A mod into mods/ from a .7z or a folder (modinstall.h; a .zip: nxinstall.cpp install_mod_zip).
#include "modinstall.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "nxinstall.h"

extern "C" {
#include "../../third_party/lzma/7z.h"
#include "../../third_party/lzma/7zAlloc.h"
#include "../../third_party/lzma/7zCrc.h"
}

namespace {
const ISzAlloc g_Alloc = {SzAlloc, SzFree};
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
        for (const std::string &c : list(p)) remove_tree(p + "/" + c);
    SDL_RemovePath(p.c_str());
}
void make_parent(const std::string &path) {
    size_t s = path.rfind('/');
    if (s != std::string::npos) SDL_CreateDirectory(path.substr(0, s).c_str());
}
bool safe_rel(const std::string &p) {
    if (p.empty() || p[0] == '/' || p.find(':') != std::string::npos) return false;
    for (size_t s = 0; s <= p.size();) {
        size_t e = p.find('/', s);
        if (e == std::string::npos) e = p.size();
        if (e - s == 2 && p.compare(s, 2, "..") == 0) return false;
        s = e + 1;
    }
    return true;
}
std::string lower(std::string s) {
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}
std::string base_name(std::string p) {
    while (!p.empty() && (p.back() == '/' || p.back() == '\\')) p.pop_back();
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}
std::string utf16_to_utf8(const UInt16 *w) {
    std::string out;
    for (size_t i = 0; w[i]; i++) {
        uint32_t c = w[i];
        if (c >= 0xD800 && c < 0xDC00 && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000) c = 0x10000 + ((c - 0xD800) << 10) + (w[++i] - 0xDC00);
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) out += (char)(0xC0 | c >> 6), out += (char)(0x80 | (c & 63));
        else if (c < 0x10000) out += (char)(0xE0 | c >> 12), out += (char)(0x80 | (c >> 6 & 63)), out += (char)(0x80 | (c & 63));
        else
            out += (char)(0xF0 | c >> 18), out += (char)(0x80 | (c >> 12 & 63)), out += (char)(0x80 | (c >> 6 & 63)),
                out += (char)(0x80 | (c & 63));
    }
    return out;
}

// the .7z read through SDL (UTF-8 paths on every system)
struct InFile {
    ISeekInStream vt;
    SDL_IOStream *io;
};
SRes in_read(ISeekInStreamPtr pp, void *buf, size_t *size) {
    InFile *p = (InFile *)pp;
    if (!*size) return SZ_OK;
    size_t n = SDL_ReadIO(p->io, buf, *size);
    *size = n;
    return n || SDL_GetIOStatus(p->io) == SDL_IO_STATUS_EOF ? SZ_OK : SZ_ERROR_READ;
}
SRes in_seek(ISeekInStreamPtr pp, Int64 *pos, ESzSeek origin) {
    InFile *p = (InFile *)pp;
    SDL_IOWhence w = origin == SZ_SEEK_SET ? SDL_IO_SEEK_SET : origin == SZ_SEEK_CUR ? SDL_IO_SEEK_CUR : SDL_IO_SEEK_END;
    Sint64 r = SDL_SeekIO(p->io, *pos, w);
    if (r < 0) return SZ_ERROR_READ;
    *pos = r;
    return SZ_OK;
}
const char *sz_error(SRes r) {
    switch (r) {
    case SZ_ERROR_UNSUPPORTED: return "packed with a method this can't read";
    case SZ_ERROR_MEM: return "not enough memory";
    case SZ_ERROR_CRC: return "damaged (CRC)";
    case SZ_ERROR_NO_ARCHIVE: return "not a 7z";
    case SZ_ERROR_ARCHIVE: return "damaged";
    case SZ_ERROR_INPUT_EOF: return "cut short";
    default: return "can't be read";
    }
}
// where the files go: one folder in mods/ named as the archive / folder; an archive whose files are all in a folder of
// that same name: that folder (as install_mod_zip)
std::string stem_of(const std::string &file) {
    std::string s = base_name(file);
    size_t dot = s.rfind('.');
    return dot == std::string::npos || dot == 0 ? s : s.substr(0, dot);
}

std::string absolute(std::string p) {  // (with '/'; relative: to the current folder, the launcher's)
    std::replace(p.begin(), p.end(), '\\', '/');
    bool abs = (!p.empty() && p[0] == '/') || (p.size() > 1 && p[1] == ':');
    if (!abs) {
        char *cwd = SDL_GetCurrentDirectory();
        std::string c = cwd ? cwd : "";
        SDL_free(cwd);
        std::replace(c.begin(), c.end(), '\\', '/');
        if (!c.empty() && c.back() != '/') c += '/';
        p = c + p;
    }
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

bool copy_tree(const std::string &from, const std::string &to, std::string &err, int depth = 0) {
    if (depth > 32) return true;
    SDL_CreateDirectory(to.c_str());
    for (const std::string &c : list(from)) {
        std::string a = from + "/" + c, b = to + "/" + c;
        if (is_dir(a)) {
            if (!copy_tree(a, b, err, depth + 1)) return false;
        } else if (!SDL_CopyFile(a.c_str(), b.c_str())) {
            return err = "can't copy " + a + ": " + SDL_GetError(), false;
        }
    }
    return true;
}
}  // namespace

bool install_mod_7z(const std::string &file, const std::string &mods, std::string &name, std::string &err) {
    static bool crc_ready = false;
    if (!crc_ready) CrcGenerateTable(), crc_ready = true;
    InFile in = {};
    in.vt.Read = in_read, in.vt.Seek = in_seek;
    if (!(in.io = SDL_IOFromFile(file.c_str(), "rb"))) return err = "can't open " + file, false;
    CLookToRead2 look = {};
    LookToRead2_CreateVTable(&look, False);
    look.realStream = &in.vt;
    look.bufSize = 1 << 18;
    std::vector<Byte> lookbuf(look.bufSize);
    look.buf = lookbuf.data();
    LookToRead2_INIT(&look);
    CSzArEx db;
    SzArEx_Init(&db);
    bool ok = false;
    Byte *out = nullptr;
    size_t out_size = 0;
    UInt32 block = 0xFFFFFFFF;
    std::vector<UInt16> wname;
    struct Ent {
        UInt32 index;
        std::string name;
    };
    std::vector<Ent> es;
    std::string stem = stem_of(file), root = mods + "/" + stem;
    bool one = true;
    SRes r = SzArEx_Open(&db, &look.vt, &g_Alloc, &g_Alloc);
    if (r != SZ_OK) {
        err = file + ": " + sz_error(r);
        goto done;
    }
    for (UInt32 i = 0; i < db.NumFiles; i++) {
        if (SzArEx_IsDir(&db, i)) continue;
        size_t n = SzArEx_GetFileNameUtf16(&db, i, nullptr);
        wname.assign(n + 1, 0);
        SzArEx_GetFileNameUtf16(&db, i, wname.data());
        std::string nm = utf16_to_utf8(wname.data());
        std::replace(nm.begin(), nm.end(), '\\', '/');
        if (!safe_rel(nm)) {
            err = file + ": bad path " + nm;
            goto done;
        }
        one &= nm.compare(0, stem.size() + 1, stem + "/") == 0;
        es.push_back({i, nm});
    }
    if (es.empty()) {
        err = file + ": empty";
        goto done;
    }
    SDL_CreateDirectory(mods.c_str());
    remove_tree(root);
    SDL_CreateDirectory(root.c_str());
    for (const Ent &e : es) {
        size_t offset = 0, size = 0;
        r = SzArEx_Extract(&db, &look.vt, e.index, &block, &out, &out_size, &offset, &size, &g_Alloc, &g_Alloc);
        if (r != SZ_OK) {
            err = file + ": " + e.name + " " + sz_error(r);
            goto done;
        }
        std::string path = one ? mods + "/" + e.name : root + "/" + e.name;
        make_parent(path);
        if (!SDL_SaveFile(path.c_str(), out + offset, size)) {
            err = "can't write " + path + ": " + SDL_GetError();
            goto done;
        }
    }
    name = stem;
    ok = true;
done:
    if (out) ISzAlloc_Free(&g_Alloc, out);
    SzArEx_Free(&db, &g_Alloc);
    SDL_CloseIO(in.io);
    if (!ok && !es.empty()) remove_tree(root);  // (half of it: gone)
    return ok;
}

bool install_mod_folder(const std::string &dir, const std::string &mods, std::string &name, std::string &err) {
    std::string d = dir;
    std::replace(d.begin(), d.end(), '\\', '/');
    while (!d.empty() && d.back() == '/') d.pop_back();
    if (!is_dir(d)) return err = dir + ": not a folder", false;
    // (a mod's own romfs / exefs / nativeNX / title id folder picked: the mod is the folder it is in, and it keeps it)
    std::string leaf = base_name(d), low = lower(leaf);
    bool inner = low == "romfs" || low == "exefs" || low == "nativenx" || (low.size() == 16 && low.compare(0, 4, "0100") == 0);
    std::string parent = d.substr(0, d.size() - leaf.size());
    while (!parent.empty() && parent.back() == '/') parent.pop_back();
    name = inner && !parent.empty() ? base_name(parent) : leaf;
    if (name.empty()) return err = dir + ": pick the mod's own folder", false;
    // (not mods/ itself or a folder in it: that one would be removed before it's copied)
    if (lower(absolute(d) + "/").compare(0, absolute(mods).size() + 1, lower(absolute(mods) + "/")) == 0)
        return err = "that folder is in mods already", false;
    SDL_CreateDirectory(mods.c_str());
    std::string root = mods + "/" + name;
    remove_tree(root);
    bool ok = inner ? copy_tree(d, root + "/" + leaf, err) : copy_tree(d, root, err);
    if (!ok) remove_tree(root);
    return ok;
}

bool install_mod(const std::string &path, const std::string &mods, std::string &name, std::string &err) {
    if (is_dir(path)) return install_mod_folder(path, mods, name, err);
    std::string ext = lower(path.substr(path.find_last_of('.') == std::string::npos ? path.size() : path.find_last_of('.')));
    if (ext == ".7z") return install_mod_7z(path, mods, name, err);
    if (ext == ".rar") return err = base_name(path) + ": a .rar can't be read here: unpack it, then pick its folder", false;
    return install_mod_zip(path, mods, name, err);
}

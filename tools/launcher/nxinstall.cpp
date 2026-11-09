// The game files from the player's own Switch files (nxinstall.h): the game's NSP or XCI and its 1.5.1 update NSP,
// decrypted with the keys of the player's console (prod.keys), the update laid over the game as the console does it,
// written out as data/exefs and data/romfs; then the Korean patch's .zip unpacked into mods/.
//
// The formats (switchbrew.org: NCA, PFS0, HFS0, XCI, Ticket, RomFS; hactool):
//   NSP  a PFS0: the content archives (.nca) and the tickets (.tik) of the titles that need one.
//   XCI  a game card: its header ("HEAD" at 0x100) points at the root HFS0, whose "secure" partition holds the .nca.
//   NCA  0xC00 bytes of header, AES-XTS (header_key, 0x200 sectors, the sector number big-endian as the tweak); up to
//        four sections, each AES-CTR with a key from the key area (key_area_key_application_<generation>) or, with a
//        rights id, the title key of the ticket (titlekek_<generation>). The counter: the section's SecureValue and
//        Generation (big-endian) then the offset in the NCA / 16 (big-endian).
//   ExeFS  the program NCA's PFS0 section. RomFS  its IVFC section, the data at the hash tree's last level.
//   Update the update's program NCA has its own ExeFS, and a RomFS section that is a patch (BKTR): an Indirect table
//        maps the patched section onto the game's own RomFS section or the update's, and an AesCtrEx table gives each
//        stretch of the update's the Generation its counter uses.
#include "nxinstall.h"

#include <SDL3/SDL.h>
#include <zlib.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <cpuid.h>
#include <immintrin.h>
#define NX_AESNI 1
#endif
#ifdef _WIN32
#include <windows.h>
#endif

extern "C" {
#include "game/game.h"  // (GAME_MAIN_BUILD_ID: the exefs/main of 1.5.1)
}

namespace {

// ------------------------------------------------------------------ log (install.log)
SDL_IOStream *g_log;
void logf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_log) SDL_WriteIO(g_log, buf, strlen(buf)), SDL_FlushIO(g_log);
}
std::string hex(const uint8_t *p, size_t n) {
    static const char *d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) s += d[p[i] >> 4], s += d[p[i] & 15];
    return s;
}
uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
uint64_t u64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

// ------------------------------------------------------------------ AES-128 (AES-NI when the CPU has it)
uint8_t SBOX[256], ISBOX[256];
void aes_tables() {
    static bool done;
    if (done) return;
    uint8_t p = 1, q = 1;
    do {  // (p runs over the field by multiplying by 3, q by dividing: q = p^-1)
        p = (uint8_t)(p ^ (p << 1) ^ (p & 0x80 ? 0x1b : 0));
        q ^= (uint8_t)(q << 1);
        q ^= (uint8_t)(q << 2);
        q ^= (uint8_t)(q << 4);
        if (q & 0x80) q ^= 0x09;
        auto rot = [](uint8_t x, int s) { return (uint8_t)((x << s) | (x >> (8 - s))); };
        SBOX[p] = (uint8_t)(q ^ rot(q, 1) ^ rot(q, 2) ^ rot(q, 3) ^ rot(q, 4) ^ 0x63);
    } while (p != 1);
    SBOX[0] = 0x63;
    for (int i = 0; i < 256; i++) ISBOX[SBOX[i]] = (uint8_t)i;
    done = true;
}
uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = (uint8_t)((a << 1) ^ (a & 0x80 ? 0x1b : 0));
        b >>= 1;
    }
    return r;
}

#ifdef NX_AESNI
bool cpu_aes() {
    unsigned a, b, c, d;
    return __get_cpuid(1, &a, &b, &c, &d) && (c & (1u << 25));
}
__attribute__((target("aes,sse2"))) void ni_keys(const uint8_t *rk, __m128i *ek, __m128i *dk) {
    for (int i = 0; i < 11; i++) ek[i] = _mm_loadu_si128((const __m128i *)(rk + 16 * i));
    dk[0] = ek[10];
    for (int i = 1; i < 10; i++) dk[i] = _mm_aesimc_si128(ek[10 - i]);
    dk[10] = ek[0];
}
__attribute__((target("aes,sse2"))) void ni_enc(const __m128i *k, const uint8_t *in, uint8_t *out) {
    __m128i x = _mm_xor_si128(_mm_loadu_si128((const __m128i *)in), k[0]);
    for (int i = 1; i < 10; i++) x = _mm_aesenc_si128(x, k[i]);
    _mm_storeu_si128((__m128i *)out, _mm_aesenclast_si128(x, k[10]));
}
__attribute__((target("aes,sse2"))) void ni_dec(const __m128i *k, const uint8_t *in, uint8_t *out) {
    __m128i x = _mm_xor_si128(_mm_loadu_si128((const __m128i *)in), k[0]);
    for (int i = 1; i < 10; i++) x = _mm_aesdec_si128(x, k[i]);
    _mm_storeu_si128((__m128i *)out, _mm_aesdeclast_si128(x, k[10]));
}
// CTR over whole blocks: counter = hi (8 bytes) || block (big-endian), four blocks at a time
__attribute__((target("aes,sse2,ssse3"))) void ni_ctr(const __m128i *k, const uint8_t hi[8], uint64_t block, uint8_t *p,
                                                       size_t blocks) {
    uint8_t c[16];
    memcpy(c, hi, 8);
    auto ctr = [&](uint64_t b) {
        for (int j = 0; j < 8; j++) c[15 - j] = (uint8_t)(b >> (8 * j));
        return _mm_loadu_si128((const __m128i *)c);
    };
    size_t i = 0;
    for (; i + 4 <= blocks; i += 4) {
        __m128i x0 = _mm_xor_si128(ctr(block + i), k[0]), x1 = _mm_xor_si128(ctr(block + i + 1), k[0]);
        __m128i x2 = _mm_xor_si128(ctr(block + i + 2), k[0]), x3 = _mm_xor_si128(ctr(block + i + 3), k[0]);
        for (int r = 1; r < 10; r++)
            x0 = _mm_aesenc_si128(x0, k[r]), x1 = _mm_aesenc_si128(x1, k[r]), x2 = _mm_aesenc_si128(x2, k[r]),
            x3 = _mm_aesenc_si128(x3, k[r]);
        x0 = _mm_aesenclast_si128(x0, k[10]), x1 = _mm_aesenclast_si128(x1, k[10]);
        x2 = _mm_aesenclast_si128(x2, k[10]), x3 = _mm_aesenclast_si128(x3, k[10]);
        __m128i *q = (__m128i *)(p + 16 * i);
        _mm_storeu_si128(q, _mm_xor_si128(_mm_loadu_si128(q), x0));
        _mm_storeu_si128(q + 1, _mm_xor_si128(_mm_loadu_si128(q + 1), x1));
        _mm_storeu_si128(q + 2, _mm_xor_si128(_mm_loadu_si128(q + 2), x2));
        _mm_storeu_si128(q + 3, _mm_xor_si128(_mm_loadu_si128(q + 3), x3));
    }
    for (; i < blocks; i++) {
        __m128i x = _mm_xor_si128(ctr(block + i), k[0]);
        for (int r = 1; r < 10; r++) x = _mm_aesenc_si128(x, k[r]);
        x = _mm_aesenclast_si128(x, k[10]);
        __m128i *q = (__m128i *)(p + 16 * i);
        _mm_storeu_si128(q, _mm_xor_si128(_mm_loadu_si128(q), x));
    }
}
#endif

struct Aes {
    uint8_t rk[176];
#ifdef NX_AESNI
    __m128i ek[11], dk[11];
    bool ni = false;
#endif
    void set(const uint8_t key[16]) {
        aes_tables();
        memcpy(rk, key, 16);
        uint8_t rcon = 1;
        for (int i = 4; i < 44; i++) {
            uint8_t t[4];
            memcpy(t, rk + 4 * (i - 1), 4);
            if (i % 4 == 0) {
                uint8_t f = t[0];
                t[0] = (uint8_t)(SBOX[t[1]] ^ rcon), t[1] = SBOX[t[2]], t[2] = SBOX[t[3]], t[3] = SBOX[f];
                rcon = (uint8_t)((rcon << 1) ^ (rcon & 0x80 ? 0x1b : 0));
            }
            for (int j = 0; j < 4; j++) rk[4 * i + j] = rk[4 * (i - 4) + j] ^ t[j];
        }
#ifdef NX_AESNI
        static int have = -1;
        if (have < 0) have = cpu_aes() && !SDL_getenv("MHXX_NO_AESNI");  // (MHXX_NO_AESNI: the plain one, to test it)
        ni = have;
        if (ni) ni_keys(rk, ek, dk);
#endif
    }
    void enc(const uint8_t *in, uint8_t *out) const {
#ifdef NX_AESNI
        if (ni) return ni_enc(ek, in, out);
#endif
        uint8_t s[16], t[16];
        for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk[i];
        for (int r = 1; r <= 10; r++) {
            for (int i = 0; i < 16; i++) t[i] = SBOX[s[(i + 4 * (i % 4)) % 16]];  // SubBytes + ShiftRows
            if (r < 10)
                for (int c = 0; c < 4; c++) {
                    uint8_t *a = t + 4 * c, b0 = a[0], b1 = a[1], b2 = a[2], b3 = a[3];
                    a[0] = gmul(b0, 2) ^ gmul(b1, 3) ^ b2 ^ b3, a[1] = b0 ^ gmul(b1, 2) ^ gmul(b2, 3) ^ b3;
                    a[2] = b0 ^ b1 ^ gmul(b2, 2) ^ gmul(b3, 3), a[3] = gmul(b0, 3) ^ b1 ^ b2 ^ gmul(b3, 2);
                }
            for (int i = 0; i < 16; i++) s[i] = t[i] ^ rk[16 * r + i];
        }
        memcpy(out, s, 16);
    }
    void dec(const uint8_t *in, uint8_t *out) const {
#ifdef NX_AESNI
        if (ni) return ni_dec(dk, in, out);
#endif
        uint8_t s[16], t[16];
        for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk[160 + i];
        for (int r = 9; r >= 0; r--) {
            for (int i = 0; i < 16; i++) t[(i + 4 * (i % 4)) % 16] = ISBOX[s[i]];  // InvShiftRows + InvSubBytes
            for (int i = 0; i < 16; i++) t[i] ^= rk[16 * r + i];
            if (r > 0)
                for (int c = 0; c < 4; c++) {
                    uint8_t *a = t + 4 * c, b0 = a[0], b1 = a[1], b2 = a[2], b3 = a[3];
                    a[0] = gmul(b0, 14) ^ gmul(b1, 11) ^ gmul(b2, 13) ^ gmul(b3, 9);
                    a[1] = gmul(b0, 9) ^ gmul(b1, 14) ^ gmul(b2, 11) ^ gmul(b3, 13);
                    a[2] = gmul(b0, 13) ^ gmul(b1, 9) ^ gmul(b2, 14) ^ gmul(b3, 11);
                    a[3] = gmul(b0, 11) ^ gmul(b1, 13) ^ gmul(b2, 9) ^ gmul(b3, 14);
                }
            memcpy(s, t, 16);
        }
        memcpy(out, s, 16);
    }
    // CTR: p holds the bytes from `block` * 16 on (n of them); the counter's upper half is hi
    void ctr(const uint8_t hi[8], uint64_t block, uint8_t *p, size_t n) const {
        size_t whole = n / 16;
#ifdef NX_AESNI
        if (ni) ni_ctr(ek, hi, block, p, whole);
        else
#endif
            for (size_t i = 0; i < whole; i++) {
                uint8_t c[16], k[16];
                memcpy(c, hi, 8);
                for (int j = 0; j < 8; j++) c[15 - j] = (uint8_t)((block + i) >> (8 * j));
                enc(c, k);
                for (int j = 0; j < 16; j++) p[16 * i + j] ^= k[j];
            }
        if (n % 16) {
            uint8_t c[16], k[16];
            memcpy(c, hi, 8);
            for (int j = 0; j < 8; j++) c[15 - j] = (uint8_t)((block + whole) >> (8 * j));
            enc(c, k);
            for (size_t j = 0; j < n % 16; j++) p[16 * whole + j] ^= k[j];
        }
    }
};
void ecb_dec(const uint8_t key[16], const uint8_t *in, uint8_t *out, size_t n) {
    Aes a;
    a.set(key);
    for (size_t i = 0; i + 16 <= n; i += 16) a.dec(in + i, out + i);
}
// AES-XTS with Nintendo's tweak (the sector number big-endian), in place
void xts_dec(const uint8_t key[32], uint8_t *p, size_t n, uint64_t sector, size_t sector_size) {
    Aes k1, k2;
    k1.set(key), k2.set(key + 16);
    for (size_t off = 0; off < n; off += sector_size, sector++) {
        uint8_t t[16] = {}, b[16];
        for (int j = 0; j < 8; j++) t[15 - j] = (uint8_t)(sector >> (8 * j));
        k2.enc(t, t);
        for (size_t i = off; i < off + sector_size && i + 16 <= n; i += 16) {
            for (int j = 0; j < 16; j++) b[j] = p[i + j] ^ t[j];
            k1.dec(b, b);
            for (int j = 0; j < 16; j++) p[i + j] = b[j] ^ t[j];
            uint8_t carry = 0;  // t *= x in GF(2^128), little-endian
            for (int j = 0; j < 16; j++) {
                uint8_t c = t[j] >> 7;
                t[j] = (uint8_t)((t[j] << 1) | carry);
                carry = c;
            }
            if (carry) t[0] ^= 0x87;
        }
    }
}

// ------------------------------------------------------------------ keys (prod.keys: "name = hex" lines)
struct Keys {
    std::map<std::string, std::vector<uint8_t>> k;
    bool load(const std::string &path, std::string &err) {
        size_t n = 0;
        char *d = (char *)SDL_LoadFile(path.c_str(), &n);
        if (!d) return err = "keys: can't read " + path, false;
        std::string s(d, n);
        SDL_free(d);
        size_t pos = 0;
        while (pos < s.size()) {
            size_t e = s.find('\n', pos);
            if (e == std::string::npos) e = s.size();
            std::string line = s.substr(pos, e - pos);
            pos = e + 1;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            auto trim = [](std::string x) {
                while (!x.empty() && strchr(" \t\r", x.back())) x.pop_back();
                while (!x.empty() && strchr(" \t\r", x.front())) x.erase(x.begin());
                return x;
            };
            std::string name = trim(line.substr(0, eq)), val = trim(line.substr(eq + 1));
            for (char &c : name) c = (char)tolower((unsigned char)c);
            if (val.size() % 2) continue;
            std::vector<uint8_t> bytes;
            bool ok = true;
            for (size_t i = 0; i < val.size() && ok; i += 2) {
                char h[3] = {val[i], val[i + 1], 0};
                char *end;
                long b = strtol(h, &end, 16);
                if (*end) ok = false;
                bytes.push_back((uint8_t)b);
            }
            if (ok && !bytes.empty()) k[name] = bytes;
        }
        derive();
        logf("keys: %s, %d keys\n", path.c_str(), (int)k.size());
        if (!get("header_key", 32)) return err = "keys: no header_key in " + path, false;
        return true;
    }
    const uint8_t *get(const std::string &name, size_t size) const {
        auto it = k.find(name);
        return it != k.end() && it->second.size() == size ? it->second.data() : nullptr;
    }
    static std::string idx(const char *name, int i) {
        char b[64];
        SDL_snprintf(b, sizeof b, "%s_%02x", name, i);
        return b;
    }
    // (the keys a dump tool also prints, from the master keys and the sources, when the file has only those)
    void derive() {
        const uint8_t *kek_seed = get("aes_kek_generation_source", 16), *key_seed = get("aes_key_generation_source", 16);
        auto kek = [&](const uint8_t *src, const uint8_t *master, uint8_t out[16]) {
            uint8_t a[16], b[16];
            ecb_dec(master, kek_seed, a, 16);
            ecb_dec(a, src, b, 16);
            ecb_dec(b, key_seed, out, 16);
        };
        const uint8_t *hk = get("header_kek_source", 16), *hs = get("header_key_source", 32), *m0 = get("master_key_00", 16);
        if (!get("header_key", 32) && kek_seed && key_seed && hk && hs && m0) {
            uint8_t h[16];
            std::vector<uint8_t> out(32);
            kek(hk, m0, h);
            ecb_dec(h, hs, out.data(), 32);
            k["header_key"] = out;
        }
        const uint8_t *kaks = get("key_area_key_application_source", 16), *tks = get("titlekek_source", 16);
        for (int i = 0; i < 0x20; i++) {
            const uint8_t *m = get(idx("master_key", i), 16);
            if (!m) continue;
            if (!get(idx("key_area_key_application", i), 16) && kaks && kek_seed && key_seed) {
                std::vector<uint8_t> out(16);
                kek(kaks, m, out.data());
                k[idx("key_area_key_application", i)] = out;
            }
            if (!get(idx("titlekek", i), 16) && tks) {
                std::vector<uint8_t> out(16);
                ecb_dec(m, tks, out.data(), 16);
                k[idx("titlekek", i)] = out;
            }
        }
    }
};

// ------------------------------------------------------------------ files: a package (NSP / XCI) and what is in it
struct File {
    SDL_IOStream *io = nullptr;
    uint64_t size = 0;
    std::string path;
    bool open(const std::string &p) {
        path = p;
        io = SDL_IOFromFile(p.c_str(), "rb");
        if (io) size = (uint64_t)SDL_GetIOSize(io);
        return io != nullptr;
    }
    ~File() {
        if (io) SDL_CloseIO(io);
    }
    bool read(uint64_t off, void *buf, size_t n) {
        if (off + n > size || SDL_SeekIO(io, (Sint64)off, SDL_IO_SEEK_SET) < 0) return false;
        uint8_t *p = (uint8_t *)buf;
        while (n) {
            size_t got = SDL_ReadIO(io, p, n);
            if (!got) return false;
            p += got, n -= got;
        }
        return true;
    }
};
struct Entry {
    std::string name;
    uint64_t off, size;  // (in the file)
};
// PFS0 (entries of 0x18) or HFS0 (0x40) at `at`
bool partition(File &f, uint64_t at, std::vector<Entry> &out, const char *magic, size_t entry) {
    uint8_t h[16];
    if (!f.read(at, h, 16) || memcmp(h, magic, 4)) return false;
    uint32_t n = u32(h + 4), strings = u32(h + 8);
    if (n > 100000) return false;
    std::vector<uint8_t> t(entry * n + strings);
    if (!f.read(at + 16, t.data(), t.size())) return false;
    uint64_t data = at + 16 + t.size();
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = t.data() + entry * i;
        uint32_t so = u32(e + 16);
        if (so >= strings) return false;
        const char *name = (const char *)t.data() + entry * n + so;
        out.push_back({std::string(name, strnlen(name, strings - so)), data + u64(e), u64(e + 8)});
    }
    return true;
}
// An NSP's files, or an XCI's secure partition's
bool package(File &f, std::vector<Entry> &out, std::string &err) {
    if (partition(f, 0, out, "PFS0", 0x18)) return logf("%s: NSP, %d files\n", f.path.c_str(), (int)out.size()), true;
    for (uint64_t shift : {(uint64_t)0, (uint64_t)0x1000}) {  // (a dump may start with the card's key area: 0x1000 bytes)
        uint8_t h[0x200];
        if (!f.read(shift, h, sizeof h) || memcmp(h + 0x100, "HEAD", 4)) continue;
        uint64_t root = shift + u64(h + 0x130);
        std::vector<Entry> parts;
        if (!partition(f, root, parts, "HFS0", 0x40)) continue;
        for (const Entry &p : parts)
            if (p.name == "secure" && partition(f, p.off, out, "HFS0", 0x40))
                return logf("%s: XCI, secure partition %d files\n", f.path.c_str(), (int)out.size()), true;
    }
    err = "not an NSP or XCI (or compressed: NSZ/XCZ are not read): " + f.path;
    return false;
}

// ------------------------------------------------------------------ NCA
struct Section {
    bool present = false;
    uint64_t start = 0, end = 0;  // (in the NCA)
    uint8_t h[0x200];             // FsHeader
    int fs_type = 0, enc = 0;
    uint8_t hi[8];  // the counter's upper half: SecureValue, Generation (big-endian)
    Aes aes;
};
struct Nca {
    File *f = nullptr;
    uint64_t at = 0, size = 0;
    std::string name;
    uint8_t h[0xC00];
    int type = -1, keygen = 0;
    uint64_t program = 0;
    Section s[4];
    std::vector<uint8_t> scratch;

    // the section's bytes [off, off + n), decrypted (CTR with the section's counter, or `hi` when given)
    bool read(int i, uint64_t off, uint8_t *out, size_t n, const uint8_t *hi = nullptr) {
        Section &c = s[i];
        uint64_t abs = c.start + off;
        if (abs + n > c.end) return false;
        if (c.enc == 1) return f->read(at + abs, out, n);  // (none)
        uint64_t a = abs & ~15ull;
        size_t lead = (size_t)(abs - a), len = lead + n;
        if (!lead) {  // (straight into out)
            if (!f->read(at + a, out, n)) return false;
            c.aes.ctr(hi ? hi : c.hi, a / 16, out, n);
            return true;
        }
        if (scratch.size() < len) scratch.resize(len);
        if (!f->read(at + a, scratch.data(), len)) return false;
        c.aes.ctr(hi ? hi : c.hi, a / 16, scratch.data(), len);
        memcpy(out, scratch.data() + lead, n);
        return true;
    }
};
// tickets: rights id -> the title key (still under the titlekek)
using Tickets = std::map<std::string, std::vector<uint8_t>>;
void tickets(File &f, const std::vector<Entry> &files, Tickets &out) {
    for (const Entry &e : files) {
        if (e.name.size() < 4 || e.name.compare(e.name.size() - 4, 4, ".tik") || e.size < 0x2C0) continue;
        uint8_t t[0x2C0];
        if (!f.read(e.off, t, sizeof t)) continue;
        if (u32(t) != 0x10004) {
            logf("ticket %s: signature type %x, not read\n", e.name.c_str(), u32(t));
            continue;
        }
        if (t[0x281] != 0) logf("ticket %s: personalized (its title key is for one console)\n", e.name.c_str());
        out[hex(t + 0x2A0, 16)] = std::vector<uint8_t>(t + 0x180, t + 0x190);
        logf("ticket %s: rights id %s\n", e.name.c_str(), hex(t + 0x2A0, 16).c_str());
    }
}
bool nca_open(Nca &n, File &f, const Entry &e, const Keys &keys, const Tickets &tik, std::string &err) {
    n.f = &f, n.at = e.off, n.size = e.size, n.name = e.name;
    if (e.size < 0xC00 || !f.read(e.off, n.h, 0xC00)) return err = e.name + ": can't read", false;
    xts_dec(keys.get("header_key", 32), n.h, 0xC00, 0, 0x200);
    if (memcmp(n.h + 0x200, "NCA3", 4))
        return err = e.name + ": not an NCA3 (wrong header_key?)", false;
    n.type = n.h[0x205];
    n.program = u64(n.h + 0x210);
    n.keygen = std::max(n.h[0x206], n.h[0x220]);
    int gen = n.keygen ? n.keygen - 1 : 0;
    uint8_t key[16];
    bool rights = false;
    for (int i = 0; i < 16; i++) rights |= n.h[0x230 + i] != 0;
    if (rights) {
        auto it = tik.find(hex(n.h + 0x230, 16));
        if (it == tik.end()) return err = e.name + ": no ticket for rights id " + hex(n.h + 0x230, 16), false;
        const uint8_t *kek = keys.get(Keys::idx("titlekek", gen), 16);
        if (!kek) return err = "keys: no " + Keys::idx("titlekek", gen), false;
        ecb_dec(kek, it->second.data(), key, 16);
    } else {
        if (n.h[0x207] != 0) return err = e.name + ": key area index " + std::to_string(n.h[0x207]) + " (not an application's)", false;
        const uint8_t *kak = keys.get(Keys::idx("key_area_key_application", gen), 16);
        if (!kak) return err = "keys: no " + Keys::idx("key_area_key_application", gen), false;
        uint8_t area[0x40];
        ecb_dec(kak, n.h + 0x300, area, 0x40);
        memcpy(key, area + 0x20, 16);  // (the third: AES-CTR)
    }
    logf("%s: type %d, program %016llx, key generation %d, %s\n", e.name.c_str(), n.type, (unsigned long long)n.program,
         n.keygen, rights ? "title key" : "key area");
    for (int i = 0; i < 4; i++) {
        Section &c = n.s[i];
        c.start = (uint64_t)u32(n.h + 0x240 + 16 * i) * 0x200, c.end = (uint64_t)u32(n.h + 0x244 + 16 * i) * 0x200;
        if (c.end <= c.start || c.end > e.size) continue;
        c.present = true;
        memcpy(c.h, n.h + 0x400 + 0x200 * i, 0x200);
        c.fs_type = c.h[2], c.enc = c.h[4];
        for (int j = 0; j < 8; j++) c.hi[j] = c.h[0x147 - j];
        c.aes.set(key);
        logf("  section %d: %llx-%llx fs %d hash %d encryption %d\n", i, (unsigned long long)c.start,
             (unsigned long long)c.end, c.fs_type, c.h[3], c.enc);
        if (c.enc != 1 && c.enc != 3 && c.enc != 4) return err = e.name + ": section encryption " + std::to_string(c.enc) + " not read", false;
    }
    return true;
}
// The program NCA of a package: the game's (its RomFS whole), or the update's (want_patch: its RomFS a patch). (A game
// card can carry an update beside the game.)
bool find_program(File &f, std::vector<Entry> &files, const Keys &keys, Nca &out, bool want_patch, std::string &err) {
    Tickets tik;
    tickets(f, files, tik);
    std::string last;
    for (const Entry &e : files) {
        if (e.name.size() < 4 || e.name.compare(e.name.size() - 4, 4, ".nca")) continue;
        if (e.name.size() > 9 && e.name.compare(e.name.size() - 9, 9, ".cnmt.nca") == 0) continue;
        std::string why;
        if (!nca_open(out, f, e, keys, tik, why)) {
            logf("%s\n", why.c_str());
            last = why;
            continue;
        }
        if (out.type != 0) continue;
        bool patch = false;
        for (const Section &c : out.s) patch |= c.present && c.fs_type == 0 && c.enc == 4;
        if (patch == want_patch) return true;
        last = f.path + (want_patch ? ": a game, not its update" : ": an update, not the game");
    }
    err = last.empty() ? f.path + ": no program in it" : last;
    return false;
}

// ------------------------------------------------------------------ the RomFS: the game's, or patched by the update
using Reader = std::function<bool(uint64_t off, uint8_t *buf, size_t n)>;
struct Patch {
    Nca *base, *upd;
    int bsec, usec;
    struct Reloc { uint64_t virt, phys; uint32_t patch; };
    struct Sub { uint64_t off; uint32_t ctr; };
    std::vector<Reloc> rel;
    std::vector<Sub> sub;
    uint64_t vsize = 0, psize = 0;

    // a BucketTree table of the update's section (offset, size): its entries, stride bytes each
    bool table(uint64_t off, uint64_t size, size_t stride, std::vector<std::vector<uint8_t>> &entries, uint64_t &total) {
        std::vector<uint8_t> t((size_t)size);
        if (size < 0x4000 || !upd->read(usec, off, t.data(), t.size())) return false;
        uint32_t buckets = u32(t.data() + 4);
        total = u64(t.data() + 8);
        for (uint32_t b = 0; b < buckets; b++) {
            size_t at = 0x4000 + (size_t)0x4000 * b;
            if (at + 0x4000 > t.size()) return false;
            uint32_t n = u32(t.data() + at + 4);
            if (0x10 + (size_t)n * stride > 0x4000) return false;
            for (uint32_t i = 0; i < n; i++) {
                const uint8_t *e = t.data() + at + 0x10 + stride * i;
                entries.emplace_back(e, e + stride);
            }
        }
        return true;
    }
    bool load(std::string &err) {
        const uint8_t *pi = upd->s[usec].h + 0x100;
        if (memcmp(pi + 0x10, "BKTR", 4) || memcmp(pi + 0x30, "BKTR", 4)) return err = "update: no patch tables (BKTR)", false;
        std::vector<std::vector<uint8_t>> e;
        if (!table(u64(pi), u64(pi + 8), 0x14, e, vsize)) return err = "update: can't read its Indirect table", false;
        for (auto &x : e) rel.push_back({u64(x.data()), u64(x.data() + 8), u32(x.data() + 16)});
        e.clear();
        if (!table(u64(pi + 0x20), u64(pi + 0x28), 0x10, e, psize)) return err = "update: can't read its AesCtrEx table", false;
        for (auto &x : e) sub.push_back({u64(x.data()), u32(x.data() + 12)});
        logf("patch: %d relocations over %llx bytes, %d subsections over %llx\n", (int)rel.size(),
             (unsigned long long)vsize, (int)sub.size(), (unsigned long long)psize);
        if (rel.empty() || sub.empty() || rel[0].virt != 0) return err = "update: empty patch tables", false;
        return true;
    }
    bool read_patch(uint64_t off, uint8_t *buf, size_t n) {
        while (n) {
            auto it = std::upper_bound(sub.begin(), sub.end(), off, [](uint64_t o, const Sub &s) { return o < s.off; });
            if (it == sub.begin()) return false;
            const Sub &s = *(it - 1);
            uint64_t end = it == sub.end() ? upd->s[usec].end - upd->s[usec].start : it->off;
            size_t chunk = (size_t)std::min<uint64_t>(n, end - off);
            if (!chunk) return false;
            uint8_t hi[8];
            memcpy(hi, upd->s[usec].hi, 8);
            for (int j = 0; j < 4; j++) hi[4 + j] = (uint8_t)(s.ctr >> (8 * (3 - j)));
            if (!upd->read(usec, off, buf, chunk, hi)) return false;
            off += chunk, buf += chunk, n -= chunk;
        }
        return true;
    }
    bool read(uint64_t off, uint8_t *buf, size_t n) {
        while (n) {
            auto it = std::upper_bound(rel.begin(), rel.end(), off, [](uint64_t o, const Reloc &r) { return o < r.virt; });
            if (it == rel.begin()) return false;
            const Reloc &r = *(it - 1);
            uint64_t end = it == rel.end() ? vsize : it->virt;
            if (off >= end) return false;
            size_t chunk = (size_t)std::min<uint64_t>(n, end - off);
            uint64_t phys = r.phys + (off - r.virt);
            if (!(r.patch ? read_patch(phys, buf, chunk) : base->read(bsec, phys, buf, chunk))) return false;
            off += chunk, buf += chunk, n -= chunk;
        }
        return true;
    }
};

struct RomFile {
    std::string path;
    uint64_t off, size;
};
// The RomFS (its data at `at` in the reader): every file, path and where its data is
bool romfs_list(const Reader &rd, uint64_t at, std::vector<RomFile> &out, std::string &err) {
    uint8_t h[0x50];
    if (!rd(at, h, sizeof h) || u64(h) != 0x50) return err = "romfs: bad header", false;
    uint64_t dmo = u64(h + 0x18), dms = u64(h + 0x20), fmo = u64(h + 0x38), fms = u64(h + 0x40), data = u64(h + 0x48);
    std::vector<uint8_t> dm((size_t)dms), fm((size_t)fms);
    if (!rd(at + dmo, dm.data(), dm.size()) || !rd(at + fmo, fm.data(), fm.size())) return err = "romfs: can't read its tables", false;
    std::function<bool(uint32_t, const std::string &, int)> walk = [&](uint32_t d, const std::string &prefix, int depth) {
        if (depth > 64 || (size_t)d + 0x18 > dm.size()) return false;
        uint32_t child = u32(dm.data() + d + 8), file = u32(dm.data() + d + 12);
        for (uint32_t fi = file; fi != 0xFFFFFFFF;) {
            if ((size_t)fi + 0x20 > fm.size()) return false;
            const uint8_t *e = fm.data() + fi;
            uint32_t nl = u32(e + 0x1c);
            if ((size_t)fi + 0x20 + nl > fm.size()) return false;
            out.push_back({prefix + std::string((const char *)e + 0x20, nl), at + data + u64(e + 8), u64(e + 0x10)});
            fi = u32(e + 4);
        }
        for (uint32_t ci = child; ci != 0xFFFFFFFF;) {
            if ((size_t)ci + 0x18 > dm.size()) return false;
            const uint8_t *e = dm.data() + ci;
            uint32_t nl = u32(e + 0x14);
            if ((size_t)ci + 0x18 + nl > dm.size()) return false;
            if (!walk(ci, prefix + std::string((const char *)e + 0x18, nl) + "/", depth + 1)) return false;
            ci = u32(e + 4);
        }
        return true;
    };
    if (!walk(0, "", 0)) return err = "romfs: broken directory tables", false;
    return true;
}

// ------------------------------------------------------------------ writing
bool safe_rel(const std::string &p) {
    if (p.empty() || p[0] == '/' || p.find(':') != std::string::npos) return false;
    size_t s = 0;
    while (s <= p.size()) {
        size_t e = p.find('/', s);
        if (e == std::string::npos) e = p.size();
        std::string part = p.substr(s, e - s);
        if (part == "..") return false;
        s = e + 1;
    }
    return true;
}
void make_parent(const std::string &path) {
    size_t s = path.rfind('/');
    if (s != std::string::npos) SDL_CreateDirectory(path.substr(0, s).c_str());
}
SDL_EnumerationResult SDLCALL list_cb(void *u, const char *, const char *name) {
    ((std::vector<std::string> *)u)->push_back(name);
    return SDL_ENUM_CONTINUE;
}
bool remove_tree(const std::string &p) {
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(p.c_str(), &info)) return true;
    if (info.type == SDL_PATHTYPE_DIRECTORY) {
        std::vector<std::string> names;
        SDL_EnumerateDirectory(p.c_str(), list_cb, &names);
        for (const std::string &n : names) remove_tree(p + "/" + n);
    }
    return SDL_RemovePath(p.c_str());
}
bool write_from(InstallJob *job, const std::string &path, const Reader &rd, uint64_t off, uint64_t size, std::string &err) {
    make_parent(path);
    SDL_IOStream *o = SDL_IOFromFile(path.c_str(), "wb");
    if (!o) return err = "can't write " + path + ": " + SDL_GetError(), false;
    static thread_local std::vector<uint8_t> buf(4 << 20);
    bool ok = true;
    while (size && ok) {
        if (job->cancel) { err = "cancelled"; ok = false; break; }
        size_t n = (size_t)std::min<uint64_t>(size, buf.size());
        if (!rd(off, buf.data(), n)) { err = "can't read the data for " + path; ok = false; break; }
        if (SDL_WriteIO(o, buf.data(), n) != n) { err = "can't write " + path + " (disk full?)"; ok = false; break; }
        off += n, size -= n;
        job->done += n;
    }
    if (!SDL_CloseIO(o) && ok) err = "can't write " + path, ok = false;
    return ok;
}

// ------------------------------------------------------------------ the Korean patch (.zip)
#ifdef _WIN32
std::string from_cp949(const std::string &s) {  // (names in a zip made on a Korean Windows, without the UTF-8 flag)
    int n = MultiByteToWideChar(949, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(949, 0, s.data(), (int)s.size(), &w[0], n);
    int m = WideCharToMultiByte(CP_UTF8, 0, w.data(), n, nullptr, 0, nullptr, nullptr);
    std::string out(m, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), n, &out[0], m, nullptr, nullptr);
    return out;
}
#endif
struct ZipEntry {
    std::string name;
    uint32_t method, csize, usize, local;
};
bool unzip(InstallJob *job, const std::string &zip, const std::string &mods, std::string &err, bool count_only,
           uint64_t &total) {
    File f;
    if (!f.open(zip)) return err = "can't open " + zip, false;
    size_t tail = (size_t)std::min<uint64_t>(f.size, 0x10000 + 22);
    std::vector<uint8_t> t(tail);
    if (!f.read(f.size - tail, t.data(), tail)) return err = "can't read " + zip, false;
    size_t eocd = std::string::npos;
    for (size_t i = tail >= 22 ? tail - 22 : 0; i + 1 > 0 && i < tail; i--) {
        if (u32(t.data() + i) == 0x06054b50) { eocd = i; break; }
        if (!i) break;
    }
    if (eocd == std::string::npos) return err = zip + ": not a zip (7z / rar: unpack it, then zip it)", false;
    uint16_t count;
    memcpy(&count, t.data() + eocd + 10, 2);
    uint32_t cd_size = u32(t.data() + eocd + 12), cd_off = u32(t.data() + eocd + 16);
    std::vector<uint8_t> cd(cd_size);
    if (!f.read(cd_off, cd.data(), cd.size())) return err = zip + ": can't read its directory", false;
    std::vector<ZipEntry> es;
    for (size_t p = 0, i = 0; i < count; i++) {
        if (p + 46 > cd.size() || u32(cd.data() + p) != 0x02014b50) return err = zip + ": broken directory", false;
        uint16_t flags, method, nl, el, cl;
        memcpy(&flags, cd.data() + p + 8, 2), memcpy(&method, cd.data() + p + 10, 2);
        memcpy(&nl, cd.data() + p + 28, 2), memcpy(&el, cd.data() + p + 30, 2), memcpy(&cl, cd.data() + p + 32, 2);
        if (p + 46 + nl > cd.size()) return err = zip + ": broken directory", false;
        std::string name((const char *)cd.data() + p + 46, nl);
#ifdef _WIN32
        if (!(flags & 0x800)) name = from_cp949(name);
#endif
        for (char &c : name)
            if (c == '\\') c = '/';
        if (flags & 1) return err = zip + ": has a password", false;
        uint32_t cs = u32(cd.data() + p + 20), us = u32(cd.data() + p + 24), lo = u32(cd.data() + p + 42);
        if (cs == 0xFFFFFFFF || us == 0xFFFFFFFF || lo == 0xFFFFFFFF) return err = zip + ": zip64 (over 4 GB) not read", false;
        if (!name.empty() && name.back() != '/') {
            if (method != 0 && method != 8) return err = zip + ": " + name + " packed with method " + std::to_string(method), false;
            if (!safe_rel(name)) return err = zip + ": bad path " + name, false;
            es.push_back({name, method, cs, us, lo});
        }
        p += 46 + nl + el + cl;
    }
    if (count_only) {
        for (auto &e : es) total += e.usize;
        return true;
    }
    // one folder in mods/, named as the zip (its files under it as they are in the zip, any layout: the game finds
    // the romfs / exefs in it); a zip whose files are all in a folder of that same name: that folder
    std::string stem = zip.substr(zip.find_last_of("/\\") + 1);
    if (stem.size() > 4) stem.resize(stem.size() - 4);
    bool one = !es.empty();
    for (auto &e : es) one &= e.name.compare(0, stem.size() + 1, stem + "/") == 0;
    std::string root = mods + "/" + stem;
    remove_tree(root);
    SDL_CreateDirectory(root.c_str());
    logf("patch: %s -> %s, %d files\n", zip.c_str(), root.c_str(), (int)es.size());
    std::vector<uint8_t> in(1 << 20), out(4 << 20);
    for (auto &e : es) {
        std::string path = one ? mods + "/" + e.name : root + "/" + e.name;
        {
            std::lock_guard<std::mutex> g(job->lock);
            job->current = e.name;
        }
        uint8_t lh[30];
        if (!f.read(e.local, lh, 30) || u32(lh) != 0x04034b50) return err = zip + ": broken entry " + e.name, false;
        uint16_t nl, el;
        memcpy(&nl, lh + 26, 2), memcpy(&el, lh + 28, 2);
        uint64_t at = (uint64_t)e.local + 30 + nl + el;
        make_parent(path);
        SDL_IOStream *o = SDL_IOFromFile(path.c_str(), "wb");
        if (!o) return err = "can't write " + path, false;
        bool ok = true;
        if (e.method == 0) {
            uint64_t left = e.csize;
            while (left && ok) {
                size_t n = (size_t)std::min<uint64_t>(left, out.size());
                ok = f.read(at, out.data(), n) && SDL_WriteIO(o, out.data(), n) == n;
                at += n, left -= n, job->done += n;
            }
        } else {
            z_stream z = {};
            inflateInit2(&z, -15);
            uint64_t left = e.csize;
            int r = Z_OK;
            while (ok && r != Z_STREAM_END) {
                if (!z.avail_in && left) {
                    size_t n = (size_t)std::min<uint64_t>(left, in.size());
                    if (!f.read(at, in.data(), n)) { ok = false; break; }
                    at += n, left -= n;
                    z.next_in = in.data(), z.avail_in = (uInt)n;
                }
                z.next_out = out.data(), z.avail_out = (uInt)out.size();
                r = inflate(&z, Z_NO_FLUSH);
                if (r != Z_OK && r != Z_STREAM_END) { ok = false; break; }
                size_t got = out.size() - z.avail_out;
                if (SDL_WriteIO(o, out.data(), got) != got) ok = false;
                job->done += got;
                if (r == Z_OK && !z.avail_in && !left && got == 0) { ok = false; break; }  // (truncated)
            }
            inflateEnd(&z);
        }
        SDL_CloseIO(o);
        if (!ok) return err = zip + ": can't unpack " + e.name, false;
    }
    return true;
}

void fail(InstallJob *job, const std::string &why) {
    logf("failed: %s\n", why.c_str());
    std::lock_guard<std::mutex> g(job->lock);
    job->error = why;
}

}  // namespace

bool install_mod_zip(const std::string &zip, const std::string &mods, std::string &name, std::string &err) {
    InstallJob job;
    uint64_t total = 0;
    if (!unzip(&job, zip, mods, err, true, total)) return false;
    name = zip.substr(zip.find_last_of("/\\") + 1);
    if (name.size() > 4) name.resize(name.size() - 4);
    SDL_CreateDirectory(mods.c_str());
    if (unzip(&job, zip, mods, err, false, total)) return true;
    remove_tree(mods + "/" + name);  // (half of it: gone)
    return false;
}

std::string install_find_keys() {
    const char *appdata = SDL_getenv("APPDATA");
    std::vector<std::string> c;
    if (appdata)
        for (const char *d : {"eden", "yuzu", "suyu", "citron", "sudachi", "torzu"})
            c.push_back(std::string(appdata) + "/" + d + "/keys/prod.keys");
    if (appdata) c.push_back(std::string(appdata) + "/Ryujinx/system/prod.keys");
    if (const char *home = SDL_getenv("HOME")) {
        c.push_back(std::string(home) + "/.local/share/eden/keys/prod.keys");
        c.push_back(std::string(home) + "/.local/share/yuzu/keys/prod.keys");
        c.push_back(std::string(home) + "/.switch/prod.keys");
    }
    for (const std::string &p : c)
        if (SDL_GetPathInfo(p.c_str(), nullptr)) return p;
    return "";
}

void install_run(InstallJob *job) {
    g_log = SDL_IOFromFile("install.log", "w");
    std::string err;
    std::string ex_new = job->game_dir + "/exefs.new", rom_new = job->game_dir + "/romfs.new";
    auto finish = [&](bool ok) {
        job->ok = ok;
        job->stage = ok ? INSTALL_DONE : job->stage.load();
        if (!ok) remove_tree(ex_new), remove_tree(rom_new);
        if (g_log) SDL_CloseIO(g_log), g_log = nullptr;
        job->finished = true;
    };
    job->stage = INSTALL_KEYS;
    Keys keys;
    if (!keys.load(job->keys, err)) return fail(job, err), finish(false);

    job->stage = INSTALL_BASE;
    File base, upd;
    std::vector<Entry> bfiles, ufiles;
    Nca bn, un;
    if (!base.open(job->base)) return fail(job, "can't open " + job->base), finish(false);
    if (!package(base, bfiles, err) || !find_program(base, bfiles, keys, bn, false, err)) return fail(job, err), finish(false);
    job->stage = INSTALL_UPDATE;
    if (!upd.open(job->update)) return fail(job, "can't open " + job->update), finish(false);
    if (!package(upd, ufiles, err) || !find_program(upd, ufiles, keys, un, true, err)) return fail(job, err), finish(false);
    if (un.program != bn.program)
        return fail(job, "the update is for another game (" + hex((uint8_t *)&un.program, 8) + ")"), finish(false);
    int bex = -1, brom = -1, uex = -1, urom = -1;
    for (int i = 0; i < 4; i++) {
        if (bn.s[i].present && bn.s[i].fs_type == 1 && bex < 0) bex = i;
        if (bn.s[i].present && bn.s[i].fs_type == 0 && brom < 0) brom = i;
        if (un.s[i].present && un.s[i].fs_type == 1 && uex < 0) uex = i;
        if (un.s[i].present && un.s[i].fs_type == 0 && urom < 0) urom = i;
    }
    if (brom < 0) return fail(job, "the game has no RomFS (is it the base game?)"), finish(false);
    if (uex < 0 || urom < 0 || un.s[urom].enc != 4) return fail(job, "the update file is not an update (no patch RomFS)"), finish(false);
    (void)bex;

    // ExeFS: the update's
    std::vector<Entry> ex;
    uint64_t pfs = u64(un.s[uex].h + 0x40), pfs_size = u64(un.s[uex].h + 0x48);
    uint8_t ph[16];
    Reader exr = [&](uint64_t off, uint8_t *b, size_t n) { return un.read(uex, off, b, n); };
    if (!exr(pfs, ph, 16) || memcmp(ph, "PFS0", 4)) return fail(job, "update: can't read its ExeFS"), finish(false);
    {
        uint32_t n = u32(ph + 4), strings = u32(ph + 8);
        std::vector<uint8_t> t(0x18 * (size_t)n + strings);
        if (n > 64 || !exr(pfs + 16, t.data(), t.size())) return fail(job, "update: bad ExeFS"), finish(false);
        uint64_t data = pfs + 16 + t.size();
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *e = t.data() + 0x18 * i;
            const char *nm = (const char *)t.data() + 0x18 * n + u32(e + 16);
            ex.push_back({nm, data + u64(e), u64(e + 8)});
        }
    }
    (void)pfs_size;

    // RomFS: the game's, patched
    Patch patch{&bn, &un, brom, urom};
    if (!patch.load(err)) return fail(job, err), finish(false);
    Reader rr = [&](uint64_t off, uint8_t *b, size_t n) { return patch.read(off, b, n); };
    uint64_t romfs_at = u64(un.s[urom].h + 0x90);
    std::vector<RomFile> files;
    if (!romfs_list(rr, romfs_at, files, err)) return fail(job, "update: " + err), finish(false);
    logf("romfs: %d files\n", (int)files.size());

    uint64_t total = 0, zip_total = 0;
    for (auto &e : ex) total += e.size;
    for (auto &f : files) total += f.size;
    if (!job->patch.empty() && !unzip(job, job->patch, job->mods_dir, err, true, zip_total)) return fail(job, err), finish(false);
    job->total = total + zip_total;

    remove_tree(ex_new), remove_tree(rom_new);
    job->stage = INSTALL_EXEFS;
    for (auto &e : ex) {
        if (!safe_rel(e.name)) return fail(job, "update: bad ExeFS name " + e.name), finish(false);
        if (!write_from(job, ex_new + "/" + e.name, exr, e.off, e.size, err)) return fail(job, err), finish(false);
    }
    {  // (the main of 1.5.1: its build id)
        uint8_t nso[0x48] = {};
        static const uint8_t want[8] = GAME_MAIN_BUILD_ID;
        SDL_IOStream *m = SDL_IOFromFile((ex_new + "/main").c_str(), "rb");
        size_t got = m ? SDL_ReadIO(m, nso, sizeof nso) : 0;
        if (m) SDL_CloseIO(m);
        logf("main: build id %s\n", hex(nso + 0x40, 8).c_str());
        if (got < sizeof nso || memcmp(nso, "NSO0", 4) || memcmp(nso + 0x40, want, 8))
            return fail(job, "the update is not 1.5.1 (main build id " + hex(nso + 0x40, 8) + ")"), finish(false);
    }
    job->stage = INSTALL_ROMFS;
    for (auto &f : files) {
        if (!safe_rel(f.path)) return fail(job, "romfs: bad name " + f.path), finish(false);
        {
            std::lock_guard<std::mutex> g(job->lock);
            job->current = f.path;
        }
        if (!write_from(job, rom_new + "/" + f.path, rr, f.off, f.size, err)) return fail(job, err), finish(false);
    }
    job->stage = INSTALL_SWAP;
    std::string ex_dir = job->game_dir + "/exefs", rom_dir = job->game_dir + "/romfs";
    std::string ex_old = ex_dir + ".old", rom_old = rom_dir + ".old";  // (the files there before: kept until it worked)
    remove_tree(ex_old), remove_tree(rom_old);
    bool had_ex = SDL_GetPathInfo(ex_dir.c_str(), nullptr), had_rom = SDL_GetPathInfo(rom_dir.c_str(), nullptr);
    bool moved = (!had_ex || SDL_RenamePath(ex_dir.c_str(), ex_old.c_str())) &&
                 (!had_rom || SDL_RenamePath(rom_dir.c_str(), rom_old.c_str()));
    if (!moved || !SDL_RenamePath(ex_new.c_str(), ex_dir.c_str()) || !SDL_RenamePath(rom_new.c_str(), rom_dir.c_str())) {
        std::string why = std::string("can't put the files in place: ") + SDL_GetError();
        remove_tree(ex_dir + ".failed"), SDL_RenamePath(ex_dir.c_str(), (ex_dir + ".failed").c_str());
        remove_tree(rom_dir + ".failed"), SDL_RenamePath(rom_dir.c_str(), (rom_dir + ".failed").c_str());
        if (had_ex) SDL_RenamePath(ex_old.c_str(), ex_dir.c_str());
        if (had_rom) SDL_RenamePath(rom_old.c_str(), rom_dir.c_str());
        remove_tree(ex_dir + ".failed"), remove_tree(rom_dir + ".failed");
        return fail(job, why), finish(false);
    }
    remove_tree(ex_old), remove_tree(rom_old);
    logf("game files written\n");
    if (!job->patch.empty()) {
        job->stage = INSTALL_PATCH;
        SDL_CreateDirectory(job->mods_dir.c_str());
        uint64_t unused = 0;
        if (!unzip(job, job->patch, job->mods_dir, err, false, unused)) return fail(job, err), finish(false);
    }
    logf("done\n");
    finish(true);
}

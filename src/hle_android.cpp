// libandroid AAssetManager backed by the game's OBB, plus the ARM EABI runtime helpers
// the game imports but the bundled FMOD libraries don't export.
//
// OBB = "DAWL" archive, whole file XOR-obfuscated: plain[i] = file[i] ^ (i & 1 ? 0x66 : 0xAF).
// FAT at 0x800: u32 ndirs, ndirs*8 bytes, u32 nfiles, nfiles*48-byte entries
// {u32 crc32(lowercase path, no final xor), 3*u32, u64 offset, u64 size, u64 size, u64 0}.
// (Format documented by the gw3-portmaster project.)
#include "core.hpp"
#include "overlay.hpp"

#include <cmath>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <strings.h>
#include <cstring>

extern std::string g_data_dir;

// Whatever main.<version>.com.activision.gw3.dimensions.obb is in the game folder (35 = 1.0.0, 101 = 1.3.0).
std::string obb_path() {
    static std::string found;
    if (!found.empty()) return found;
    if (DIR* d = opendir(g_data_dir.c_str())) {
        long best = -1;   // newest data version wins (main.101 = 1.3.0 over main.35 = 1.0.0)
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.rfind("main.", 0) == 0 && n.size() > 4 && n.substr(n.size() - 4) == ".obb") {
                long v = atol(n.c_str() + 5);
                if (v > best) { best = v; found = g_data_dir + "/" + n; }
            }
        }
        closedir(d);
    }
    if (found.empty()) found = g_data_dir + "/main.35.com.activision.gw3.dimensions.obb";
    return found;
}

struct WadEntry { uint64_t off, size; };
static std::unordered_map<uint32_t, WadEntry> g_wad;
static int g_wad_fd = -1;

// The whole OBB (~80 MB), already de-obfuscated: thousands of tiny SD reads cost ~70 s on Switch.
static std::vector<uint8_t> g_wad_mem;
static void wad_read(void* dst, uint64_t off, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    if (!g_wad_mem.empty()) {
        size_t ok = off < g_wad_mem.size() ? std::min<uint64_t>(n, g_wad_mem.size() - off) : 0;
        memcpy(d, g_wad_mem.data() + off, ok); memset(d + ok, 0, n - ok);
        return;
    }
    // ponytail: one lock around seek+read (newlib has no pread); fine for the asset loader's few threads
    static std::mutex mx;
    ssize_t got;
    { std::lock_guard<std::mutex> lk(mx); lseek(g_wad_fd, off, SEEK_SET); got = read(g_wad_fd, d, n); }
    if (got < (ssize_t)n) memset(d + (got > 0 ? got : 0), 0, n - (got > 0 ? got : 0));
    for (size_t i = 0; i < n; i++) d[i] ^= ((off + i) & 1) ? 0x66 : 0xAF;
}
static uint32_t wad_hash(const char* s) {
    static uint32_t tab[256];
    if (!tab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; tab[i] = c; }
    uint32_t c = 0xFFFFFFFF;
    for (; *s; s++) c = tab[(c ^ (uint8_t)tolower(*s)) & 0xFF] ^ (c >> 8);
    return c;
}
static void wad_open() {
    std::string p = obb_path();
    g_wad_fd = open(p.c_str(), O_RDONLY);
    if (g_wad_fd < 0) { logf("assets: cannot open %s", p.c_str()); return; }
    logf("assets: using %s", p.c_str());
    {
        off_t sz = lseek(g_wad_fd, 0, SEEK_END);
        std::vector<uint8_t> buf;
        try { buf.resize(sz); } catch (...) { buf.clear(); }
        uint64_t got = 0, t = now_us();
        lseek(g_wad_fd, 0, SEEK_SET);
        while (!buf.empty() && got < buf.size()) { ssize_t r = read(g_wad_fd, buf.data() + got, std::min<uint64_t>(buf.size() - got, 4u << 20)); if (r <= 0) break; got += r; }
        if (!buf.empty() && got == buf.size()) {
            for (size_t i = 0; i < buf.size(); i++) buf[i] ^= (i & 1) ? 0x66 : 0xAF;
            g_wad_mem = std::move(buf);
            logf("assets: OBB loaded into memory (%llu MiB, %llu ms)", (unsigned long long)(got >> 20), (unsigned long long)((now_us() - t) / 1000));
        } else logf("assets: could not load the OBB into memory, reading from the SD card instead");
    }
    char magic[4]; wad_read(magic, 0, 4);
    if (memcmp(magic, "DAWL", 4)) { logf("assets: OBB is not a DAWL archive"); return; }
    uint32_t ndirs; wad_read(&ndirs, 0x800, 4);
    uint64_t o = 0x804 + 8ull * ndirs;
    uint32_t nfiles; wad_read(&nfiles, o, 4);
    std::vector<uint8_t> fat(48ull * nfiles); wad_read(fat.data(), o + 4, fat.size());
    for (uint32_t i = 0; i < nfiles; i++) {
        uint8_t* e = &fat[48ull * i];
        uint32_t h; uint64_t off, size;
        memcpy(&h, e, 4); memcpy(&off, e + 16, 8); memcpy(&size, e + 24, 8);
        g_wad[h] = {off, size};
    }
    logf("assets: OBB has %u files", nfiles);
}

// For host code (the overlay's font): a whole file from the OBB, by its archive path
bool asset_load(const char* path, std::vector<uint8_t>& out) {
    auto it = g_wad.find(wad_hash(path));
    if (it == g_wad.end()) return false;
    out.resize(it->second.size);
    wad_read(out.data(), it->second.off, out.size());
    return true;
}

// guest AAsset* -> host state
struct Asset { uint64_t off, size, pos; FILE* f; const unsigned char* mem; };

// Our own Switch-style prompts (L/R/ZL/ZR) in place of the Xbox LB/RB/LT/RT images (tools/make_prompts.py)
struct Builtin { const char* name; unsigned size; const unsigned char* data; };
static const Builtin kBuiltins[] = {
#include "prompts.inc"
};
static const Builtin* builtin(const char* path) {
    static const bool stock = getenv("GW3_STOCK") != nullptr;
    if (stock) return nullptr;
    const char* base = strrchr(path, '/'); base = base ? base + 1 : path;
    for (const Builtin& b : kBuiltins) if (!strcasecmp(b.name, base)) return &b;
    return nullptr;
}
static std::mutex g_amx;
static std::unordered_map<gaddr, Asset> g_assets;

static void h_fromJava(Ctx& c) { static gaddr mgr = gmalloc(16); c.ret(mgr); }
static void h_open(Ctx& c) {
    const char* name = c.s(1);
    bool absolute = name[0] == '/';
    Asset a{};
    auto it = absolute ? g_wad.end() : g_wad.find(wad_hash(name));
    const Builtin* bi = it != g_wad.end() ? builtin(name) : nullptr;
    const std::string* ov = it != g_wad.end() ? options_asset(name) : nullptr;
    if (ov) { a.mem = (const unsigned char*)ov->data(); a.size = ov->size(); }
    else if (bi) { a.mem = bi->data; a.size = bi->size; }
    else if (it != g_wad.end()) { a.off = it->second.off; a.size = it->second.size; }
    else if (!absolute && name[0] != '.') { c.ret(0); return; }   // not in the OBB: don't hit the SD card for every probe
    else {
        std::string p = absolute ? std::string(name) : g_data_dir + "/" + name;
        a.f = fopen(p.c_str(), "rb");
        if (!a.f) { logf("assets: missing %s", name); c.ret(0); return; }
        fseek(a.f, 0, SEEK_END); a.size = ftell(a.f); fseek(a.f, 0, SEEK_SET);
    }
    static const bool trace = getenv("GW3_TRACE_ASSETS") != nullptr;
    if (trace) logf("assets: open %s (%llu bytes)", name, (unsigned long long)a.size);
    gaddr g = gmalloc(16);
    std::lock_guard<std::mutex> lk(g_amx);
    g_assets[g] = a; c.ret(g);
}
static Asset* A(gaddr g) { std::lock_guard<std::mutex> lk(g_amx); auto it = g_assets.find(g); return it == g_assets.end() ? nullptr : &it->second; }
static void h_read(Ctx& c) {
    struct T { uint64_t t = now_us(); ~T() { g_t_asset_us += now_us() - t; } } timer;
    Asset* a = A(c.u(0)); if (!a) { c.ret(-1); return; }
    uint64_t n = std::min<uint64_t>(c.u(2), a->size - a->pos);
    if (a->mem) memcpy(c.p(1), a->mem + a->pos, n);
    else if (a->f) { fseek(a->f, a->pos, SEEK_SET); n = fread(c.p(1), 1, n, a->f); }
    else wad_read(c.p(1), a->off + a->pos, n);
    a->pos += n; c.ret(n);
}
static void h_seek(Ctx& c) {
    Asset* a = A(c.u(0)); if (!a) { c.ret(-1); return; }
    int64_t o = c.i(1), base = c.i(2) == 1 ? a->pos : c.i(2) == 2 ? a->size : 0;
    if (base + o < 0 || base + o > (int64_t)a->size) { c.ret(-1); return; }
    a->pos = base + o; c.ret(a->pos);
}
static void h_len(Ctx& c) { Asset* a = A(c.u(0)); c.ret(a ? a->size : 0); }
static void h_remain(Ctx& c) { Asset* a = A(c.u(0)); c.ret(a ? a->size - a->pos : 0); }
static void h_close(Ctx& c) {
    std::lock_guard<std::mutex> lk(g_amx);
    auto it = g_assets.find(c.u(0));
    if (it == g_assets.end()) return;
    if (it->second.f) fclose(it->second.f);
    g_assets.erase(it); gfree(c.u(0));
}

// ---- ARM EABI helpers (softfp: floats in r0/r1, doubles in r0:r1 / r2:r3)
static float F(uint32_t w) { float f; memcpy(&f, &w, 4); return f; }
static double D(uint32_t lo, uint32_t hi) { uint64_t w = lo | (uint64_t)hi << 32; double d; memcpy(&d, &w, 8); return d; }
#define FF(op) [](Ctx& c) { c.retf(F(c.u(0)) op F(c.u(1))); }
#define DD(op) [](Ctx& c) { c.retd(D(c.u(0), c.u(1)) op D(c.u(2), c.u(3))); }
#define FC(op) [](Ctx& c) { c.ret(F(c.u(0)) op F(c.u(1))); }
#define DC(op) [](Ctx& c) { c.ret(D(c.u(0), c.u(1)) op D(c.u(2), c.u(3))); }
static uint64_t U64(Ctx& c, int i) { return c.u(i) | (uint64_t)c.u(i + 1) << 32; }

void hle_android_init() {
    wad_open();
    struct { const char* n; HleFn f; } t[] = {
        {"AAssetManager_fromJava", h_fromJava}, {"AAssetManager_open", h_open}, {"AAsset_read", h_read},
        {"AAsset_seek", h_seek}, {"AAsset_getLength", h_len}, {"AAsset_getRemainingLength", h_remain}, {"AAsset_close", h_close},
        {"__aeabi_fadd", FF(+)}, {"__aeabi_fsub", FF(-)}, {"__aeabi_fmul", FF(*)}, {"__aeabi_fdiv", FF(/)},
        {"__aeabi_dadd", DD(+)}, {"__aeabi_dsub", DD(-)}, {"__aeabi_dmul", DD(*)}, {"__aeabi_ddiv", DD(/)},
        {"__aeabi_fcmpeq", FC(==)}, {"__aeabi_fcmplt", FC(<)}, {"__aeabi_fcmple", FC(<=)}, {"__aeabi_fcmpge", FC(>=)}, {"__aeabi_fcmpgt", FC(>)},
        {"__aeabi_dcmpeq", DC(==)}, {"__aeabi_dcmplt", DC(<)}, {"__aeabi_dcmple", DC(<=)}, {"__aeabi_dcmpge", DC(>=)}, {"__aeabi_dcmpgt", DC(>)},
        {"__aeabi_f2d", [](Ctx& c) { c.retd(F(c.u(0))); }}, {"__aeabi_d2f", [](Ctx& c) { c.retf((float)D(c.u(0), c.u(1))); }},
        {"__aeabi_f2iz", [](Ctx& c) { c.ret((int32_t)F(c.u(0))); }}, {"__aeabi_f2uiz", [](Ctx& c) { float f = F(c.u(0)); c.ret(f <= 0 ? 0 : (uint32_t)f); }},
        {"__aeabi_f2lz", [](Ctx& c) { c.ret64((int64_t)F(c.u(0))); }}, {"__aeabi_f2ulz", [](Ctx& c) { float f = F(c.u(0)); c.ret64(f <= 0 ? 0 : (uint64_t)f); }},
        {"__aeabi_d2iz", [](Ctx& c) { c.ret((int32_t)D(c.u(0), c.u(1))); }}, {"__aeabi_d2uiz", [](Ctx& c) { double d = D(c.u(0), c.u(1)); c.ret(d <= 0 ? 0 : (uint32_t)d); }},
        {"__aeabi_d2lz", [](Ctx& c) { c.ret64((int64_t)D(c.u(0), c.u(1))); }}, {"__aeabi_d2ulz", [](Ctx& c) { double d = D(c.u(0), c.u(1)); c.ret64(d <= 0 ? 0 : (uint64_t)d); }},
        {"__aeabi_i2d", [](Ctx& c) { c.retd(c.i(0)); }}, {"__aeabi_ui2d", [](Ctx& c) { c.retd(c.u(0)); }},
        {"__aeabi_i2f", [](Ctx& c) { c.retf((float)c.i(0)); }}, {"__aeabi_ui2f", [](Ctx& c) { c.retf((float)c.u(0)); }},
        {"__aeabi_l2d", [](Ctx& c) { c.retd((double)(int64_t)U64(c, 0)); }}, {"__aeabi_ul2d", [](Ctx& c) { c.retd((double)U64(c, 0)); }},
        {"__aeabi_l2f", [](Ctx& c) { c.retf((float)(int64_t)U64(c, 0)); }}, {"__aeabi_ul2f", [](Ctx& c) { c.retf((float)U64(c, 0)); }},
        {"__aeabi_idiv", [](Ctx& c) { c.ret(c.i(1) ? c.i(0) / c.i(1) : 0); }},
        {"__aeabi_uidiv", [](Ctx& c) { c.ret(c.u(1) ? c.u(0) / c.u(1) : 0); }},
        {"__aeabi_idivmod", [](Ctx& c) { int32_t a = c.i(0), b = c.i(1); if (!b) { c.ret64(0); return; } reg(0) = a / b; reg(1) = a % b; }},
        {"__aeabi_uidivmod", [](Ctx& c) { uint32_t a = c.u(0), b = c.u(1); if (!b) { c.ret64(0); return; } reg(0) = a / b; reg(1) = a % b; }},
        {"__aeabi_ldivmod", [](Ctx& c) { int64_t a = U64(c, 0), b = U64(c, 2); int64_t q = b ? a / b : 0, r = b ? a % b : 0; c.ret64(q); reg(2) = (uint32_t)r; reg(3) = (uint32_t)((uint64_t)r >> 32); }},
        {"__aeabi_uldivmod", [](Ctx& c) { uint64_t a = U64(c, 0), b = U64(c, 2); uint64_t q = b ? a / b : 0, r = b ? a % b : 0; c.ret64(q); reg(2) = (uint32_t)r; reg(3) = (uint32_t)(r >> 32); }},
        {"__aeabi_ldiv0", [](Ctx& c) { c.ret(0); }}, {"__aeabi_idiv0", [](Ctx& c) { c.ret(0); }},
        {"__gnu_ldivmod_helper", [](Ctx& c) {  // (a, b, int64* rem) -> a / b
            int64_t a = U64(c, 0), b = U64(c, 2); gaddr rp = c.word(4);
            int64_t q = b ? a / b : 0; if (rp) gref<int64_t>(rp) = b ? a % b : 0; c.ret64(q); }},
    };
    for (auto& e : t) hle_register(e.n, e.f);
}

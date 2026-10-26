// Host implementations of the bionic libc / libm / libdl / liblog / pthread functions the game imports.
#include "core.hpp"

#include <cerrno>
#include <cmath>
#include <cctype>
#include <cwchar>
#include <cwctype>
#include <ctime>
#include <condition_variable>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <chrono>
#include <thread>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <utime.h>

std::string g_data_dir = ".";   // set by main: folder with the OBB and save files

// Android paths -> host. The game mostly builds paths from the dirs we hand it,
// but the OBB is looked up under Android/obb/<package>/.
std::string map_path(const char* p) {
    std::string s = p ? p : "";
    if (s == "/proc/cpuinfo") {  // FMOD checks for ARMv7 + VFP/NEON
        std::string f = g_data_dir + "/cpuinfo.txt";
        if (FILE* t = fopen(f.c_str(), "wx")) {
            fputs("Processor\t: ARMv7 Processor rev 1 (v7l)\nprocessor\t: 0\nFeatures\t: swp half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt\n"
                  "CPU implementer\t: 0x41\nCPU architecture: 7\nCPU variant\t: 0x1\nCPU part\t: 0xd07\nCPU revision\t: 1\n\nHardware\t: Nintendo Switch\n", t);
            fclose(t);
        }
        return f;
    }
    if (s.find("/obb/") != std::string::npos) return g_data_dir + "/" + s.substr(s.rfind('/') + 1);
    return s;
}

// ------------------------------------------------------------------ errno / misc
void hle_sync_errno() { if (errno) gref(cur_tls() + 8) = errno; }
static void h_errno(Ctx& c) { c.ret(cur_tls() + 8); }

// ------------------------------------------------------------------ printf engine
// Reads printf-style arguments from either the call registers/stack or a guest va_list.
struct ArgSrc {
    Ctx* c = nullptr; gaddr va = 0;
    uint32_t n32() { if (c) return c->next(); uint32_t v = gref(va); va += 4; return v; }
    uint64_t n64() {
        if (c) return c->next64();
        va = (va + 7) & ~7u; uint64_t v = gref<uint64_t>(va); va += 8; return v;
    }
};

static std::string gformat(const char* f, ArgSrc& a) {
    std::string out;
    char spec[64], buf[512];
    while (*f) {
        if (*f != '%') { out += *f++; continue; }
        const char* st = f++;
        if (*f == '%') { out += '%'; f++; continue; }
        std::string flags;
        while (strchr("-+ #0'", *f)) flags += *f++;
        int width = -1, prec = -1;
        if (*f == '*') { width = (int32_t)a.n32(); f++; } else if (isdigit(*f)) width = strtol(f, (char**)&f, 10);
        if (*f == '.') { f++; if (*f == '*') { prec = (int32_t)a.n32(); f++; } else prec = strtol(f, (char**)&f, 10); }
        int lng = 0;  // 1 = l, 2 = ll / j / q / L
        bool h = false;
        while (strchr("hlLqjzt", *f)) {
            if (*f == 'l') lng++; else if (*f == 'L' || *f == 'q' || *f == 'j') lng = 2; else if (*f == 'h') h = true;
            f++;
        }
        char conv = *f++;
        std::string sp = "%" + flags;
        if (width >= 0) sp += std::to_string(width);
        if (prec >= 0) sp += "." + std::to_string(prec);
        switch (conv) {
        case 'd': case 'i': {
            long long v = lng >= 2 ? (long long)a.n64() : (long long)(int32_t)a.n32();
            snprintf(spec, sizeof spec, "%sll%c", sp.c_str(), conv); snprintf(buf, sizeof buf, spec, v); out += buf; break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            unsigned long long v = lng >= 2 ? a.n64() : a.n32();
            if (h) v &= 0xFFFF;
            snprintf(spec, sizeof spec, "%sll%c", sp.c_str(), conv); snprintf(buf, sizeof buf, spec, v); out += buf; break;
        }
        case 'c': snprintf(spec, sizeof spec, "%sc", sp.c_str()); snprintf(buf, sizeof buf, spec, (int)a.n32()); out += buf; break;
        case 'p': snprintf(buf, sizeof buf, "0x%x", a.n32()); out += buf; break;
        case 's': {
            gaddr s = a.n32();
            snprintf(spec, sizeof spec, "%ss", sp.c_str());
            int n = snprintf(nullptr, 0, spec, s ? gstr(s) : "(null)");
            std::string tmp(n + 1, 0); snprintf(tmp.data(), n + 1, spec, s ? gstr(s) : "(null)");
            out += tmp.c_str(); break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            uint64_t w = a.n64(); double d; memcpy(&d, &w, 8);
            snprintf(spec, sizeof spec, "%s%c", sp.c_str(), conv); snprintf(buf, sizeof buf, spec, d); out += buf; break;
        }
        case 'n': { gaddr p = a.n32(); if (p) gref(p) = out.size(); break; }
        default: out.append(st, f); break;
        }
    }
    return out;
}
static uint32_t put_out(gaddr dst, uint32_t cap, const std::string& s) {
    if (dst && cap) { uint32_t n = std::min<uint32_t>(cap - 1, s.size()); memcpy(g2h(dst), s.data(), n); gref<char>(dst + n) = 0; }
    return s.size();
}

// ------------------------------------------------------------------ stdio
static constexpr uint32_t BIONIC_FILE_SIZE = 84;
static gaddr g_sF;
static std::mutex g_files_mx;
static std::unordered_map<gaddr, FILE*> g_files;
static FILE* F(gaddr g) {
    std::lock_guard<std::mutex> lk(g_files_mx);
    auto it = g_files.find(g);
    return it == g_files.end() ? nullptr : it->second;
}
static void h_fopen(Ctx& c) {
    std::string p = map_path(c.s(0));
    FILE* f = fopen(p.c_str(), c.s(1));
    logf("fopen(%s, %s) -> %s", c.s(0), c.s(1), f ? "ok" : strerror(errno));
    if (!f) { gref(cur_tls() + 8) = errno; c.ret(0); return; }
    gaddr g = gmalloc(BIONIC_FILE_SIZE);
    std::lock_guard<std::mutex> lk(g_files_mx);
    g_files[g] = f; c.ret(g);
}
static void h_fdopen(Ctx& c) {
    FILE* f = fdopen(c.i(0), c.s(1));
    if (!f) { c.ret(0); return; }
    gaddr g = gmalloc(BIONIC_FILE_SIZE);
    std::lock_guard<std::mutex> lk(g_files_mx);
    g_files[g] = f; c.ret(g);
}
static void h_freopen(Ctx& c) { c.ret(0); }
static void h_fclose(Ctx& c) {
    FILE* f = F(c.u(0));
    if (!f || c.u(0) < g_sF + 3 * BIONIC_FILE_SIZE) { c.ret(0); return; }
    { std::lock_guard<std::mutex> lk(g_files_mx); g_files.erase(c.u(0)); }
    gfree(c.u(0)); c.ret(fclose(f));
}
#define FWRAP(expr) do { FILE* fp = F(c.u(FI)); if (!fp) { c.ret(-1); return; } expr; } while (0)
static void h_fread(Ctx& c) { const int FI = 3; FWRAP(c.ret(fread(c.p(0), c.u(1), c.u(2), fp))); }
static void h_fwrite(Ctx& c) { const int FI = 3; FWRAP(c.ret(fwrite(c.p(0), c.u(1), c.u(2), fp))); }
static void h_fseek(Ctx& c) { const int FI = 0; FWRAP(c.ret(fseek(fp, c.i(1), c.i(2)))); }
static void h_ftell(Ctx& c) { const int FI = 0; FWRAP(c.ret(ftell(fp))); }
static void h_fflush(Ctx& c) { if (!c.u(0)) { fflush(nullptr); c.ret(0); return; } const int FI = 0; FWRAP(c.ret(fflush(fp))); }
static void h_fgets(Ctx& c) { const int FI = 2; FWRAP(c.ret(fgets((char*)c.p(0), c.i(1), fp) ? c.u(0) : 0)); }
static void h_fputs(Ctx& c) { const int FI = 1; FWRAP(c.ret(fputs(c.s(0), fp))); }
static void h_fputc(Ctx& c) { const int FI = 1; FWRAP(c.ret(fputc(c.i(0), fp))); }
static void h_getc(Ctx& c) { const int FI = 0; FWRAP(c.ret(getc(fp))); }
static void h_ungetc(Ctx& c) { const int FI = 1; FWRAP(c.ret(ungetc(c.i(0), fp))); }
static void h_setvbuf(Ctx& c) { c.ret(0); }
static void h_fprintf(Ctx& c) {
    c.slot = 2; ArgSrc a{&c}; std::string s = gformat(c.s(1), a);
    FILE* fp = F(c.u(0)); c.ret(fp ? fwrite(s.data(), 1, s.size(), fp) : 0);
}
static void h_vfprintf(Ctx& c) {
    ArgSrc a; a.va = c.u(2); std::string s = gformat(c.s(1), a);
    FILE* fp = F(c.u(0)); c.ret(fp ? fwrite(s.data(), 1, s.size(), fp) : 0);
}
static void h_printf(Ctx& c) { c.slot = 1; ArgSrc a{&c}; std::string s = gformat(c.s(0), a); fputs(s.c_str(), stdout); c.ret(s.size()); }
static void h_puts(Ctx& c) { puts(c.s(0)); c.ret(0); }
static void h_putchar(Ctx& c) { c.ret(putchar(c.i(0))); }
static void h_perror(Ctx& c) { logf("perror: %s", c.s(0)); c.ret(0); }
static void h_sprintf(Ctx& c) { c.slot = 2; ArgSrc a{&c}; c.ret(put_out(c.u(0), 0x7FFFFFFF, gformat(c.s(1), a))); }
static void h_snprintf(Ctx& c) { c.slot = 3; ArgSrc a{&c}; c.ret(put_out(c.u(0), c.u(1), gformat(c.s(2), a))); }
static void h_vsprintf(Ctx& c) { ArgSrc a; a.va = c.u(2); c.ret(put_out(c.u(0), 0x7FFFFFFF, gformat(c.s(1), a))); }
static void h_vsnprintf(Ctx& c) { ArgSrc a; a.va = c.u(3); c.ret(put_out(c.u(0), c.u(1), gformat(c.s(2), a))); }

// sscanf: run each conversion through the host sscanf, writing results at guest sizes.
static void h_sscanf(Ctx& c) {
    const char* in = c.s(0); const char* f = c.s(1);
    c.slot = 2;
    int assigned = 0;
    while (*f) {
        if (isspace(*f)) { while (isspace(*in)) in++; f++; continue; }
        if (*f != '%') { if (*in != *f) break; in++; f++; continue; }
        const char* st = f++;
        if (*f == '%') { if (*in != '%') break; in++; f++; continue; }
        bool skip = false; if (*f == '*') { skip = true; f++; }
        while (isdigit(*f)) f++;
        int lng = 0; bool hh = false, h = false;
        while (strchr("hlLqjz", *f)) { if (*f == 'l') lng++; else if (*f == 'h') { hh = h; h = true; } else lng = 2; f++; }
        char conv = *f;
        if (conv == '[') { while (*f && *f != ']') f++; if (f[1] == ']') { f++; while (*f && *f != ']') f++; } }
        f++;
        std::string spec(st, f);
        spec.erase(std::remove_if(spec.begin() + 1, spec.end(), [](char ch) { return ch == 'h' || ch == 'l' || ch == 'L' || ch == 'q' || ch == 'j' || ch == 'z'; }), spec.end());
        spec += "%n";
        int used = 0, got = 0;
        if (conv == 'n') { if (!skip) gref(c.next()) = in - c.s(0); continue; }
        if (strchr("diuxXo", conv)) {
            long long v = 0; std::string s2 = spec; s2.insert(s2.size() - 3, "ll");
            if (skip) { s2.erase(1, 1); s2.insert(1, "*"); got = sscanf(in, s2.c_str(), &used) == 0 ? 1 : 0; if (!used) break; in += used; continue; }
            got = sscanf(in, s2.c_str(), &v, &used);
            if (got != 1) break;
            gaddr p = c.next();
            if (lng >= 2) gref<uint64_t>(p) = v; else if (hh) gref<uint8_t>(p) = v; else if (h) gref<uint16_t>(p) = v; else gref(p) = (uint32_t)v;
        } else if (strchr("feEgGa", conv)) {
            double v = 0; std::string s2 = spec; s2.insert(s2.size() - 3, "l");
            if (skip) { if (sscanf(in, "%*lf%n", &used), !used) break; in += used; continue; }
            got = sscanf(in, s2.c_str(), &v, &used);
            if (got != 1) break;
            gaddr p = c.next();
            if (lng) gref<double>(p) = v; else gref<float>(p) = (float)v;
        } else {  // s c [
            if (skip) { if (sscanf(in, spec.c_str(), &used), !used) break; in += used; continue; }
            got = sscanf(in, spec.c_str(), (char*)g2h(c.next()), &used);
            if (got != 1) break;
        }
        in += used; assigned++;
    }
    c.ret(assigned);
}

// ------------------------------------------------------------------ strings needing pointer care
static void h_strdup(Ctx& c) { c.ret(gstrdup(c.s(0))); }
template <class T, T (*fn)(const char*, char**, int)> static void strto_int(Ctx& c) {
    char* end; T v = fn(c.s(0), &end, c.i(2));
    if (c.u(1)) gref(c.u(1)) = h2g(end);
    if constexpr (sizeof(T) == 8 && (std::is_same_v<T, long long> || std::is_same_v<T, unsigned long long>)) c.ret64(v);
    else c.ret((uint32_t)v);
    hle_sync_errno();
}
static long strtol32(const char* s, char** e, int b) { long v = strtol(s, e, b); return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : v; }
static unsigned long strtoul32(const char* s, char** e, int b) { unsigned long v = strtoul(s, e, b); return v > UINT32_MAX && (long)v > 0 ? UINT32_MAX : v; }
static void h_strtod(Ctx& c) { char* end; double v = strtod(c.s(0), &end); if (c.u(1)) gref(c.u(1)) = h2g(end); c.retd(v); }
static void h_strtok_r(Ctx& c) {
    char* save = (char*)g2h(gref(c.u(2)));
    char* r = strtok_r((char*)c.p(0), c.s(1), &save);
    gref(c.u(2)) = h2g(save); c.ret(h2g(r));
}
static void h_strerror(Ctx& c) {
    static thread_local gaddr buf = 0;
    if (!buf) buf = gmalloc(128);
    put_out(buf, 128, strerror(c.i(0))); c.ret(buf);
}
static void h_strerror_r(Ctx& c) { put_out(c.u(1), c.u(2), strerror(c.i(0))); c.ret(0); }
static void h_basename(Ctx& c) {
    const char* s = c.s(0); const char* b = strrchr(s, '/');
    c.ret(b ? c.u(0) + (b - s) + 1 : c.u(0));
}
static void h_setlocale(Ctx& c) { static gaddr C = gstrdup("C"); c.ret(C); }
static void h_getenv(Ctx& c) { c.ret(0); }

// mbstate_t is 4 bytes on bionic, bigger on the host: keep a host-side state per call.
static void h_mbrtowc(Ctx& c) { mbstate_t st{}; c.ret(mbrtowc((wchar_t*)c.p(0), c.s(1), c.u(2), &st)); }
static void h_wcrtomb(Ctx& c) { mbstate_t st{}; c.ret(wcrtomb((char*)c.p(0), (wchar_t)c.u(1), &st)); }
static void h_wctype(Ctx& c) {
    static const char* names[] = {"alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower", "print", "punct", "space", "upper", "xdigit"};
    for (uint32_t i = 0; i < 12; i++) if (!strcmp(c.s(0), names[i])) { c.ret(i + 1); return; }
    c.ret(0);
}
static void h_iswctype(Ctx& c) {
    static const char* names[] = {"", "alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower", "print", "punct", "space", "upper", "xdigit"};
    uint32_t k = c.u(1); c.ret(k && k <= 12 ? iswctype(c.u(0), wctype(names[k])) : 0);
}

// ------------------------------------------------------------------ memory
static void h_malloc(Ctx& c) { c.ret(gmalloc(c.u(0))); }
static void h_calloc(Ctx& c) { uint32_t n = c.u(0) * c.u(1); gaddr p = gmalloc(n); memset(g2h(p), 0, n); c.ret(p); }
static void h_free(Ctx& c) { gfree(c.u(0)); }
static void h_realloc(Ctx& c) {
    gaddr old = c.u(0); uint32_t n = c.u(1);
    if (!old) { c.ret(gmalloc(n)); return; }
    if (!n) { gfree(old); c.ret(0); return; }
    gaddr p = gmalloc(n);
    memcpy(g2h(p), g2h(old), std::min(n, gmsize(old)));
    gfree(old); c.ret(p);
}
static void h_memalign(Ctx& c) { c.ret(gmalloc(c.u(1), c.u(0))); }

// ------------------------------------------------------------------ time
static void h_time(Ctx& c) { uint32_t t = (uint32_t)time(nullptr); if (c.u(0)) gref(c.u(0)) = t; c.ret(t); }
static void h_clock_gettime(Ctx& c) {
    timespec ts; int r = clock_gettime(c.i(0), &ts);
    gref(c.u(1)) = ts.tv_sec; gref(c.u(1) + 4) = ts.tv_nsec; c.ret(r);
}
static void h_gettimeofday(Ctx& c) {
    timeval tv; gettimeofday(&tv, nullptr);
    if (c.u(0)) { gref(c.u(0)) = tv.tv_sec; gref(c.u(0) + 4) = tv.tv_usec; }
    c.ret(0);
}
// bionic struct tm: 9 ints, long tm_gmtoff, char* tm_zone (44 bytes)
static void tm_to_guest(const tm* t, gaddr g) {
    memcpy(g2h(g), t, 36);
#ifdef __SWITCH__
    gref(g + 36) = 0;
#else
    gref(g + 36) = t->tm_gmtoff;
#endif
    gref(g + 40) = 0;
}
static tm tm_from_guest(gaddr g) { tm t{}; memcpy(&t, g2h(g), 36); return t; }
static void h_gmtime_r(Ctx& c) { time_t t = (int32_t)gref(c.u(0)); tm r; gmtime_r(&t, &r); tm_to_guest(&r, c.u(1)); c.ret(c.u(1)); }
static void h_gmtime(Ctx& c) { static thread_local gaddr b = 0; if (!b) b = gmalloc(44); time_t t = (int32_t)gref(c.u(0)); tm r; gmtime_r(&t, &r); tm_to_guest(&r, b); c.ret(b); }
static void h_localtime(Ctx& c) { static thread_local gaddr b = 0; if (!b) b = gmalloc(44); time_t t = (int32_t)gref(c.u(0)); tm r; localtime_r(&t, &r); tm_to_guest(&r, b); c.ret(b); }
static void h_strftime(Ctx& c) { tm t = tm_from_guest(c.u(3)); c.ret(strftime((char*)c.p(0), c.u(1), c.s(2), &t)); }
static void h_wcsftime(Ctx& c) { tm t = tm_from_guest(c.u(3)); c.ret(wcsftime((wchar_t*)c.p(0), c.u(1), (const wchar_t*)c.p(2), &t)); }
static void h_usleep(Ctx& c) { usleep(c.u(0)); c.ret(0); }
static void h_sysconf(Ctx& c) {
    switch (c.i(0)) {
    case 0x27: case 0x28: c.ret(4096); return;              // _SC_PAGESIZE
    case 0x60: case 0x61: c.ret(4); return;                 // _SC_NPROCESSORS_*
    case 0x06: c.ret(100); return;                          // _SC_CLK_TCK
    default: logf("sysconf(%d)", c.i(0)); c.ret(-1);
    }
}

// ------------------------------------------------------------------ files / dirs (fd level)
static void h_open(Ctx& c) {
    std::string p = map_path(c.s(0));
    int fl = c.i(1), hf = fl & 3;
    if (fl & 0100) hf |= O_CREAT; if (fl & 01000) hf |= O_TRUNC; if (fl & 02000) hf |= O_APPEND; if (fl & 0200) hf |= O_EXCL;
    int fd = open(p.c_str(), hf, c.u(2));
    logf("open(%s) -> %d", c.s(0), fd);
    hle_sync_errno(); c.ret(fd);
}
static void fill_stat(const struct stat& st, gaddr g) {
    memset(g2h(g), 0, 104);
    gref<uint64_t>(g) = st.st_dev; gref(g + 12) = st.st_ino; gref(g + 16) = st.st_mode; gref(g + 20) = st.st_nlink;
    gref<int64_t>(g + 48) = st.st_size; gref(g + 56) = 4096; gref<uint64_t>(g + 64) = st.st_blocks;
    gref(g + 72) = st.st_atime; gref(g + 80) = st.st_mtime; gref(g + 88) = st.st_ctime; gref<uint64_t>(g + 96) = st.st_ino;
}
static void h_stat(Ctx& c) { struct stat st; int r = stat(map_path(c.s(0)).c_str(), &st); if (!r) fill_stat(st, c.u(1)); hle_sync_errno(); c.ret(r); }
static void h_fstat(Ctx& c) { struct stat st; int r = fstat(c.i(0), &st); if (!r) fill_stat(st, c.u(1)); c.ret(r); }
static void h_access(Ctx& c) { int r = access(map_path(c.s(0)).c_str(), c.i(1)); hle_sync_errno(); c.ret(r); }
static void h_remove(Ctx& c) { c.ret(remove(map_path(c.s(0)).c_str())); }
static void h_unlink(Ctx& c) { c.ret(unlink(map_path(c.s(0)).c_str())); }
// The game saves via write-new-then-rename; the Switch SD card won't rename over an existing file.
static void h_rename(Ctx& c) {
    std::string a = map_path(c.s(0)), b = map_path(c.s(1));
    int r = rename(a.c_str(), b.c_str());
    if (r && access(b.c_str(), F_OK) == 0 && access(a.c_str(), F_OK) == 0) { remove(b.c_str()); r = rename(a.c_str(), b.c_str()); }
    if (r) logf("rename(%s, %s) failed: %s", c.s(0), c.s(1), strerror(errno));
    hle_sync_errno(); c.ret(r);
}
static void h_mkdir(Ctx& c) { c.ret(mkdir(map_path(c.s(0)).c_str(), c.u(1))); }
static std::unordered_map<gaddr, DIR*> g_dirs;
static void h_opendir(Ctx& c) {
    DIR* d = opendir(map_path(c.s(0)).c_str());
    if (!d) { c.ret(0); return; }
    gaddr g = gmalloc(280); g_dirs[g] = d; c.ret(g);
}
static void h_readdir(Ctx& c) {  // bionic dirent: u64 ino, s64 off, u16 reclen, u8 type, char name[256]
    auto it = g_dirs.find(c.u(0)); if (it == g_dirs.end()) { c.ret(0); return; }
    dirent* e = readdir(it->second); if (!e) { c.ret(0); return; }
    gaddr g = c.u(0);
    gref<uint64_t>(g) = e->d_ino; gref<uint64_t>(g + 8) = 0; gref<uint16_t>(g + 16) = 280; gref<uint8_t>(g + 18) = e->d_type;
    put_out(g + 19, 256, e->d_name); c.ret(g);
}
static void h_closedir(Ctx& c) { auto it = g_dirs.find(c.u(0)); if (it != g_dirs.end()) { closedir(it->second); g_dirs.erase(it); gfree(c.u(0)); } c.ret(0); }

// ------------------------------------------------------------------ pthreads
// Guest mutex/cond/sem objects are 4 bytes on bionic. We store a handle (0x80000000|index)
// in that word on first use and keep the real object host-side.
template <class T> struct HandleTable {
    static constexpr uint32_t N = 1 << 16;
    T* objs[N] = {};
    std::atomic<uint32_t> next{1};
    std::mutex mx;
    T* get(gaddr g) {
        uint32_t w = __atomic_load_n(&gref(g), __ATOMIC_ACQUIRE);
        if (w & 0x80000000) return objs[w & 0xFFFF];
        std::lock_guard<std::mutex> lk(mx);
        w = gref(g);
        if (w & 0x80000000) return objs[w & 0xFFFF];
        uint32_t i = next++;
        if (i >= N) fatal("handle table full");
        objs[i] = new T();   // ponytail: never freed (destroy just forgets), fine for a game's lifetime
        __atomic_store_n(&gref(g), 0x80000000 | i, __ATOMIC_RELEASE);
        return objs[i];
    }
};
struct Sem { std::mutex m; std::condition_variable cv; uint32_t count = 0; };
static HandleTable<std::recursive_mutex> g_mutexes;   // ponytail: every mutex recursive
static HandleTable<std::condition_variable_any> g_conds;
static HandleTable<Sem> g_sems;

static void h_mutex_init(Ctx& c) { gref(c.u(0)) = 0; g_mutexes.get(c.u(0)); c.ret(0); }
static void h_mutex_lock(Ctx& c) { g_mutexes.get(c.u(0))->lock(); c.ret(0); }
static void h_mutex_unlock(Ctx& c) { g_mutexes.get(c.u(0))->unlock(); c.ret(0); }
static void h_ret0(Ctx& c) { c.ret(0); }
static void h_retm1(Ctx& c) { c.ret(-1); }
static void h_cond_init(Ctx& c) { gref(c.u(0)) = 0; g_conds.get(c.u(0)); c.ret(0); }
static void h_cond_wait(Ctx& c) {
    auto* cv = g_conds.get(c.u(0)); auto* m = g_mutexes.get(c.u(1));
    std::unique_lock<std::recursive_mutex> lk(*m, std::adopt_lock);
    cv->wait(lk); lk.release(); c.ret(0);
}
static void h_cond_timedwait(Ctx& c) {
    auto* cv = g_conds.get(c.u(0)); auto* m = g_mutexes.get(c.u(1));
    gaddr ts = c.u(2);
    auto deadline = std::chrono::system_clock::time_point(std::chrono::seconds((int32_t)gref(ts)) + std::chrono::nanoseconds((int32_t)gref(ts + 4)));
    std::unique_lock<std::recursive_mutex> lk(*m, std::adopt_lock);
    bool to = cv->wait_until(lk, deadline) == std::cv_status::timeout;
    lk.release(); c.ret(to ? 110 /*ETIMEDOUT*/ : 0);
}
static void h_cond_signal(Ctx& c) { g_conds.get(c.u(0))->notify_one(); c.ret(0); }
static void h_cond_broadcast(Ctx& c) { g_conds.get(c.u(0))->notify_all(); c.ret(0); }
static void h_sem_init(Ctx& c) { gref(c.u(0)) = 0; g_sems.get(c.u(0))->count = c.u(2); c.ret(0); }
static void h_sem_post(Ctx& c) { Sem* s = g_sems.get(c.u(0)); { std::lock_guard<std::mutex> lk(s->m); s->count++; } s->cv.notify_one(); c.ret(0); }
static void h_sem_wait(Ctx& c) { Sem* s = g_sems.get(c.u(0)); std::unique_lock<std::mutex> lk(s->m); s->cv.wait(lk, [s] { return s->count > 0; }); s->count--; c.ret(0); }

// bionic pthread_attr_t: flags, stack_base, stack_size, guard_size, sched_policy, sched_priority
static void h_attr_init(Ctx& c) { memset(c.p(0), 0, 24); gref(c.u(0) + 8) = 1 << 20; c.ret(0); }
static void h_attr_setstacksize(Ctx& c) { gref(c.u(0) + 8) = c.u(1); c.ret(0); }
static void h_attr_setdetachstate(Ctx& c) { gref(c.u(0)) = c.u(1); c.ret(0); }

struct Exit { bool done = false; uint32_t ret = 0; };
static std::mutex g_join_mx;
static std::condition_variable g_join_cv;
static std::unordered_map<uint32_t, Exit> g_exits;
void pthread_exit_hook(uint32_t tid, uint32_t ret) {
    { std::lock_guard<std::mutex> lk(g_join_mx); g_exits[tid] = {true, ret}; }
    g_join_cv.notify_all();
}
static void h_pthread_create(Ctx& c) {
    uint32_t stack = c.u(1) ? gref(c.u(1) + 8) : (1u << 20);
    uint32_t tid = guest_thread_spawn(c.u(2), c.u(3), stack);
    gref(c.u(0)) = tid;
    logf("pthread_create: tid %u entry %s", tid, addr_name(c.u(2)).c_str());
    c.ret(0);
}
static void h_pthread_join(Ctx& c) {
    std::unique_lock<std::mutex> lk(g_join_mx);
    g_join_cv.wait(lk, [&] { return g_exits[c.u(0)].done; });
    if (c.u(1)) gref(c.u(1)) = g_exits[c.u(0)].ret;
    c.ret(0);
}
static void h_pthread_exit(Ctx& c) { guest_return_now(c.u(0)); }
static void h_pthread_self(Ctx& c) { c.ret(cur_tid()); }
static void h_gettid(Ctx& c) { c.ret(cur_tid()); }

static std::atomic<uint32_t> g_next_key{1};
static thread_local uint32_t t_keys[512];
static void h_key_create(Ctx& c) { uint32_t k = g_next_key++; if (k >= 512) fatal("pthread keys"); gref(c.u(0)) = k; c.ret(0); }
static void h_getspecific(Ctx& c) { c.ret(c.u(0) < 512 ? t_keys[c.u(0)] : 0); }
static void h_setspecific(Ctx& c) { if (c.u(0) < 512) t_keys[c.u(0)] = c.u(1); c.ret(0); }
static std::recursive_mutex g_once_mx;
static void h_once(Ctx& c) {
    std::lock_guard<std::recursive_mutex> lk(g_once_mx);
    if (gref(c.u(0)) == 0) { gref(c.u(0)) = 1; call_guest(c.u(1)); }
    c.ret(0);
}

// futex (bionic uses it inside the C++ static-init guards): emulated with one global condvar.
static std::mutex g_futex_mx;
static std::condition_variable g_futex_cv;
static void h_syscall(Ctx& c) {
    switch (c.u(0)) {
    case 240: {  // futex(addr, op, val, timeout)
        gaddr a = c.u(1); uint32_t op = c.u(2) & 0x7F;
        if (op == 0) {
            std::unique_lock<std::mutex> lk(g_futex_mx);
            if (gref(a) != c.u(3)) { gref(cur_tls() + 8) = EAGAIN; c.ret(-1); return; }
            if (c.u(4)) g_futex_cv.wait_for(lk, std::chrono::seconds((int32_t)gref(c.u(4))) + std::chrono::nanoseconds((int32_t)gref(c.u(4) + 4)));
            else g_futex_cv.wait_for(lk, std::chrono::milliseconds(50));  // ponytail: bounded wait guards against missed wakes
            c.ret(0);
        } else if (op == 1) {
            { std::lock_guard<std::mutex> lk(g_futex_mx); }
            g_futex_cv.notify_all(); c.ret(1);
        } else { logf("futex op %u", op); c.ret(-1); }
        return;
    }
    case 224: c.ret(cur_tid()); return;
    default: logf("syscall %u unimplemented", c.u(0)); c.ret(-1);
    }
}

// ------------------------------------------------------------------ setjmp / longjmp
// jmp_buf (ours): r4-r11, sp, lr, d8-d15
static void save_jmp(gaddr b) {
    for (int i = 0; i < 8; i++) gref(b + 4 * i) = reg(4 + i);
    gref(b + 32) = reg(13); gref(b + 36) = reg(14);
    for (int i = 0; i < 8; i++) gref<uint64_t>(b + 40 + 8 * i) = dreg(8 + i);
}
static void h_setjmp(Ctx& c) { save_jmp(c.u(0)); c.ret(0); }
static void h_longjmp(Ctx& c) {
    gaddr b = c.u(0); uint32_t v = c.u(1) ? c.u(1) : 1;
    for (int i = 0; i < 8; i++) reg(4 + i) = gref(b + 4 * i);
    reg(13) = gref(b + 32);
    for (int i = 0; i < 8; i++) dreg(8 + i) = gref<uint64_t>(b + 40 + 8 * i);
    reg(0) = v;
    guest_set_pc(gref(b + 36));
}

// ------------------------------------------------------------------ qsort / bsearch (guest comparator)
static void h_qsort(Ctx& c) {
    gaddr base = c.u(0); uint32_t n = c.u(1), sz = c.u(2); gaddr cmp = c.u(3);
    std::vector<uint32_t> idx(n);
    for (uint32_t i = 0; i < n; i++) idx[i] = i;
    std::stable_sort(idx.begin(), idx.end(), [&](uint32_t a, uint32_t b) { return (int32_t)call_guest(cmp, {base + a * sz, base + b * sz}) < 0; });
    std::vector<uint8_t> tmp(n * sz);
    for (uint32_t i = 0; i < n; i++) memcpy(&tmp[i * sz], g2h(base + idx[i] * sz), sz);
    memcpy(g2h(base), tmp.data(), n * sz);
}
static void h_bsearch(Ctx& c) {
    gaddr key = c.u(0), base = c.u(1); uint32_t n = c.u(2), sz = c.u(3); gaddr cmp = c.word(4);
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        int32_t r = (int32_t)call_guest(cmp, {key, base + mid * sz});
        if (!r) { c.ret(base + mid * sz); return; }
        if (r < 0) hi = mid; else lo = mid + 1;
    }
    c.ret(0);
}

// ------------------------------------------------------------------ dl*
static void h_dlopen(Ctx& c) {
    const char* n = c.s(0);
    logf("dlopen(%s)", n ? n : "NULL");
    if (n && strstr(n, "OpenSLES")) { c.ret(0); return; }   // no audio backend yet
    c.ret(1);
}
static void h_dlsym(Ctx& c) {
    gaddr a = any_sym(c.s(1));
    if (!a) a = hle_lookup(c.s(1));
    if (!a) logf("dlsym(%s) -> NULL", c.s(1));
    c.ret(a);
}
static void h_dlerror(Ctx& c) { static gaddr e = gstrdup("not found"); c.ret(e); }

// ------------------------------------------------------------------ android log / process
static void h_log_print(Ctx& c) { c.slot = 3; ArgSrc a{&c}; logf("[%s] %s", c.s(1), gformat(c.s(2), a).c_str()); c.ret(0); }
static void h_log_vprint(Ctx& c) { ArgSrc a; a.va = c.u(3); logf("[%s] %s", c.s(1), gformat(c.s(2), a).c_str()); c.ret(0); }
static void h_log_write(Ctx& c) { logf("[%s] %s", c.s(1), c.s(2)); c.ret(0); }
static void h_assert2(Ctx& c) { fatal("guest assert: %s:%d %s: %s", c.s(0), c.i(1), c.s(2), c.s(3)); }
static void h_abort(Ctx& c) { fatal("guest abort() from %s", addr_name(reg(14)).c_str()); }
static void h_exit(Ctx& c) { logf("guest exit(%d)", c.i(0)); exit(c.i(0)); }
static void h_stack_chk_fail(Ctx& c) { fatal("guest stack smashing detected at %s", addr_name(reg(14)).c_str()); }
static void h_gethostname(Ctx& c) { put_out(c.u(0), c.u(1), "switch"); c.ret(0); }
static void h_lrand48(Ctx& c) { c.ret(lrand48() & 0x7FFFFFFF); }
static void h_srand48(Ctx& c) { srand48(c.i(0)); }

// ------------------------------------------------------------------ data imports
static const unsigned char kCtype[257] = {0,  // bionic/BSD _ctype_ (index -1 = EOF)
#define C_ 0x20
    C_,C_,C_,C_,C_,C_,C_,C_,C_,C_|8,C_|8,C_|8,C_|8,C_|8,C_,C_, C_,C_,C_,C_,C_,C_,C_,C_,C_,C_,C_,C_,C_,C_,C_,C_,
    0x88,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,
    4,4,4,4,4,4,4,4,4,4,0x10,0x10,0x10,0x10,0x10,0x10,
    0x10,0x41,0x41,0x41,0x41,0x41,0x41,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,0x10,0x10,0x10,0x10,0x10,
    0x10,0x42,0x42,0x42,0x42,0x42,0x42,2,2,2,2,2,2,2,2,2, 2,2,2,2,2,2,2,2,2,2,2,0x10,0x10,0x10,0x10,C_,
#undef C_
};

// const-correct wrappers (newlib has no C++ const overloads of these)
static const void* c_memchr(const void* s, int c, size_t n) { return memchr(s, c, n); }
static const void* c_memrchr(const void* s, int c, size_t n) {
    const unsigned char* p = (const unsigned char*)s + n;
    while (n--) if (*--p == (unsigned char)c) return p;
    return nullptr;
}
static const char* c_strchr(const char* s, int c) { return strchr(s, c); }
static const char* c_strrchr(const char* s, int c) { return strrchr(s, c); }
static const char* c_strpbrk(const char* s, const char* a) { return strpbrk(s, a); }
static const char* c_strstr(const char* s, const char* a) { return strstr(s, a); }
static const wchar_t* c_wmemchr(const wchar_t* s, wchar_t c, size_t n) { return wmemchr(s, c, n); }
static const void* c_memmem(const void* h, size_t hn, const void* n, size_t nn) {
    if (!nn) return h;
    for (size_t i = 0; i + nn <= hn; i++) if (!memcmp((const char*)h + i, n, nn)) return (const char*)h + i;
    return nullptr;
}

void hle_libc_init() {
    // data
    g_sF = gmalloc(3 * BIONIC_FILE_SIZE);
    memset(g2h(g_sF), 0, 3 * BIONIC_FILE_SIZE);
    g_files[g_sF] = stdin; g_files[g_sF + BIONIC_FILE_SIZE] = stdout; g_files[g_sF + 2 * BIONIC_FILE_SIZE] = stderr;
    hle_data_register("__sF", g_sF);
    gaddr ct = gmalloc(257); memcpy(g2h(ct), kCtype, 257);
    gaddr ctp = gmalloc(4); gref(ctp) = ct + 1;   // bionic exports `const char* _ctype_`
    hle_data_register("_ctype_", ctp);
    gaddr tl = gmalloc(257 * 2); for (int i = 0; i < 257; i++) gref<int16_t>(tl + 2 * i) = i == 0 ? -1 : tolower(i - 1);
    gaddr tlp = gmalloc(4); gref(tlp) = tl;
    hle_data_register("_tolower_tab_", tlp);
    gaddr guard = gmalloc(4); gref(guard) = 0xC0FFEE00;
    hle_data_register("__stack_chk_guard", guard);

    struct { const char* n; HleFn f; } t[] = {
        // string.h
        {"memchr", W(c_memchr)}, {"memrchr", W(c_memrchr)},
        {"memcmp", W(memcmp)}, {"memcpy", W(memcpy)}, {"memmove", W(memmove)}, {"memset", W(memset)},
        {"memmem", W(c_memmem)},
        {"strcasecmp", W(strcasecmp)}, {"strncasecmp", W(strncasecmp)}, {"strcat", W(strcat)}, {"strncat", W(strncat)},
        {"strchr", W(c_strchr)}, {"strrchr", W(c_strrchr)},
        {"strcmp", W(strcmp)}, {"strncmp", W(strncmp)}, {"strcoll", W(strcoll)}, {"strcpy", W(strcpy)}, {"strncpy", W(strncpy)},
        {"strcspn", W(strcspn)}, {"strspn", W(strspn)}, {"strlen", W(strlen)}, {"strxfrm", W(strxfrm)},
        {"strpbrk", W(c_strpbrk)}, {"strstr", W(c_strstr)},
        {"strdup", h_strdup}, {"strtok_r", h_strtok_r}, {"strerror", h_strerror}, {"strerror_r", h_strerror_r},
        {"strtol", strto_int<long, strtol32>}, {"strtoul", strto_int<unsigned long, strtoul32>},
        {"strtoll", strto_int<long long, strtoll>}, {"strtoull", strto_int<unsigned long long, strtoull>},
        {"strtod", h_strtod}, {"atoi", W(atoi)}, {"basename", h_basename},
        // ctype / wchar
        {"isalnum", W(isalnum)}, {"isalpha", W(isalpha)}, {"iscntrl", W(iscntrl)}, {"islower", W(islower)}, {"isprint", W(isprint)},
        {"ispunct", W(ispunct)}, {"isspace", W(isspace)}, {"isupper", W(isupper)}, {"isxdigit", W(isxdigit)},
        {"tolower", W(tolower)}, {"toupper", W(toupper)}, {"towlower", W(towlower)}, {"towupper", W(towupper)},
        {"btowc", W(btowc)}, {"wctob", W(wctob)}, {"wctype", h_wctype}, {"iswctype", h_iswctype},
        {"wcslen", W(wcslen)}, {"wcscoll", W(wcscoll)}, {"wcsxfrm", W(wcsxfrm)},
        {"wmemchr", W(c_wmemchr)}, {"wmemcmp", W(wmemcmp)},
        {"wmemcpy", W(wmemcpy)}, {"wmemmove", W(wmemmove)}, {"wmemset", W(wmemset)},
        {"mbrtowc", h_mbrtowc}, {"wcrtomb", h_wcrtomb}, {"wcsftime", h_wcsftime},
        {"getwc", h_retm1}, {"putwc", h_retm1}, {"ungetwc", h_retm1},
        // math (softfp: doubles in core register pairs, handled by the wrapper)
        {"acos", WS(acos, double (*)(double))}, {"asin", WS(asin, double (*)(double))}, {"atan", WS(atan, double (*)(double))},
        {"atan2", WS(atan2, double (*)(double, double))}, {"ceil", WS(ceil, double (*)(double))}, {"cos", WS(cos, double (*)(double))},
        {"cosh", WS(cosh, double (*)(double))}, {"exp", WS(exp, double (*)(double))}, {"floor", WS(floor, double (*)(double))},
        {"fmod", WS(fmod, double (*)(double, double))}, {"log", WS(log, double (*)(double))}, {"log10", WS(log10, double (*)(double))},
        {"pow", WS(pow, double (*)(double, double))}, {"sin", WS(sin, double (*)(double))}, {"sinh", WS(sinh, double (*)(double))},
        {"tan", WS(tan, double (*)(double))}, {"tanh", WS(tanh, double (*)(double))}, {"rint", WS(rint, double (*)(double))},
        {"ldexp", WS(ldexp, double (*)(double, int))}, {"frexp", WS(frexp, double (*)(double, int*))}, {"modf", WS(modf, double (*)(double, double*))},
        {"acosf", WS(acosf, float (*)(float))}, {"atan2f", WS(atan2f, float (*)(float, float))}, {"cosf", WS(cosf, float (*)(float))},
        {"sinf", WS(sinf, float (*)(float))}, {"tanf", WS(tanf, float (*)(float))}, {"floorf", WS(floorf, float (*)(float))},
        {"fmodf", WS(fmodf, float (*)(float, float))}, {"powf", WS(powf, float (*)(float, float))}, {"log10f", WS(log10f, float (*)(float))},
        {"modff", WS(modff, float (*)(float, float*))}, {"lrintf", WS(lrintf, long (*)(float))},
        // stdio
        {"fopen", h_fopen}, {"fdopen", h_fdopen}, {"freopen", h_freopen}, {"fclose", h_fclose}, {"fread", h_fread}, {"fwrite", h_fwrite},
        {"fseek", h_fseek}, {"fseeko", h_fseek}, {"ftell", h_ftell}, {"ftello", h_ftell}, {"fflush", h_fflush}, {"fgets", h_fgets},
        {"fputs", h_fputs}, {"fputc", h_fputc}, {"putc", h_fputc}, {"getc", h_getc}, {"ungetc", h_ungetc}, {"setvbuf", h_setvbuf},
        {"fprintf", h_fprintf}, {"vfprintf", h_vfprintf}, {"printf", h_printf}, {"puts", h_puts}, {"putchar", h_putchar},
        {"perror", h_perror}, {"sprintf", h_sprintf}, {"snprintf", h_snprintf}, {"vsprintf", h_vsprintf}, {"vsnprintf", h_vsnprintf},
        {"sscanf", h_sscanf},
        // memory
        {"malloc", h_malloc}, {"calloc", h_calloc}, {"realloc", h_realloc}, {"free", h_free}, {"memalign", h_memalign},
        // time
        {"time", h_time}, {"clock_gettime", h_clock_gettime}, {"gettimeofday", h_gettimeofday}, {"gmtime", h_gmtime},
        {"gmtime_r", h_gmtime_r}, {"localtime", h_localtime}, {"strftime", h_strftime}, {"usleep", h_usleep}, {"sysconf", h_sysconf},
        // files
        {"open", h_open}, {"close", W(close)}, {"read", W(read)}, {"write", W(write)}, {"lseek", W(lseek)},
        {"stat", h_stat}, {"fstat", h_fstat}, {"access", h_access}, {"remove", h_remove}, {"unlink", h_unlink}, {"rename", h_rename},
        {"mkdir", h_mkdir}, {"opendir", h_opendir}, {"readdir", h_readdir}, {"closedir", h_closedir},
        {"chmod", h_ret0}, {"chown", h_ret0}, {"utime", h_ret0}, {"fcntl", h_ret0}, {"ioctl", h_retm1}, {"dup2", h_retm1},
        {"writev", h_retm1}, {"poll", h_ret0}, {"select", h_ret0},
        // pthread
        {"pthread_mutex_init", h_mutex_init}, {"pthread_mutex_lock", h_mutex_lock}, {"pthread_mutex_unlock", h_mutex_unlock},
        {"pthread_mutex_destroy", h_ret0}, {"pthread_mutexattr_init", h_ret0}, {"pthread_mutexattr_settype", h_ret0},
        {"pthread_cond_init", h_cond_init}, {"pthread_cond_wait", h_cond_wait}, {"pthread_cond_timedwait", h_cond_timedwait},
        {"pthread_cond_signal", h_cond_signal}, {"pthread_cond_broadcast", h_cond_broadcast}, {"pthread_cond_destroy", h_ret0},
        {"pthread_attr_init", h_attr_init}, {"pthread_attr_destroy", h_ret0}, {"pthread_attr_setstacksize", h_attr_setstacksize},
        {"pthread_attr_setdetachstate", h_attr_setdetachstate}, {"pthread_create", h_pthread_create}, {"pthread_join", h_pthread_join},
        {"pthread_exit", h_pthread_exit}, {"pthread_self", h_pthread_self}, {"pthread_kill", h_ret0},
        {"pthread_key_create", h_key_create}, {"pthread_key_delete", h_ret0}, {"pthread_getspecific", h_getspecific},
        {"pthread_setspecific", h_setspecific}, {"pthread_once", h_once}, {"gettid", h_gettid}, {"syscall", h_syscall},
        {"sem_init", h_sem_init}, {"sem_post", h_sem_post}, {"sem_wait", h_sem_wait}, {"sem_destroy", h_ret0},
        // control flow
        {"setjmp", h_setjmp}, {"sigsetjmp", h_setjmp}, {"longjmp", h_longjmp}, {"siglongjmp", h_longjmp},
        {"qsort", h_qsort}, {"bsearch", h_bsearch},
        // dl / log / process
        {"dlopen", h_dlopen}, {"dlsym", h_dlsym}, {"dlclose", h_ret0}, {"dlerror", h_dlerror},
        {"__android_log_print", h_log_print}, {"__android_log_vprint", h_log_vprint}, {"__android_log_write", h_log_write},
        {"__assert2", h_assert2}, {"abort", h_abort}, {"exit", h_exit}, {"__stack_chk_fail", h_stack_chk_fail}, {"__errno", h_errno},
        {"__cxa_atexit", h_ret0}, {"__cxa_finalize", h_ret0}, {"__aeabi_atexit", h_ret0},
        {"getpid", h_gettid}, {"getuid", h_ret0}, {"geteuid", h_ret0}, {"getgid", h_ret0}, {"getegid", h_ret0},
        {"getpwuid", h_ret0}, {"gethostname", h_gethostname}, {"getenv", h_getenv}, {"setlocale", h_setlocale},
        {"lrand48", h_lrand48}, {"srand48", h_srand48},
        {"sigaction", h_ret0}, {"bsd_signal", h_ret0}, {"sigprocmask", h_ret0}, {"kill", h_ret0}, {"raise", h_ret0}, {"alarm", h_ret0},
        {"fork", h_retm1}, {"execl", h_retm1}, {"waitpid", h_retm1},
        // networking: offline only
        {"socket", h_retm1}, {"socketpair", h_retm1}, {"connect", h_retm1}, {"bind", h_retm1}, {"listen", h_retm1}, {"accept", h_retm1},
        {"send", h_retm1}, {"sendto", h_retm1}, {"recv", h_retm1}, {"recvfrom", h_retm1}, {"shutdown", h_retm1},
        {"setsockopt", h_retm1}, {"getsockopt", h_retm1}, {"getsockname", h_retm1}, {"getpeername", h_retm1},
        {"getaddrinfo", h_retm1}, {"freeaddrinfo", h_ret0}, {"gethostbyname", h_ret0}, {"inet_addr", h_retm1},
        {"inet_ntoa", h_ret0}, {"inet_ntop", h_ret0}, {"inet_pton", h_ret0}, {"if_indextoname", h_ret0},
    };
    for (auto& e : t) hle_register(e.n, e.f);
}

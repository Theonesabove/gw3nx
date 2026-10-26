// gw3nx: runs the 32-bit ARM Android build of Geometry Wars 3 under dynarmic.
// Guest = 32-bit ARM code from the APK. Host = this program (x64 PC or Switch).
#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <functional>

typedef uint32_t gaddr;  // guest address

// ---- guest memory: one contiguous 4 GiB host window, host = g_mem + guest ----
extern uint8_t* g_mem;
inline void* g2h(gaddr a) { return a ? g_mem + a : nullptr; }
inline gaddr h2g(const void* p) { return p ? (gaddr)((const uint8_t*)p - g_mem) : 0; }
template <class T = uint32_t> inline T& gref(gaddr a) { return *(T*)(g_mem + a); }
inline const char* gstr(gaddr a) { return (const char*)g2h(a); }

void mem_init();
gaddr gmalloc(uint32_t size, uint32_t align = 16);
void gfree(gaddr p);
uint32_t gmsize(gaddr p);
gaddr gstrdup(const char* s);

// ---- logging ----
uint64_t now_us();
extern std::atomic<uint64_t> g_t_shader_us, g_t_asset_us;   // startup profiling totals
void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
[[noreturn]] void fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// ---- CPU / calls ----
uint32_t& reg(int i);           // current thread's guest registers (r0..r15)
uint64_t& dreg(int i);           // d0..d31
gaddr cur_tls();                 // current guest thread's TLS block
uint64_t call_guest(gaddr fn, std::initializer_list<uint32_t> args = {});
void thread_init_main();
void guest_thread_adopt();
void watchdog_start();
void host_thread_start(std::function<void()> body);  // detached host thread      // let a host-created thread call guest code
uint32_t guest_thread_spawn(gaddr fn, gaddr arg, uint32_t stack_size);  // returns tid
void guest_set_pc(gaddr pc);     // used by longjmp: resume guest elsewhere
void guest_return_now(uint32_t r0);  // pthread_exit: unwind to the host caller
uint32_t cur_tid();

// ---- HLE (host implementations of imported functions) ----
struct Ctx;
typedef void (*HleFn)(Ctx&);
gaddr hle_stub(const char* name, HleFn fn);   // guest address that calls fn
gaddr hle_lookup(const char* name);           // stub for a registered import, 0 if unknown
void hle_register(const char* name, HleFn fn);
void hle_hook(gaddr guest_fn, const char* name, HleFn fn);  // overwrite a guest function
// Run fn first, then the original function (fn must not change r0-r3). The first two ARM
// instructions of guest_fn are moved into a trampoline, so they must not be PC-relative.
void hle_hook_pre(gaddr guest_fn, const char* name, HleFn fn);
// Replace guest_fn with fn and return an address that runs the original (same first-two-instruction rule).
gaddr hle_hook_wrap(gaddr guest_fn, const char* name, HleFn fn);
gaddr hle_data(const char* name);             // data imports (__sF, _ctype_...)
void hle_data_register(const char* name, gaddr addr);
extern thread_local const char* g_cur_hle;                 // name of HLE fn being run (for crash logs)

// Arguments follow AAPCS softfp: words in r0-r3 then the stack, 64-bit values in even pairs.
struct Ctx {
    unsigned slot = 0;
    uint32_t word(unsigned i) { return i < 4 ? reg(i) : gref(reg(13) + 4 * (i - 4)); }
    uint32_t next() { return word(slot++); }
    uint64_t next64() { slot = (slot + 1) & ~1u; uint64_t lo = next(); return lo | (uint64_t)next() << 32; }
    uint32_t u(unsigned i) { return word(i); }
    int32_t i(unsigned i) { return (int32_t)word(i); }
    float f(unsigned i) { uint32_t w = word(i); float f; memcpy(&f, &w, 4); return f; }
    void* p(unsigned i) { return g2h(word(i)); }
    const char* s(unsigned i) { return gstr(word(i)); }
    void ret(uint32_t v) { reg(0) = v; }
    void ret64(uint64_t v) { reg(0) = (uint32_t)v; reg(1) = (uint32_t)(v >> 32); }
    void retf(float f) { uint32_t w; memcpy(&w, &f, 4); reg(0) = w; }
    void retd(double d) { uint64_t w; memcpy(&w, &d, 8); ret64(w); }
};

// Guest-side 32-bit vs 64-bit: host `long`/size_t/pointers are 32-bit words on the guest,
// `long long` and double are 64-bit pairs.
template <class T> constexpr bool is64g = std::is_same_v<T, double> || std::is_same_v<T, long long> || std::is_same_v<T, unsigned long long>;

extern gaddr g_guest_lo, g_guest_hi;
[[noreturn]] void bad_guest_pointer(gaddr a);
template <class T> T ctx_get(Ctx& c) {
    if constexpr (std::is_pointer_v<T>) {
        gaddr a = c.next();
        if (a && (a < g_guest_lo || a >= g_guest_hi)) bad_guest_pointer(a);
        return (T)g2h(a);
    }
    else if constexpr (std::is_same_v<T, float>) { uint32_t w = c.next(); float f; memcpy(&f, &w, 4); return f; }
    else if constexpr (std::is_same_v<T, double>) { uint64_t w = c.next64(); double d; memcpy(&d, &w, 8); return d; }
    else if constexpr (is64g<T>) return (T)c.next64();
    else if constexpr (std::is_signed_v<T>) return (T)(int32_t)c.next();
    else return (T)c.next();
}
template <class R> void ctx_ret(Ctx& c, R r) {
    if constexpr (std::is_pointer_v<R>) c.ret(h2g((const void*)r));
    else if constexpr (std::is_same_v<R, float>) c.retf(r);
    else if constexpr (std::is_same_v<R, double>) c.retd(r);
    else if constexpr (is64g<R>) c.ret64((uint64_t)r);
    else c.ret((uint32_t)r);
}
void hle_sync_errno();
template <class R, class... A> struct WrapImpl {
    template <auto F> static void call(Ctx& c) {
        std::tuple<A...> args{ctx_get<A>(c)...};  // braced init: evaluated left to right
        if constexpr (std::is_void_v<R>) std::apply(F, args);
        else ctx_ret<R>(c, std::apply(F, args));
        hle_sync_errno();
    }
};
template <auto F> struct Wrap;
template <class R, class... A, R (*F)(A...)> struct Wrap<F> { static void call(Ctx& c) { WrapImpl<R, A...>::template call<F>(c); } };
template <class R, class... A, R (*F)(A...) noexcept> struct Wrap<F> { static void call(Ctx& c) { WrapImpl<R, A...>::template call<F>(c); } };
// W(fn) turns a host function with plain arguments into an HLE handler.
#define W(fn) (&Wrap<fn>::call)
#define WS(fn, sig) (&Wrap<static_cast<sig>(fn)>::call)

// ---- loader ----
struct GLib {
    std::string name;
    gaddr base = 0, end = 0;
    gaddr dynsym = 0, dynstr = 0, hash = 0, exidx = 0;
    uint32_t exidx_count = 0, nsyms = 0;
    gaddr init_array = 0; uint32_t init_count = 0; gaddr init_fn = 0;
};
GLib* load_lib(const char* path, gaddr base);
gaddr lib_sym(GLib* lib, const char* name);    // exported symbol address
gaddr any_sym(const char* name);              // search all loaded libs
void lib_run_init(GLib* lib);
GLib* lib_for_addr(gaddr a);
std::string addr_name(gaddr a);               // "lib+0xoff (nearest symbol)"

// ---- registration hooks per module ----
void hle_libc_init();
void hle_gl_init();
void jni_init();
void hle_android_init();
void hle_opensl_init();
gaddr jni_env();
gaddr jni_vm();
gaddr jni_new_string(const char* s);
gaddr jni_obj(const char* tag);

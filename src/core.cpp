#include "core.hpp"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <map>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <functional>
#include <pthread.h>
#include <unordered_map>
#include <vector>
#ifdef __SWITCH__
#include <switch.h>
#else
#include <sys/mman.h>
#endif

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/A32/coprocessor.h"
#include "dynarmic/interface/exclusive_monitor.h"

using namespace Dynarmic;

uint8_t* g_mem;
thread_local const char* g_cur_hle = "";

static std::mutex g_log_mx;
void logf(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_log_mx);
    uint64_t t = now_us();
    fprintf(stderr, "[%3llu.%03llu] ", (unsigned long long)(t / 1000000), (unsigned long long)(t / 1000 % 1000));
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr); fflush(stderr);
}
uint64_t now_us() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
}
std::atomic<uint64_t> g_t_shader_us{0}, g_t_asset_us{0};
void fatal(const char* fmt, ...) {
    { std::lock_guard<std::mutex> lk(g_log_mx);
      va_list ap; va_start(ap, fmt); fprintf(stderr, "FATAL: "); vfprintf(stderr, fmt, ap); va_end(ap);
      fputc('\n', stderr); }
    fflush(stderr);
    abort();
}

// ---------------------------------------------------------------- memory
// Guest layout: everything below GUEST_LO is unmapped (null traps), HLE stubs at 0x0F000000,
// libs at 0x10000000..0x12500000, heap from HEAP_LO to the end of the mapped window.
// Host side: one contiguous block holds [GUEST_LO, HEAP_HI); g_mem = block - GUEST_LO.
static constexpr gaddr GUEST_LO = 0x0F000000, HEAP_LO = 0x13000000;
static gaddr HEAP_HI;
gaddr g_guest_lo = GUEST_LO, g_guest_hi = 0;
static std::array<uint8_t*, A32::UserConfig::NUM_PAGE_TABLE_ENTRIES> g_page_table;

void mem_init() {
#ifdef __SWITCH__
    // libnx gives (almost) all free RAM to the newlib heap at startup, so size from the heap itself.
    // The game peaks around 140 MiB of guest heap; the rest stays for JIT caches, Mesa and SDL.
    extern char* fake_heap_start; extern char* fake_heap_end;
    uint64_t heap = fake_heap_end - fake_heap_start;
    uint64_t size = std::min<uint64_t>(512ull << 20, heap > (512ull << 20) ? heap - (320ull << 20) : 0) & ~0xFFFFFull;
    logf("mem: heap %llu MiB, guest window %llu MiB", (unsigned long long)(heap >> 20), (unsigned long long)(size >> 20));
    uint8_t* block = size >= (256u << 20) ? (uint8_t*)aligned_alloc(0x1000, size) : nullptr;
    if (!block) fatal("not enough memory for the guest (heap %llu MiB) - launch through title takeover (hold R on a game)", (unsigned long long)(heap >> 20));
#else
    // ponytail: 3.7 GiB reserved up front, committed lazily by Linux.
    uint64_t size = 0xF0000000ull - GUEST_LO;
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) fatal("mmap guest window");
    uint8_t* block = (uint8_t*)p;
#endif
    g_mem = block - GUEST_LO;
    HEAP_HI = GUEST_LO + (gaddr)size;
    g_guest_hi = HEAP_HI;
    for (size_t i = GUEST_LO >> 12; i < (HEAP_HI >> 12); i++) g_page_table[i] = g_mem;
}

// Heap: power-of-two classes up to 64 KiB with free lists; bigger blocks first-fit with coalescing.
// Each block has a 16-byte header right before the payload: [size][class/0][raw start][magic].
static std::mutex g_heap_mx;
static gaddr g_bump = HEAP_LO;
static std::vector<gaddr> g_free_small[17];        // class k = 16<<k bytes (k<=12 -> 64K)
static std::map<gaddr, uint32_t> g_free_big;       // raw addr -> raw size
static constexpr uint32_t MAGIC = 0x6A3B11C0;

static gaddr bump(uint32_t n) {
    gaddr a = g_bump;
    g_bump += (n + 15) & ~15u;
    if (g_bump > HEAP_HI || g_bump < a) fatal("guest heap exhausted");
    return a;
}
static gaddr big_take(uint32_t n) {
    for (auto it = g_free_big.begin(); it != g_free_big.end(); ++it) {  // ponytail: O(n) first fit
        if (it->second < n) continue;
        gaddr a = it->first; uint32_t sz = it->second;
        g_free_big.erase(it);
        if (sz - n >= 4096) g_free_big[a + n] = sz - n; else n = sz;
        gref(a) = n;  // remember raw size in first word of raw block
        return a;
    }
    gaddr a = bump(n); gref(a) = n; return a;
}
static void big_give(gaddr a, uint32_t n) {
    auto next = g_free_big.lower_bound(a);
    if (next != g_free_big.end() && a + n == next->first) { n += next->second; next = g_free_big.erase(next); }
    if (next != g_free_big.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == a) { prev->second += n; return; }
    }
    g_free_big[a] = n;
}

gaddr gmalloc(uint32_t size, uint32_t align) {
    if (align < 16) align = 16;
    std::lock_guard<std::mutex> lk(g_heap_mx);
    uint32_t need = size + 16 + (align > 16 ? align : 0);
    gaddr raw; uint32_t cls = 0xFF;
    if (need <= (16u << 12)) {
        cls = 0; while ((16u << cls) < need) cls++;
        if (!g_free_small[cls].empty()) { raw = g_free_small[cls].back(); g_free_small[cls].pop_back(); }
        else raw = bump(16u << cls);
    } else {
        raw = big_take((need + 4095) & ~4095u);
    }
    gaddr p = (raw + 16 + align - 1) & ~(align - 1);
    gref(p - 16) = size; gref(p - 12) = cls; gref(p - 8) = raw; gref(p - 4) = MAGIC;
    return p;
}
void gfree(gaddr p) {
    if (!p) return;
    std::lock_guard<std::mutex> lk(g_heap_mx);
    if (gref(p - 4) != MAGIC) { logf("gfree: bad pointer %08x (%s)", p, g_cur_hle); return; }
    gref(p - 4) = 0;
    uint32_t cls = gref(p - 12); gaddr raw = gref(p - 8);
    if (cls != 0xFF) g_free_small[cls].push_back(raw);
    else big_give(raw, gref(raw));
}
uint32_t guest_heap_used() { return g_bump - HEAP_LO; }
uint32_t gmsize(gaddr p) { return p ? gref(p - 16) : 0; }
gaddr gstrdup(const char* s) {
    size_t n = strlen(s) + 1; gaddr g = gmalloc(n); memcpy(g2h(g), s, n); return g;
}

// ---------------------------------------------------------------- HLE registry / stubs
static constexpr gaddr STUB_BASE = 0x0F000000, STUB_MAX = 0x10000;  // 8 bytes each
static constexpr uint32_t SVC_RET = 0xFFFFFF;                         // return-to-host trap
static gaddr g_ret_trap;
struct HleEntry { std::string name; HleFn fn; };
static std::vector<HleEntry> g_hle;                  // index = svc number
static std::unordered_map<std::string, HleFn>& hle_fns() { static std::unordered_map<std::string, HleFn> m; return m; }
static std::unordered_map<std::string, gaddr> g_hle_stubs, g_hle_datas;
static std::mutex g_hle_mx;

static void write_stub(gaddr at, uint32_t svc) {
    gref(at) = 0xEF000000 | svc;      // svc #n
    gref(at + 4) = 0xE12FFF1E;        // bx lr
}
gaddr hle_stub(const char* name, HleFn fn) {
    std::lock_guard<std::mutex> lk(g_hle_mx);
    uint32_t idx = g_hle.size();
    if (idx >= STUB_MAX) fatal("too many HLE stubs");
    g_hle.push_back({name, fn});
    gaddr at = STUB_BASE + idx * 8;
    write_stub(at, idx);
    return at;
}
void hle_register(const char* name, HleFn fn) { hle_fns()[name] = fn; }
gaddr hle_lookup(const char* name) {
    { std::lock_guard<std::mutex> lk(g_hle_mx);
      auto s = g_hle_stubs.find(name); if (s != g_hle_stubs.end()) return s->second; }
    auto f = hle_fns().find(name);
    if (f == hle_fns().end()) return 0;
    gaddr a = hle_stub(name, f->second);
    std::lock_guard<std::mutex> lk(g_hle_mx);
    g_hle_stubs[name] = a;
    return a;
}
void hle_hook(gaddr fn, const char* name, HleFn h) {
    std::lock_guard<std::mutex> lk(g_hle_mx);
    uint32_t idx = g_hle.size();
    g_hle.push_back({name, h});
    write_stub(fn, idx);
}
void hle_hook_pre(gaddr fn, const char* name, HleFn h) {
    std::lock_guard<std::mutex> lk(g_hle_mx);
    uint32_t idx = g_hle.size();
    g_hle.push_back({name, h});
    gaddr t = gmalloc(24, 8);
    gref(t) = 0xEF000000 | idx;        // svc #idx -> our handler, then fall through
    gref(t + 4) = gref(fn);            // the two original instructions
    gref(t + 8) = gref(fn + 4);
    gref(t + 12) = 0xE51FF004;         // ldr pc, [pc, #-4]
    gref(t + 16) = fn + 8;             // back into the original
    gref(fn) = 0xE51FF004;             // original: ldr pc, [pc, #-4]
    gref(fn + 4) = t;                  //           .word trampoline
}
gaddr hle_hook_wrap(gaddr fn, const char* name, HleFn h) {
    gaddr t = gmalloc(16, 8);
    gref(t) = gref(fn);                // the two original instructions
    gref(t + 4) = gref(fn + 4);
    gref(t + 8) = 0xE51FF004;          // ldr pc, [pc, #-4]
    gref(t + 12) = fn + 8;
    hle_hook(fn, name, h);
    return t;
}
gaddr hle_data(const char* name) { auto d = g_hle_datas.find(name); return d == g_hle_datas.end() ? 0 : d->second; }
void hle_data_register(const char* name, gaddr a) { g_hle_datas[name] = a; }

// ---------------------------------------------------------------- CPU
struct GThread;
struct JitBox;
static thread_local GThread* t_self;
static thread_local JitBox* t_jit;    // innermost running jit on this thread
static ExclusiveMonitor g_monitor(256);
static std::atomic<uint32_t> g_next_cpu{0}, g_next_tid{1};

struct GThread {
    uint32_t tid;
    gaddr tls, stack_lo, stack_top;
    std::vector<std::unique_ptr<JitBox>> jits;  // one per host->guest nesting depth
    int depth = -1;
    // watchdog state (read from another thread)
    std::atomic<uint64_t> svc_count{0};
    std::atomic<const char*> in_hle{nullptr};
    std::atomic<JitBox*> running{nullptr};
    std::atomic<bool> dump_req{false};
    std::atomic<uint32_t> dump_pc{0}, dump_lr{0}, dump_seq{0};
    uint32_t nx_handle = 0;   // Switch thread handle, for the watchdog's register sampling
    int stuck_beats = 0;
};
static std::mutex g_threads_mx;
static std::vector<GThread*> g_threads;

struct Cp15 : A32::Coprocessor {
    uint32_t tpidruro = 0, tpidrurw = 0;
    std::optional<Callback> CompileInternalOperation(bool, unsigned, A32::CoprocReg, A32::CoprocReg, A32::CoprocReg, unsigned) override { return std::nullopt; }
    CallbackOrAccessOneWord CompileSendOneWord(bool, unsigned opc1, A32::CoprocReg CRn, A32::CoprocReg CRm, unsigned opc2) override {
        if (!opc1 && CRn == A32::CoprocReg::C13 && CRm == A32::CoprocReg::C0 && opc2 == 2) return &tpidrurw;
        if (!opc1 && CRn == A32::CoprocReg::C7 && CRm == A32::CoprocReg::C10) return Callback{[](void*, uint32_t, uint32_t) -> uint64_t { return 0; }, std::nullopt};  // barriers
        logf("cp15: unhandled MCR opc1=%u CRn=%d CRm=%d opc2=%u", opc1, (int)CRn, (int)CRm, opc2);
        return std::monostate{};
    }
    CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned, A32::CoprocReg) override { return std::monostate{}; }
    CallbackOrAccessOneWord CompileGetOneWord(bool, unsigned opc1, A32::CoprocReg CRn, A32::CoprocReg CRm, unsigned opc2) override {
        if (!opc1 && CRn == A32::CoprocReg::C13 && CRm == A32::CoprocReg::C0 && opc2 == 3) return &tpidruro;
        if (!opc1 && CRn == A32::CoprocReg::C13 && CRm == A32::CoprocReg::C0 && opc2 == 2) return &tpidrurw;
        if (!opc1 && CRn == A32::CoprocReg::C9 && CRm == A32::CoprocReg::C13 && opc2 == 0)  // PMCCNTR cycle counter
            return Callback{[](void*, uint32_t, uint32_t) -> uint64_t { return (uint32_t)std::chrono::steady_clock::now().time_since_epoch().count(); }, std::nullopt};
        logf("cp15: unhandled MRC opc1=%u CRn=%d CRm=%d opc2=%u", opc1, (int)CRn, (int)CRm, opc2);
        return std::monostate{};
    }
    CallbackOrAccessTwoWords CompileGetTwoWords(bool, unsigned, A32::CoprocReg) override { return std::monostate{}; }
    std::optional<Callback> CompileLoadWords(bool, bool, A32::CoprocReg, std::optional<uint8_t>) override { return std::nullopt; }
    std::optional<Callback> CompileStoreWords(bool, bool, A32::CoprocReg, std::optional<uint8_t>) override { return std::nullopt; }
};

[[noreturn]] static void guest_fault(const char* what, gaddr a);

struct Callbacks final : A32::UserCallbacks {
    JitBox* box;
    template <class T> T rd(gaddr a) { if (!g_page_table[a >> 12]) guest_fault("read", a); T v; memcpy(&v, g_mem + a, sizeof v); return v; }
    template <class T> void wr(gaddr a, T v) { if (!g_page_table[a >> 12]) guest_fault("write", a); memcpy(g_mem + a, &v, sizeof v); }
    template <class T> bool cas(gaddr a, T v, T expected) {
        if (!g_page_table[a >> 12]) guest_fault("excl-write", a);
        return __atomic_compare_exchange_n((T*)(g_mem + a), &expected, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    uint8_t MemoryRead8(A32::VAddr a) override { return rd<uint8_t>(a); }
    uint16_t MemoryRead16(A32::VAddr a) override { return rd<uint16_t>(a); }
    uint32_t MemoryRead32(A32::VAddr a) override { return rd<uint32_t>(a); }
    uint64_t MemoryRead64(A32::VAddr a) override { return rd<uint64_t>(a); }
    void MemoryWrite8(A32::VAddr a, uint8_t v) override { wr(a, v); }
    void MemoryWrite16(A32::VAddr a, uint16_t v) override { wr(a, v); }
    void MemoryWrite32(A32::VAddr a, uint32_t v) override { wr(a, v); }
    void MemoryWrite64(A32::VAddr a, uint64_t v) override { wr(a, v); }
    bool MemoryWriteExclusive8(A32::VAddr a, uint8_t v, uint8_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive16(A32::VAddr a, uint16_t v, uint16_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive32(A32::VAddr a, uint32_t v, uint32_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive64(A32::VAddr a, uint64_t v, uint64_t e) override { return cas(a, v, e); }
    void InterpreterFallback(A32::VAddr pc, size_t) override { guest_fault("interpreter fallback", pc); }
    void CallSVC(uint32_t swi) override;
    void ExceptionRaised(A32::VAddr pc, A32::Exception e) override {
        switch (e) {
        case A32::Exception::Yield: case A32::Exception::WaitForEvent: case A32::Exception::WaitForInterrupt:
            std::this_thread::yield();   // guest spin-wait: let the other thread run
            return;
        case A32::Exception::PreloadData: case A32::Exception::PreloadDataWithIntentToWrite:
        case A32::Exception::PreloadInstruction: case A32::Exception::SendEvent: case A32::Exception::SendEventLocal:
            return;
        default:
            guest_fault("cpu exception", pc);
        }
    }
    // Time slicing: Run() returns after ~SLICE guest instructions so the thread can yield.
    // Horizon never preempts equal-priority threads on a core, and guest code spin-waits.
    static constexpr int64_t SLICE = 1000000;
    int64_t ticks_left = SLICE;
    void AddTicks(uint64_t t) override { ticks_left -= (int64_t)t; }
    uint64_t GetTicksRemaining() override { return ticks_left > 0 ? (uint64_t)ticks_left : 0; }
};

struct JitBox {
    Callbacks cb;
    std::shared_ptr<Cp15> cp15 = std::make_shared<Cp15>();
    std::unique_ptr<A32::Jit> jit;
    bool returned = false;
    JitBox(gaddr tls, size_t cache) {
        cb.box = this;
        cp15->tpidruro = tls;
        A32::UserConfig c;
        c.callbacks = &cb;
        c.processor_id = g_next_cpu++ % 256;
        c.global_monitor = &g_monitor;
        c.arch_version = A32::ArchVersion::v7;
        c.page_table = &g_page_table;
        c.absolute_offset_page_table = true;
        c.coprocessors[15] = cp15;
        c.enable_cycle_counting = true;
        c.code_cache_size = cache;
        c.define_unpredictable_behaviour = true;
        c.hook_hint_instructions = true;  // report YIELD/WFE so spin-waits can yield
        jit = std::make_unique<A32::Jit>(c);
    }
};

uint32_t& reg(int i) { return t_jit->jit->Regs()[i]; }
uint64_t& dreg(int i) { return ((uint64_t*)t_jit->jit->ExtRegs().data())[i]; }
gaddr cur_tls() { return t_self->tls; }
void guest_set_pc(gaddr pc) { t_jit->jit->Regs()[15] = pc & ~1u; }
void guest_return_now(uint32_t r0) { t_jit->jit->Regs()[0] = r0; t_jit->returned = true; t_jit->jit->HaltExecution(); }
uint32_t cur_tid() { return t_self->tid; }

static void guest_fault(const char* what, gaddr a) {
    auto& r = t_jit->jit->Regs();
    logf("GUEST FAULT: %s at %08x  tid=%u  last hle=%s", what, a, t_self ? t_self->tid : 0, g_cur_hle);
    logf("  pc=%08x %s", r[15], addr_name(r[15]).c_str());
    logf("  lr=%08x %s", r[14], addr_name(r[14]).c_str());
    for (int i = 0; i < 13; i += 4)
        logf("  r%-2d=%08x r%-2d=%08x r%-2d=%08x r%-2d=%08x", i, r[i], i + 1, r[i + 1], i + 2, r[i + 2], i + 3, i + 3 < 16 ? r[i + 3] : 0);
    // crude backtrace: code-looking words on the stack
    gaddr sp = r[13];
    for (int n = 0, k = 0; n < 512 && k < 12; n++) {
        if (!g_page_table[(sp + 4 * n) >> 12]) break;
        gaddr v = gref(sp + 4 * n);
        GLib* l = lib_for_addr(v);
        if (l && v >= l->base) { logf("  stack[%d] %s", n, addr_name(v).c_str()); k++; }
    }
    fatal("guest crashed");
}

void bad_guest_pointer(gaddr a) { guest_fault("bad pointer passed to shim", a); }

void Callbacks::CallSVC(uint32_t swi) {
    if (swi == SVC_RET) { box->returned = true; box->jit->HaltExecution(); return; }
    if (swi >= g_hle.size()) guest_fault("unknown svc", reg(15) - 4);
    const char* prev = g_cur_hle;
    g_cur_hle = g_hle[swi].name.c_str();
    static const char* trace = getenv("GW3_TRACE");   // substring of the calling library's name
    GLib* caller = trace ? lib_for_addr(reg(14)) : nullptr;
    static const char* trace_fn = getenv("GW3_TRACE_FN");   // comma list of shim names to log
    if (trace_fn && strstr(trace_fn, g_cur_hle) && (trace_fn == strstr(trace_fn, g_cur_hle) || strstr(trace_fn, g_cur_hle)[-1] == ','))
        logf("[t%u] %s(%d, %d, %d, %d) <- %s", t_self->tid, g_cur_hle, (int)reg(0), (int)reg(1), (int)reg(2), (int)reg(3), addr_name(reg(14)).c_str());
    if (caller && caller->name.find(trace) != std::string::npos) logf("[t%u] %s(%08x, %08x, %08x) <- %s", t_self->tid, g_cur_hle, reg(0), reg(1), reg(2), addr_name(reg(14)).c_str());
    if (t_self->tid != 1 && g_cur_hle[0] == 'g' && g_cur_hle[1] == 'l') {   // GL off the render thread?
        static std::atomic<int> warned{0};
        if (warned++ < 20) logf("GL call %s from tid %u <- %s", g_cur_hle, t_self->tid, addr_name(reg(14)).c_str());
    }
    t_self->svc_count++;
    t_self->in_hle = g_cur_hle;
    Ctx c;
    g_hle[swi].fn(c);
    t_self->in_hle = nullptr;
    g_cur_hle = prev;
}

static GThread* new_thread(uint32_t stack_size) {
    auto* t = new GThread;
    { std::lock_guard<std::mutex> lk(g_threads_mx); g_threads.push_back(t); }
    t->tid = g_next_tid++;
    t->stack_lo = gmalloc(stack_size, 4096);
    t->stack_top = t->stack_lo + stack_size;
    // bionic TLS: [0]=self [1]=tid [2]=errno [5]=stack guard
    t->tls = gmalloc(64 * 4);
    memset(g2h(t->tls), 0, 256);
    gref(t->tls) = t->tls; gref(t->tls + 4) = t->tid;
    gref(t->tls + 20) = 0xC0FFEE00;
    return t;
}

uint64_t call_guest(gaddr fn, std::initializer_list<uint32_t> args) {
    GThread* t = t_self;
    int d = ++t->depth;
    if (d >= (int)t->jits.size())
        #ifdef __SWITCH__
        t->jits.push_back(std::make_unique<JitBox>(t->tls, d == 0 ? (t->tid == 1 ? 128u << 20 : 16u << 20) : 4u << 20));
#else
        t->jits.push_back(std::make_unique<JitBox>(t->tls, d == 0 ? 96u << 20 : 16u << 20));
#endif
    JitBox* box = t->jits[d].get();
    JitBox* outer = t_jit;
    auto& r = box->jit->Regs();
    gaddr sp = d == 0 ? t->stack_top : outer->jit->Regs()[13] - 256;
    sp &= ~7u;
    size_t n = args.size();
    if (n > 4) { sp -= ((n - 4) * 4 + 7) & ~7u; }
    size_t i = 0;
    for (uint32_t a : args) { if (i < 4) r[i] = a; else gref(sp + 4 * (i - 4)) = a; i++; }
    r[13] = sp; r[14] = g_ret_trap; r[15] = fn & ~1u;
    box->jit->SetCpsr(0x10 | ((fn & 1) ? 0x20 : 0));
    box->jit->SetFpscr(0);
    box->returned = false;
    t_jit = box;
    t->running = box;
    while (!box->returned) {
        box->cb.ticks_left = Callbacks::SLICE;
        box->jit->Run();
        static const bool noslice = getenv("GW3_NOSLICE") != nullptr;   // test knob
        if (!box->returned && box->cb.ticks_left <= 0 && !noslice) {   // slice used up: let other threads run
#ifdef __SWITCH__
            svcSleepThread(-1);   // yield, allowing migration to another core
#else
            std::this_thread::yield();
#endif
        }
        if (t->dump_req.exchange(false)) {   // watchdog asked where we are (it prints it)
            auto& rr = box->jit->Regs();
            t->dump_pc = rr[15]; t->dump_lr = rr[14]; t->dump_seq++;
        }
    }
    t->running = outer;
    t_jit = outer;
    --t->depth;
    return r[0] | (uint64_t)r[1] << 32;
}

void dump_guest_state() {
    if (!t_jit) { logf("  (no guest code running on this thread)"); return; }
    auto& r = t_jit->jit->Regs();
    logf("  guest tid=%u pc=%08x %s", t_self ? t_self->tid : 0, r[15], addr_name(r[15]).c_str());
    logf("  guest lr=%08x %s  last hle=%s", r[14], addr_name(r[14]).c_str(), g_cur_hle);
}

// Every 3 s, report threads that made no shim calls since the last check.
std::atomic<long> g_frame{0};
extern std::string g_data_dir;

// Heartbeat every 3 s into its own file (watchdog.log), so it works even if the main log's lock is
// held by a stuck thread. Threads with no shim calls since the last beat get asked for their guest PC.
void watchdog_start() {
    host_thread_start([] {
#ifdef __SWITCH__
        svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2B);   // above the game threads, so it always gets to run
#endif
        FILE* f = fopen((g_data_dir + "/watchdog.log").c_str(), "w");
        if (!f) return;
        std::unordered_map<GThread*, uint64_t> last;
        for (int beat = 0;; beat++) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            std::vector<GThread*> ts;
            if (g_threads_mx.try_lock()) { ts = g_threads; g_threads_mx.unlock(); }
            else fprintf(f, "  (thread list lock busy)\n");
            fprintf(f, "beat %d: frame %ld\n", beat, g_frame.load());
            for (GThread* t : ts) {
                uint64_t n = t->svc_count;
                uint64_t d = n - last[t];
                last[t] = n;
                const char* h = t->in_hle;
                if (d && beat % 10) continue;   // busy-and-progressing threads: only every 30 s
                fprintf(f, "  tid %u: +%llu calls, %s%s", t->tid, (unsigned long long)d, h ? "in " : "", h ? h : (t->running ? "in guest code" : "in host code"));
                if (t->dump_seq) fprintf(f, ", last guest pc %s lr %s", addr_name(t->dump_pc).c_str(), addr_name(t->dump_lr).c_str());
                fprintf(f, "\n");
                if (!d && !h) if (JitBox* b = t->running) { t->dump_req = true; b->jit->HaltExecution(); }
                t->stuck_beats = d ? 0 : t->stuck_beats + 1;
#ifdef __SWITCH__
                // Stuck and not answering: pause it and read its real CPU registers from the kernel.
                if (t->stuck_beats >= 2 && t->nx_handle && !t->dump_seq) {
                    ThreadContext ctx;
                    Result r1 = svcSetThreadActivity(t->nx_handle, ThreadActivity_Paused);
                    Result r2 = R_SUCCEEDED(r1) ? svcGetThreadContext3(&ctx, t->nx_handle) : r1;
                    if (R_SUCCEEDED(r1)) svcSetThreadActivity(t->nx_handle, ThreadActivity_Runnable);
                    extern char __start__;
                    u64 base = (u64)&__start__;
                    if (R_SUCCEEDED(r2)) {
                        auto off = [&](u64 v) { return v >= base && v < base + 0x2000000 ? v - base : v; };
                        fprintf(f, "    host pc=%s0x%lx lr=%s0x%lx sp=0x%lx\n", ctx.pc.x >= base && ctx.pc.x < base + 0x2000000 ? "elf+" : "",
                                off(ctx.pc.x), ctx.lr >= base && ctx.lr < base + 0x2000000 ? "elf+" : "", off(ctx.lr), ctx.sp);
                        u64* sp = (u64*)ctx.sp;   // code addresses on its stack: a rough backtrace
                        for (int i = 0, k = 0; i < 128 && k < 8; i++)
                            if (sp[i] > base && sp[i] < base + 0x2000000) { fprintf(f, "      stack elf+0x%lx\n", sp[i] - base); k++; }
                    } else fprintf(f, "    (could not read registers: 0x%x)\n", r2);
                }
#endif
            }
            fflush(f);
        }
    });
}

void guest_thread_adopt() { if (!t_self) t_self = new_thread(512u << 10); }

void thread_init_main() {
    g_hle.reserve(STUB_MAX);
    g_ret_trap = STUB_BASE + (STUB_MAX - 1) * 8;
    write_stub(g_ret_trap, SVC_RET);
    t_self = new_thread(8u << 20);
#ifdef __SWITCH__
    t_self->nx_handle = threadGetCurHandle();
#endif
}

// Host threads. On Switch go straight to libnx: 1 MiB stacks, normal priority, all cores
// (the pthread layer uses 128 KiB stacks, a low priority and one core).
void host_thread_start(std::function<void()> body) {
#ifdef __SWITCH__
    // pthreads (libnx expects its pthread bookkeeping on every thread), but with a 1 MiB stack;
    // each thread then lifts its own priority and core mask.
    auto* fn = new std::function<void()>(std::move(body));
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1u << 20);
    pthread_t th;
    // Horizon doesn't preempt equal-priority threads on one core, so a guest thread spin-waiting on
    // another on the same core hangs forever: give every thread its own ideal core (0..2, round robin).
    static std::atomic<int> next_core{1};
    int core = next_core++ % 3;
    auto* args = new std::pair<std::function<void()>*, int>(fn, core);
    int rc = pthread_create(&th, &attr, [](void* p) -> void* {
        auto* a = (std::pair<std::function<void()>*, int>*)p;
        u64 mask = 0;
        svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
        if (!(mask & (1u << a->second))) mask |= 1u << a->second;
        svcSetThreadCoreMask(CUR_THREAD_HANDLE, a->second, (u32)mask);
        svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2C);
        auto* f = a->first;
        delete a;
        (*f)();
        delete f;
        return nullptr;
    }, args);
    if (rc) fatal("pthread_create failed: %d (%s)", rc, strerror(rc));
    pthread_detach(th);
#else
    std::thread(std::move(body)).detach();
#endif
}

uint32_t guest_thread_spawn(gaddr fn, gaddr arg, uint32_t stack_size) {
    GThread* t = new_thread(stack_size < (256u << 10) ? (1u << 20) : stack_size);
    uint32_t tid = t->tid;
    host_thread_start([t, fn, arg] {
        t_self = t;
#ifdef __SWITCH__
        t->nx_handle = threadGetCurHandle();
#endif
        logf("thread %u running", t->tid);
        call_guest(fn, {arg});
        // ponytail: thread structures leak on exit (few threads, all long-lived)
        extern void pthread_exit_hook(uint32_t tid, uint32_t ret);
        pthread_exit_hook(t->tid, t->jits[0]->jit->Regs()[0]);
    });
    return tid;
}

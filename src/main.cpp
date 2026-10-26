// Boots Geometry Wars 3 (Android 1.0.0, armeabi-v7a) on the host by driving the same
// GW3JNILib lifecycle the Java GW3Activity would.
#include "core.hpp"
#include "overlay.hpp"

#include <SDL2/SDL.h>
#ifdef __SWITCH__
#include <switch.h>
#endif
#include <GLES2/gl2.h>
#include <string>
#include <vector>
#include <unistd.h>
#include <exception>
#include <atomic>
#include <algorithm>
#include <mutex>
#include <typeinfo>

extern std::string g_data_dir;
int g_screen_w = 1280, g_screen_h = 720;

static void ret0(Ctx& c) { c.ret(0); }
static std::atomic<long> g_detail_calls{0};
static void ret1(Ctx& c) { c.ret(1); }
// Engine pad layout (reverse-engineered by gw3-portmaster): g_JoypadStates = 4 pads x 9 words,
// word 0 = button mask, words 1..8 = float axes LX LY RX RY LTrig RTrig HatY HatX.
// Mask bits: 0 A 1 B 2 X 3 Y 4 L1 5 R1 6 L2 7 R2 8 Up 9 Down 10 Left 11 Right 12 Start 13 Back.
// Up to 4 controllers, one per player (local co-op in 1.3.0's console UI).
static constexpr int MAXPADS = 4;
static SDL_GameController* g_pads[MAXPADS];
static uint32_t g_pad_mask_n[MAXPADS];
static float g_pad_axis_n[MAXPADS][8];
static int g_npads = 1;   // what we report; at least 1 so the single-player UI never asks to reconnect
#define g_pad_mask g_pad_mask_n[0]
#define g_pad_axis g_pad_axis_n[0]
static bool g_pads_live[MAXPADS];
static void poll_pad() {
    // keep the pad list compact: drop detached pads, open newly attached ones
    for (int p = 0; p < MAXPADS; p++)
        if (g_pads[p] && !SDL_GameControllerGetAttached(g_pads[p])) { SDL_GameControllerClose(g_pads[p]); g_pads[p] = nullptr; }
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (!SDL_IsGameController(i)) continue;
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
        bool open = false;
        for (int p = 0; p < MAXPADS; p++) if (g_pads[p] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pads[p])) == id) open = true;
        if (open) continue;
        for (int p = 0; p < MAXPADS; p++) if (!g_pads[p]) { g_pads[p] = SDL_GameControllerOpen(i); logf("input: controller %d = %s", p, SDL_GameControllerName(g_pads[p])); break; }
    }
    // SDL's Switch mapping is positional (Nintendo B = "A"); swap so each button matches its label and
    // the game's A/B/X/Y prompts are right (confirm = A, back = B, Nintendo-style).
#ifdef __SWITCH__
    static const SDL_GameControllerButton map[14] = {SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_Y,
        SDL_CONTROLLER_BUTTON_X,
#else
    static const SDL_GameControllerButton map[14] = {SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X,
        SDL_CONTROLLER_BUTTON_Y,
#endif
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_CONTROLLER_BUTTON_INVALID,
        SDL_CONTROLLER_BUTTON_INVALID, SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT,
        SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_BACK};
    static const SDL_GameControllerAxis ax[] = {SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, SDL_CONTROLLER_AXIS_RIGHTX,
        SDL_CONTROLLER_AXIS_RIGHTY, SDL_CONTROLLER_AXIS_TRIGGERLEFT, SDL_CONTROLLER_AXIS_TRIGGERRIGHT};
#ifdef __SWITCH__
    // SDL always lists 4 Switch controllers; only count ones that really are connected (pad 0 = handheld/P1)
    for (int p = 1; p < MAXPADS; p++)
        if (g_pads[p] && !hidGetNpadStyleSet((HidNpadIdType)(HidNpadIdType_No1 + p))) g_pads_live[p] = false; else g_pads_live[p] = g_pads[p] != nullptr;
#else
    for (int p = 0; p < MAXPADS; p++) g_pads_live[p] = g_pads[p] != nullptr;
#endif
    g_pads_live[0] = true;
    int n = 0;
    for (int p = 0; p < MAXPADS; p++) {
        uint32_t& m = g_pad_mask_n[p]; float* av = g_pad_axis_n[p];
        m = 0; memset(av, 0, 8 * sizeof(float));
        SDL_GameController* pad = g_pads[p];
        if (!pad || !g_pads_live[p]) continue;
        n = p + 1;
        for (int b = 0; b < 14; b++) if (map[b] != SDL_CONTROLLER_BUTTON_INVALID && SDL_GameControllerGetButton(pad, map[b])) m |= 1u << b;
        for (int a = 0; a < 6; a++) av[a] = SDL_GameControllerGetAxis(pad, ax[a]) / 32767.f;
        if (av[4] > 0.3f) m |= 1 << 6;
        if (av[5] > 0.3f) m |= 1 << 7;
    }
    g_npads = n > 0 ? n : 1;
}
// ---- rumble. The Android build stripped all rumble calls (InputDevice::SetRumble is an empty stub),
// so game events hooked below queue a rumble here and the main loop plays it on every live pad.
// ponytail: every pad rumbles for every event; per-player rumble needs the Player -> pad mapping.
static std::mutex g_rumble_mx;
static float g_rumble_strong, g_rumble_weak;   // 0..1
static uint32_t g_rumble_until;
static void rumble_kick(float strong, float weak, uint32_t ms) {
    std::lock_guard<std::mutex> lk(g_rumble_mx);
    uint32_t now = SDL_GetTicks();
    logf("rumble %.1f/%.1f %u ms", strong, weak, ms);
    if (now >= g_rumble_until) g_rumble_strong = g_rumble_weak = 0;
    g_rumble_strong = std::max(g_rumble_strong, strong); g_rumble_weak = std::max(g_rumble_weak, weak);
    g_rumble_until = std::max(g_rumble_until, now + ms);
}
static void rumble_update() {
    float s = 0, w = 0;
    {
        std::lock_guard<std::mutex> lk(g_rumble_mx);
        if (SDL_GetTicks() < g_rumble_until) { s = g_rumble_strong * gw_settings().rumble; w = g_rumble_weak * gw_settings().rumble; }
    }
    static float last_s = -1, last_w = -1;
    if (s == last_s && w == last_w) return;
    last_s = s; last_w = w;
#ifdef __SWITCH__
    // HD Rumble: low band carries the "strong" thump, high band the "weak" buzz
    static HidVibrationDeviceHandle handles[MAXPADS][2];
    static u32 styles[MAXPADS];
    for (int p = 0; p < MAXPADS; p++) {
        if (!g_pads_live[p]) continue;
        HidNpadIdType id = p == 0 && (hidGetNpadStyleSet(HidNpadIdType_Handheld) & HidNpadStyleTag_NpadHandheld) ? HidNpadIdType_Handheld
                                                                                                                 : (HidNpadIdType)(HidNpadIdType_No1 + p);
        u32 st = hidGetNpadStyleSet(id);
        u32 tag = st & HidNpadStyleTag_NpadHandheld ? HidNpadStyleTag_NpadHandheld : st & HidNpadStyleTag_NpadFullKey ? HidNpadStyleTag_NpadFullKey
                : st & HidNpadStyleTag_NpadJoyDual ? HidNpadStyleTag_NpadJoyDual : st & HidNpadStyleTag_NpadJoyLeft ? HidNpadStyleTag_NpadJoyLeft
                : st & HidNpadStyleTag_NpadJoyRight ? HidNpadStyleTag_NpadJoyRight : 0;
        if (!tag) continue;
        int n = tag == HidNpadStyleTag_NpadJoyLeft || tag == HidNpadStyleTag_NpadJoyRight ? 1 : 2;
        if (styles[p] != (tag | (u32)id << 16)) {
            if (R_FAILED(hidInitializeVibrationDevices(handles[p], n, id, (HidNpadStyleTag)tag))) continue;
            styles[p] = tag | (u32)id << 16;
        }
        HidVibrationValue v[2];
        for (auto& x : v) { x.amp_low = s; x.freq_low = 160.0f; x.amp_high = w; x.freq_high = 320.0f; }
        hidSendVibrationValues(handles[p], v, n);
    }
#else
    for (int p = 0; p < MAXPADS; p++)
        if (g_pads[p]) SDL_GameControllerRumble(g_pads[p], (Uint16)(s * 65535), (Uint16)(w * 65535), s || w ? 1000 : 0);
#endif
}

// ---- auto-pause: tell the game it went to the background (Android onPause) on HOME / sleep
static std::atomic<int> g_want_focus{1};
#ifdef __SWITCH__
static AppletHookCookie g_focus_cookie;
static void on_applet(AppletHookType t, void*) {
    if (t == AppletHookType_OnFocusState) g_want_focus = appletGetFocusState() == AppletFocusState_InFocus;
}
#endif

static void joy_buttons(Ctx& c) { int p = c.i(0); c.ret(p >= 0 && p < MAXPADS ? g_pad_mask_n[p] : 0); }
static void joy_axis(Ctx& c) { int p = c.i(0); c.retf(p >= 0 && p < MAXPADS && c.u(1) < 8 ? g_pad_axis_n[p][c.u(1)] : 0.f); }

static void write_ppm(const char* path, int w, int h) {
    std::vector<uint8_t> px(w * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    FILE* f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; y--) for (int x = 0; x < w; x++) fwrite(&px[(y * w + x) * 4], 1, 3, f);
    fclose(f);
}

void dump_guest_state();

#ifdef __SWITCH__
#include <switch.h>
// Host crash handler: log PC/LR as offsets into gw3nx.elf (map with addr2line) plus the guest location.
extern "C" {
alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
extern char __start__;
// abort()/assert() normally end the process silently (Mesa, libstdc++): log who called them first.
void __real_exit(int);
void __real__exit(int);
void __wrap_exit(int code) {
    fprintf(stderr, "EXIT: exit(%d) called from elf+0x%lx\n", code, (uintptr_t)__builtin_return_address(0) - (uintptr_t)&__start__);
    fflush(stderr); __real_exit(code);
}
void __wrap__exit(int code) {
    fprintf(stderr, "EXIT: _exit(%d) called from elf+0x%lx\n", code, (uintptr_t)__builtin_return_address(0) - (uintptr_t)&__start__);
    fflush(stderr); __real__exit(code);
}
void abort(void) {
    u64 base = (u64)&__start__;
    fprintf(stderr, "FATAL: abort() called from elf+0x%lx <- elf+0x%lx\n",
            (u64)__builtin_return_address(0) - base, (u64)__builtin_extract_return_addr(__builtin_return_address(0)) - base);
    dump_guest_state();
    fflush(stderr);
    svcExitProcess();
    __builtin_unreachable();
}
void __assert_func(const char* file, int line, const char* func, const char* expr) {
    fprintf(stderr, "FATAL: assert failed: %s:%d %s: %s\n", file, line, func ? func : "", expr);
    abort();
}
void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    static bool in_handler = false;
    if (in_handler) svcExitProcess();
    in_handler = true;
    // build the whole report first, then write it once
    static char buf[4096];
    int n = 0;
    u64 base = (u64)&__start__;
    auto add = [&](const char* fmt, auto... a) { n += snprintf(buf + n, sizeof buf - n, fmt, a...); };
    add("HOST CRASH: type=0x%x far=%016lx\n  pc=%016lx (elf+0x%lx)\n  lr=%016lx (elf+0x%lx)\n  sp=%016lx\n",
        ctx->error_desc, ctx->far.x, ctx->pc.x, ctx->pc.x - base, ctx->lr.x, ctx->lr.x - base, ctx->sp.x);
    for (int i = 0; i < 29; i++) add("  x%d=%016lx%s", i, ctx->cpu_gprs[i].x, i % 4 == 3 ? "\n" : "");
    add("\n");
    u64* sp = (u64*)ctx->sp.x;
    for (int i = 0, k = 0; i < 256 && k < 16; i++) {
        u64 v = sp[i];
        if (v > base && v < base + 0x1000000) { add("  stack[%d] elf+0x%lx\n", i, v - base); k++; }
    }
    fwrite(buf, 1, n, stderr);
    fflush(stderr);
    dump_guest_state();
    fflush(stderr);
    fclose(stderr);
    svcExitProcess();
}
}
#endif

int main(int argc, char** argv) {
    // usage: gw3nx <dir with libgwnext.so, libfmodex.so, libfmodevent.so, main.<ver>...obb> [frames] [shot.ppm]
#ifdef __SWITCH__
    g_data_dir = "/switch/gw3";
    freopen("/switch/gw3/gw3.log", "w", stderr);
    setvbuf(stderr, nullptr, _IOLBF, 0);
    argc = 1;
#else
    if (argc < 2) { fprintf(stderr, "usage: %s <game dir> [frames] [screenshot.ppm]\n", argv[0]); return 1; }
    g_data_dir = argv[1];
#endif
    long max_frames = argc > 2 ? atol(argv[2]) : -1;
    const char* shot = argc > 3 ? argv[3] : nullptr;

    std::set_terminate([] {
        const char* what = "unknown";
        std::string w;
        try { if (auto e = std::current_exception()) std::rethrow_exception(e); }
        catch (const std::exception& e) { w = std::string(typeid(e).name()) + ": " + e.what(); what = w.c_str(); }
        catch (...) {}
        logf("FATAL: uncaught C++ exception: %s", what);
        dump_guest_state();
        abort();
    });
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER)) fatal("SDL_Init: %s", SDL_GetError());
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    // 1080p when docked, 720p handheld; SDL resizes a resizable window on dock changes (see the event loop).
#ifdef __SWITCH__
    if (appletGetOperationMode() == AppletOperationMode_Console) { g_screen_w = 1920; g_screen_h = 1080; }
#endif
    if (const char* r = getenv("GW3_RES")) sscanf(r, "%dx%d", &g_screen_w, &g_screen_h);
    int win_w = g_screen_w, win_h = g_screen_h;
    // debug: GW3_GAMERES=1280x720 makes the game render smaller than the window (the state after docking)
    if (const char* r = getenv("GW3_GAMERES")) sscanf(r, "%dx%d", &g_screen_w, &g_screen_h);
    SDL_Window* win = SDL_CreateWindow("Geometry Wars 3", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!win) fatal("SDL_CreateWindow: %s", SDL_GetError());
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) fatal("SDL_GL_CreateContext: %s", SDL_GetError());
    logf("GL: %s / %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    extern int g_drawable_w, g_drawable_h;
    SDL_GL_GetDrawableSize(win, &g_drawable_w, &g_drawable_h);
    logf("drawable %dx%d", g_drawable_w, g_drawable_h);

    mem_init();
    thread_init_main();
#ifdef __SWITCH__
    watchdog_start();
#else
    if (getenv("GW3_WATCHDOG")) watchdog_start();
#endif
    hle_libc_init();
    hle_gl_init();
    jni_init();
    hle_android_init();
    hle_opensl_init();

    std::string d = g_data_dir + "/";
    GLib* fmodex = load_lib((d + "libfmodex.so").c_str(), 0x12000000);
    GLib* fmodev = load_lib((d + "libfmodevent.so").c_str(), 0x12400000);
    GLib* gw = load_lib((d + "libgwnext.so").c_str(), 0x10000000);

    // --- game fixes (found by the Linux handheld port, gw3-portmaster) ---
    auto hook = [&](const char* sym, HleFn f) { gaddr a = lib_sym(gw, sym); if (a) hle_hook(a, sym, f); else logf("hook: %s not found", sym); };
    if (getenv("GW3_NOAUDIO")) { hook("_ZN5Audio4InitEv", ret0); hook("_ZN5Audio6UpdateEv", ret0); }
    hook("_Z12GetNoJoypadsv", [](Ctx& c) { c.ret(g_npads); });
    // Rumble on the moments the console versions rumble for
    auto hook_pre = [&](const char* sym, HleFn f) { gaddr a = lib_sym(gw, sym); if (a) hle_hook_pre(a, sym, f); else logf("hook: %s not found", sym); };
    hook_pre("_ZN9SmartBombC2ERKN5Maths6VectorEP10EntityUnit13SmartBombTypeffS5_", [](Ctx&) { rumble_kick(0.9f, 0.6f, 700); });
    hook_pre("_ZN6Player12TriggerDeathEN6Netify9LiteObjIdE", [](Ctx&) { rumble_kick(1.0f, 0.8f, 450); });
    hook_pre("_ZN6C_Game14TriggerHitBossEi10DamageType", [](Ctx&) { rumble_kick(0.4f, 0.5f, 120); });
    hook_pre("_ZN4Boss13DeathSequenceEv", [](Ctx&) { rumble_kick(1.0f, 1.0f, 1500); });
    // debug: GW3_TESTPRE runs a pre-hook on every frame, to exercise the trampoline path
    static std::atomic<long> pre_calls{0};
    if (getenv("GW3_TESTPRE")) hook_pre("Java_com_activision_gw3_common_GW3JNILib_viewOnDrawFrame", [](Ctx&) { if (++pre_calls % 120 == 0) logf("pre-hook ok: %ld calls", pre_calls.load()); });
    // Achievements: kept on the SD card instead of Google Play (achievements.txt, shown in the overlay)
    hook("_ZN8Services24PlayerCanGetAchievementsEi", [](Ctx& c) { c.ret(1); });
    hook("_ZN8Services27RegisterAchievementProgressEi13E_Achievementf", [](Ctx& c) { if (c.f(2) >= 1.0f) overlay_achievement(c.i(1), SDL_GetTicks()); });
    hook("_ZN8Services18ShowAchievementsUIEi", [](Ctx&) { if (getenv("GW3_STOCK") || !options_show_achievements()) overlay_show_achievements(); });
    // debug: GW3_STOCK leaves the menus as the Android build had them (for before/after screenshots)
    if (!getenv("GW3_STOCK")) {
        options_install(gw);
        // Google Play is gone: hide the "SIGN IN (X)" prompt on the main menu (and Options)
        static gaddr set_visible = lib_sym(gw, "_ZN6UINode10SetVisibleEb");
        hook("_ZN21LeaderboardSignInNode6UpdateEf", [](Ctx& c) { call_guest(set_visible, {c.u(0), 0}); });
        hook("_ZN14AppStoreButton6UpdateEf", [](Ctx& c) { call_guest(set_visible, {c.u(0), 0}); });
    }
    // Effects detail (particles / background), read through these getters; see g_detail
    hook("_ZN16C_DetailSettings21GetParticleComplexityEv", [](Ctx& c) { g_detail_calls++; float p, b; overlay_detail(p, b); c.retf(p); });
    hook("_ZN16C_DetailSettings23GetBackgroundComplexityEv", [](Ctx& c) { g_detail_calls++; float p, b; overlay_detail(p, b); c.retf(b); });
    hook("_ZN12UserControls27GetMasterUserControllerTypeEv", ret0);
    hook("_Z17IsJoypadConnectedi", [](Ctx& c) { int p = c.i(0); c.ret(p == 0 || (p > 0 && p < MAXPADS && g_pads_live[p])); });
    hook("_Z16GetJoypadButtonsi", joy_buttons);
    hook("_Z13GetJoypadAxisii", joy_axis);
    // telemetry recorder methods called with this == NULL (their Java side doesn't exist)
    static const uint32_t evt_rec[] = {0x225e04, 0x225fc0, 0x226098, 0x2261c0, 0x2262e4, 0x2263bc, 0x2264c8, 0x2265d4, 0x2266c4,
        0x2267fc, 0x226960, 0x226a58, 0x226b80, 0x226c58, 0x226d30, 0x226e68, 0x226fcc, 0x2270c4, 0x22719c, 0x2272c4, 0x2273ec,
        0x227534, 0x22760c, 0x2276e4, 0x2277bc};
    // These are Audio::C_AudioSystem command methods (TriggerOneShotSound, BeginMusicCue, ...), not
    // telemetry: they only crashed (this == NULL) when the audio system wasn't initialised. Keep the
    // stubs only for the silent mode.
    if (getenv("GW3_NOAUDIO")) for (uint32_t off : evt_rec) hle_hook(gw->base + off, "audio_cmd_stub", ret0);

    lib_run_init(fmodex);
    lib_run_init(fmodev);
    lib_run_init(gw);
    if (gaddr p = lib_sym(gw, "IsAndroidPaused")) gref(p) = 0;
    // 1.3.0 keeps its controller UI under Console/ and only uses it on Android TV: be a TV.
    extern bool g_tv_mode;
    g_tv_mode = getenv("GW3_TV") || (lib_sym(gw, "Java_com_activision_gw3_common_GW3JNILib_setJoystickConnected") && !getenv("GW3_PHONE"));
    logf("ui: %s mode", g_tv_mode ? "TV/console" : "phone");
    gaddr onload = lib_sym(gw, "JNI_OnLoad");
    logf("JNI_OnLoad -> %08x", (uint32_t)call_guest(onload, {jni_vm(), 0}));

    auto J = [&](const char* n) {
        gaddr a = lib_sym(gw, (std::string("Java_com_activision_gw3_common_GW3JNILib_") + n).c_str());
        if (!a) logf("missing %s", n);
        return a;
    };
    gaddr env = jni_env(), thiz = jni_obj("GW3JNILib"), cfg = jni_obj("GameConfig"), am = jni_obj("AssetManager");
    gaddr dir = jni_new_string((g_data_dir + "/").c_str());
    float inches = 6.2f; uint32_t inch_bits; memcpy(&inch_bits, &inches, 4);
    auto step = [&](const char* n, std::initializer_list<uint32_t> a) {
        gaddr f = J(n); if (!f) return;
        logf("lifecycle: %s", n);
        call_guest(f, a);
    };
    step("onAppCreated", {env, thiz});
    step("setAssetManager", {env, thiz, am});
    step("setDeviceName", {env, thiz, jni_new_string("Nintendo Switch")});
    step("setScreenSizeInches", {env, thiz, inch_bits});
    step("setDeviceScreenSize", {env, thiz, (uint32_t)g_screen_w, (uint32_t)g_screen_h});
    step("setAppWindowSize", {env, thiz, (uint32_t)g_screen_w, (uint32_t)g_screen_h});
    step("setGameFilesDir", {env, thiz, dir});
    step("setPrivateFilesDir", {env, thiz, dir});
    step("setNoJoysticks", {env, thiz, 1});
    if (J("setJoystickConnected")) step("setJoystickConnected", {env, thiz, 0, 1});   // 1.3.0: pad 0 present
    if (gaddr p = lib_sym(gw, "g_NoJoypads")) gref(p) = 1;
    step("viewInitGameConfig", {env, thiz, cfg});
    step("viewOnSurfaceCreated", {env, thiz});
    step("viewOnInit", {env, thiz});
    step("viewOnSurfaceChanged", {env, thiz, (uint32_t)g_screen_w, (uint32_t)g_screen_h});
    step("viewOnResume", {env, thiz});

    gaddr draw = J("viewOnDrawFrame");
    gaddr joy_states = lib_sym(gw, "g_JoypadStates");
    gaddr set_conn = J("setJoystickConnected");      // 1.3.0 only
    uint32_t pad_stride = set_conn ? 40 : 36;         // 1.3.0 added a per-pad "connected" byte
    bool pad_was[MAXPADS] = {true};
    overlay_init();
#ifdef __SWITCH__
    appletHook(&g_focus_cookie, on_applet, nullptr);
#endif
    logf("lifecycle: entering draw loop");
    for (long frame = 0; max_frames < 0 || frame < max_frames; frame++) {
        SDL_Event e;
        bool quit = false;
        bool resized = false;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) quit = true;
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) g_want_focus = 0;
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) g_want_focus = 1;
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) resized = true;
        }
        if (quit) break;
        // debug: GW3_PAUSETEST="900:1100" fakes losing focus at 900 and getting it back at 1100
        if (const char* r = getenv("GW3_PAUSETEST")) { long a, b; if (sscanf(r, "%ld:%ld", &a, &b) == 2) { if (frame == a) g_want_focus = 0; if (frame == b) g_want_focus = 1; } }
        {
            static bool focused = true;
            if (focused != (bool)g_want_focus) {
                focused = g_want_focus;
                logf("lifecycle: %s", focused ? "viewOnResume (focus back)" : "viewOnPause (focus lost)");
                call_guest(J(focused ? "viewOnResume" : "viewOnPause"), {env, thiz});
                if (gaddr p = lib_sym(gw, "IsAndroidPaused")) gref(p) = 0;   // keep the engine drawing (as at startup)
            }
        }
        // debug: GW3_RESIZE="600:1920x1080" resizes the window at frame 600 (the dock/undock path)
        if (const char* r = getenv("GW3_RESIZE")) { long f; int w, h; if (sscanf(r, "%ld:%dx%d", &f, &w, &h) == 3 && f == frame) SDL_SetWindowSize(win, w, h); }
        if (resized) {
            SDL_GL_GetDrawableSize(win, &g_drawable_w, &g_drawable_h);
            // The engine sizes its render targets once at startup and mis-renders if told about a new size,
            // so keep its resolution and only stretch the final image (the screen viewport) to the new size.
            logf("display: now %dx%d (game keeps rendering at %dx%d until restart)", g_drawable_w, g_drawable_h, g_screen_w, g_screen_h);
        }
        poll_pad();
        // debug: GW3_SCRIPT="300:0,420:0" presses engine button bit N for 6 frames at each frame number;
        // GW3_SHOTS="600,900" writes shot_<frame>.ppm
        if (const char* sc = getenv("GW3_SCRIPT"))
            for (const char* q = sc; *q;) {
                long f = strtol(q, (char**)&q, 10); int bit = *q == ':' ? strtol(q + 1, (char**)&q, 10) : 0;
                long len = *q == ':' ? strtol(q + 1, (char**)&q, 10) : (bit >= 20 ? 60 : 6);   // optional ":frames"
                if (bit < 20 && frame >= f && frame < f + len) g_pad_mask |= 1u << bit;
                if (bit >= 20 && frame >= f && frame < f + len) g_pad_axis[bit - 20] = 0.9f;  // 20+axis: hold an axis
                if (*q == ',') q++; else break;
            }
        if (overlay_input(g_pad_mask_n, MAXPADS, SDL_GetTicks())) memset(g_pad_axis_n, 0, sizeof g_pad_axis_n);
        // The console menus only follow the left stick (the d-pad bits are ignored there), and in a level
        // the left stick is movement too: mirror the d-pad onto the stick so it works everywhere.
        for (int p = 0; p < MAXPADS; p++) {
            uint32_t m = g_pad_mask_n[p];
            float* av = g_pad_axis_n[p];
            av[0] = std::clamp(av[0] + (float)(!!(m & 1u << 11) - !!(m & 1u << 10)), -1.0f, 1.0f);
            av[1] = std::clamp(av[1] + (float)(!!(m & 1u << 9) - !!(m & 1u << 8)), -1.0f, 1.0f);
        }
        if (joy_states)
            for (int p = 0; p < MAXPADS; p++) {
                gaddr st = joy_states + p * pad_stride;
                gref(st) = g_pad_mask_n[p]; memcpy(g2h(st + 4), g_pad_axis_n[p], 32);
            }
        if (set_conn)   // tell the console UI about controllers coming and going
            for (int p = 0; p < MAXPADS; p++) {
                bool now = p == 0 || g_pads_live[p];
                if (now != pad_was[p]) { call_guest(set_conn, {env, thiz, (uint32_t)p, now ? 1u : 0u}); pad_was[p] = now; logf("input: player %d %s", p + 1, now ? "connected" : "disconnected"); }
            }
        if (getenv("GW3_DUMPVARS") && frame % 300 == 0)
            for (const char* v : {"dvbDetailSettings_Mobile_ParticleComplexity", "dvbDetailSettings_Mobile_BackgroundComplexity",
                                  "dvbDetailSettings_Default_ParticleComplexity", "dvbDetailSettings_Particle_ComplexityMin", "gMaxParticles"})
                if (gaddr a = lib_sym(gw, v)) { float f; memcpy(&f, g2h(a), 4); logf("var %s = %f / %d", v, f, (int)gref(a)); }
        if (getenv("GW3_OPTLOG") && frame == 400) options_log_labels();
        // debug: GW3_ACHTEST="900:5" unlocks achievement 5 at frame 900 (tests the pop-up and the list)
        if (const char* r = getenv("GW3_ACHTEST")) { long f; int i; if (sscanf(r, "%ld:%d", &f, &i) == 2 && f == frame) overlay_achievement(i, SDL_GetTicks()); }
        uint32_t t0 = SDL_GetTicks();
        { extern std::atomic<long> g_frame; g_frame = frame; }
        { extern void jni_run_posted(); jni_run_posted(); }
        call_guest(draw, {env, thiz});
        overlay_draw(g_drawable_w, g_drawable_h, SDL_GetTicks());
        rumble_update();
        if (getenv("GW3_SHAKELOG")) {
            static gaddr sx = lib_sym(gw, "gMapShakeX"), sy = lib_sym(gw, "gMapShakeY");
            float x, y; memcpy(&x, g2h(sx), 4); memcpy(&y, g2h(sy), 4);
            if (x != 0 || y != 0) logf("shake %ld: %f %f", frame, x, y);
        }
        if (frame % 60 == 0) { extern uint32_t guest_heap_used(); logf("frame %ld (%u ms) guest heap %u MiB | totals: shaders %llu ms, asset reads %llu ms", frame, SDL_GetTicks() - t0, guest_heap_used() >> 20, (unsigned long long)(g_t_shader_us / 1000), (unsigned long long)(g_t_asset_us / 1000)); }
        if (shot && frame == max_frames - 1) write_ppm(shot, g_drawable_w, g_drawable_h);
        if (const char* sh = getenv("GW3_SHOTS"))
            for (const char* q = sh; *q;) {
                long f = strtol(q, (char**)&q, 10);
                if (f == frame) { char n[64]; snprintf(n, sizeof n, "shot_%ld.ppm", frame); write_ppm(n, g_drawable_w, g_drawable_h); }
                if (*q == ',') q++; else break;
            }
        SDL_GL_SwapWindow(win);
    }
    logf("done");
    fflush(stderr);
    _exit(0);   // guest threads are still running: skip static destructors (libnx still closes services)
}

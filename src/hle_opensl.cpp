// Fake libOpenSLES.so for FMOD: the guest gets OpenSL objects whose vtables are HLE stubs.
// FMOD enqueues PCM blocks on an Android simple buffer queue; a pump thread copies each block into
// an SDL audio queue and then calls FMOD's (guest) buffer-queue callback so it enqueues the next.
// Interface slots used were mapped by the gw3-portmaster project (fmod_opensl.c).
#include "core.hpp"

#include <SDL2/SDL.h>
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

enum Kind { ENGINE_OBJ, MIX_OBJ, PLAYER_OBJ, ENGINE_ITF, PLAY_ITF, BQ_ITF, CFG_ITF, NKIND };
static gaddr g_obj[NKIND];
static gaddr g_iid_engine, g_iid_play, g_iid_bq, g_iid_cfg, g_iid_record, g_iid_outmix;
static gaddr g_slCreateEngine;

static std::mutex g_qmx;
static std::deque<std::pair<gaddr, uint32_t>> g_queue;  // guest PCM blocks enqueued by FMOD
static gaddr g_cb, g_cb_ctx;
static int g_freq = 48000, g_chans = 2;
static SDL_AudioDeviceID g_dev;
static std::atomic<bool> g_playing{false};

static void pump() {
    guest_thread_adopt();
    for (;;) {
        if (!g_playing || !g_dev) { SDL_Delay(10); continue; }
        // keep ~60 ms queued
        if (SDL_GetQueuedAudioSize(g_dev) > (uint32_t)(g_freq * g_chans * 2 * 60 / 1000)) { SDL_Delay(2); continue; }
        std::pair<gaddr, uint32_t> blk{0, 0};
        { std::lock_guard<std::mutex> lk(g_qmx); if (!g_queue.empty()) { blk = g_queue.front(); g_queue.pop_front(); } }
        if (blk.first) SDL_QueueAudio(g_dev, g2h(blk.first), blk.second);
        if (g_cb) call_guest(g_cb, {g_obj[BQ_ITF], g_cb_ctx});
        if (!blk.first) SDL_Delay(2);
    }
}

static void open_device() {
    if (g_dev) return;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) { logf("opensl: SDL audio init failed: %s", SDL_GetError()); return; }
    SDL_AudioSpec want{}, have{};
    want.freq = g_freq; want.format = AUDIO_S16SYS; want.channels = g_chans; want.samples = 1024;
    g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!g_dev) { logf("opensl: SDL_OpenAudioDevice: %s", SDL_GetError()); return; }
    logf("opensl: audio %d Hz, %d ch", have.freq, have.channels);
    host_thread_start(pump);
}

static void sl_dispatch(Ctx& c) {
    std::string n = g_cur_hle;   // "SL<kind>_<slot>"
    int kind = n[2] - '0', slot = atoi(n.c_str() + 4);
    gaddr self = c.u(0);
    switch (kind) {
    case ENGINE_OBJ: case MIX_OBJ: case PLAYER_OBJ:
        if (slot == 3) {  // GetInterface(self, iid, out)
            gaddr iid = c.u(1), out = c.u(2);
            gaddr r = g_obj[BQ_ITF];
            if (self == g_obj[ENGINE_OBJ] || self == g_obj[MIX_OBJ]) r = g_obj[ENGINE_ITF];
            else if (iid == g_iid_play) r = g_obj[PLAY_ITF];
            else if (iid == g_iid_cfg) r = g_obj[CFG_ITF];
            if (out) gref(out) = r;
        }
        break;
    case ENGINE_ITF:
        if (slot == 7) gref(c.u(1)) = g_obj[MIX_OBJ];   // CreateOutputMix
        if (slot == 2) {                                 // CreateAudioPlayer(self, out, src, sink, ...)
            gaddr src = c.u(2);
            if (src && gref(src + 4)) {
                gaddr fmt = gref(src + 4);
                if (gref(fmt) == 2) { g_chans = gref(fmt + 4); g_freq = gref(fmt + 8) / 1000; }  // SL_DATAFORMAT_PCM, milliHz
            }
            logf("opensl: CreateAudioPlayer %d Hz %d ch", g_freq, g_chans);
            gref(c.u(1)) = g_obj[PLAYER_OBJ];
        }
        break;
    case PLAY_ITF:
        if (slot == 0) {  // SetPlayState
            bool play = c.u(1) == 3;
            if (play) open_device();
            if (g_dev) SDL_PauseAudioDevice(g_dev, play ? 0 : 1);
            g_playing = play;
        }
        break;
    case BQ_ITF:
        if (slot == 0) { std::lock_guard<std::mutex> lk(g_qmx); g_queue.push_back({c.u(1), c.u(2)}); }
        if (slot == 1 || slot == 4) { std::lock_guard<std::mutex> lk(g_qmx); g_queue.clear(); }
        if (slot == 2 && c.u(1)) { std::lock_guard<std::mutex> lk(g_qmx); gref(c.u(1)) = g_queue.size(); gref(c.u(1) + 4) = 0; }
        if (slot == 3) { g_cb = c.u(1); g_cb_ctx = c.u(2); }
        break;
    }
    c.ret(0);  // SL_RESULT_SUCCESS
}

static void h_slCreateEngine(Ctx& c) { if (c.u(0)) gref(c.u(0)) = g_obj[ENGINE_OBJ]; c.ret(0); }

static gaddr sl_sym(const char* n) {
    if (!strcmp(n, "slCreateEngine")) return g_slCreateEngine;
    if (!strcmp(n, "SL_IID_ENGINE")) return g_iid_engine;
    if (!strcmp(n, "SL_IID_PLAY")) return g_iid_play;
    if (!strcmp(n, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE") || !strcmp(n, "SL_IID_BUFFERQUEUE")) return g_iid_bq;
    if (!strcmp(n, "SL_IID_ANDROIDCONFIGURATION")) return g_iid_cfg;
    if (!strcmp(n, "SL_IID_RECORD")) return g_iid_record;
    if (!strcmp(n, "SL_IID_OUTPUTMIX")) return g_iid_outmix;
    return 0;
}

static void h_dlopen(Ctx& c) { logf("dlopen(%s)", c.s(0) ? c.s(0) : "NULL"); c.ret(c.s(0) && strstr(c.s(0), "OpenSLES") ? 2 : 1); }
static void h_dlsym(Ctx& c) {
    gaddr a = c.u(0) == 2 ? sl_sym(c.s(1)) : 0;
    if (!a) a = any_sym(c.s(1));
    if (!a) a = hle_lookup(c.s(1));
    if (!a) logf("dlsym(%s) -> NULL", c.s(1));
    c.ret(a);
}

void hle_opensl_init() {
    static const int slots[NKIND] = {10, 10, 10, 16, 12, 6, 2};
    static std::vector<std::string> names;
    names.reserve(100);
    for (int k = 0; k < NKIND; k++) {
        gaddr vt = gmalloc(slots[k] * 4);
        for (int i = 0; i < slots[k]; i++) {
            names.push_back("SL" + std::to_string(k) + "_" + std::to_string(i));
            gref(vt + 4 * i) = hle_stub(names.back().c_str(), sl_dispatch);
        }
        g_obj[k] = gmalloc(4); gref(g_obj[k]) = vt;
    }
    // interface IDs: each variable holds its own address, so value or address both compare equal
    for (gaddr* id : {&g_iid_engine, &g_iid_play, &g_iid_bq, &g_iid_cfg, &g_iid_record, &g_iid_outmix}) { *id = gmalloc(4); gref(*id) = *id; }
    g_slCreateEngine = hle_stub("slCreateEngine", h_slCreateEngine);
    hle_register("dlopen", h_dlopen);
    hle_register("dlsym", h_dlsym);
}

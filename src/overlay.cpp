// Port overlay: a small settings menu drawn on top of the game (hold - / Back for 1 s).
// Draws with its own GL objects and restores every piece of GL state it touches.
#include "core.hpp"
#include "overlay.hpp"

#include <GLES2/gl2.h>
#include <ctime>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cctype>
#include <string>
#include <vector>
#include <algorithm>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"       // public domain; decodes the game's own font textures from the OBB
#include "font8x8_basic.h"   // public domain, github.com/dhepper/font8x8
extern std::string g_data_dir;

namespace {
struct Preset { const char* name; float particles, background; };
const Preset kPresets[] = {
    {"Android High", 0.45f, 0.325f},        // what the Android build uses on a high-end phone
    {"Console (PS4/XB1)", 1.0f, 1.0f},      // the PS4 / Xbox One values, closest to PC
    {"Max (beyond console)", 1.5f, 1.5f},   // stress test
};
const int kNumPresets = sizeof kPresets / sizeof kPresets[0];

// Achievements, in the game's E_Achievement order (= Google Play IDs ...EAIQAA..HQ, 0..29). Names and
// Android thresholds from the Google Play listing; the two co-op ones were hidden there.
struct Ach { const char* name; const char* desc; };
const Ach kAch[] = {
    {"First Step", "Earn 3 stars on any Adventure Mode level"},
    {"Geometry Warrior", "Earn 3 stars on all 50 Adventure Mode levels"},
    {"Keen Adventurer", "Defeat the Topaz boss in Adventure Mode"},
    {"Specialist", "Upgrade any one Drone to max level"},
    {"Maximise", "Upgrade all 5 Drones to max level"},
    {"Millionaire", "Score 1 million on any Adventure Mode level"},
    {"Multi Millionaire", "Score 100 million on any Adventure Mode level"},
    {"Geomtastic", "Reach a x1000 multiplier on any Adventure level"},
    {"Boom", "Kill 100 enemies with a single Smart Bomb"},
    {"Kill Them All", "Kill every enemy type at least once"},
    {"Dying Time", "Be killed by every enemy type at least once"},
    {"Powered Up", "Activate each Super State at least once"},
    {"Best Friends", "Defeat the Garnet boss in Co-op Mode"},
    {"BFF", "Earn 3 stars on all 10 Co-op Mode levels"},
    {"Gatherer", "Collect 500,000 Geoms in Adventure Mode"},
    {"Beat Craig", "Beat the dev's 9,451,570 on Peanut Dreams"},
    {"Beat Col", "Beat the dev's 2,154,750 on Gate Dash"},
    {"Beat Phil", "Beat the dev's 21,478,525 on Nufo Flow"},
    {"Rocket Destroyer", "Destroy 10,000 Rocket enemies in total"},
    {"Deadliest", "Complete Deadline Classic without losing a life"},
    {"Bossman", "Beat any boss without a Drone Super or Smart Bomb"},
    {"Retro", "Score 5 million on Evolved Classic before dying"},
    {"Sharp Shooter", "Score 500,000 on Twisted Blister without moving"},
    {"Survivalist", "Survive 30 s on Pacifism Classic without a gate"},
    {"Peacekeeper", "Survive 60 s on Deflector without shooting, pass"},
    {"Stationary", "Score 250,000 on Waves Classic without moving"},
    {"Bullet Proof", "Score 1 million on Maze without dying"},
    {"Beat Andy", "Beat the dev's 8,418,850 on Pacifism Classic"},
    {"King Killer", "Spend 30 s outside the King Zones in King Classic"},
    {"Cleaning Up", "Destroy the cleaner"},
};
const int kNumAch = sizeof kAch / sizeof kAch[0];
std::string g_ach_date[kNumAch];   // "" = locked
std::mutex g_mx;                   // unlocks arrive from game threads

GwSettings g_set;           // saved in gw3nx.ini
int g_cursor = 0;
enum Page { MAIN, ACH } g_page = MAIN;
int g_ach_cursor = 0;
bool g_open = false;
int g_back_held = 0;
uint32_t g_prev = 0;
uint32_t g_toast_until = 0, g_toast_start = 0;
std::string g_toast;                  // port messages (effects preset), drawn with the 8x8 font
std::string g_ach_head, g_ach_name, g_ach_desc;   // achievement pop-up, drawn in the game's style
uint32_t g_ach_start = 0, g_ach_until = 0;

std::string ini_path() { return g_data_dir + "/gw3nx.ini"; }
void save() {
    if (FILE* f = fopen(ini_path().c_str(), "w")) {
        fprintf(f, "particles=%.3f\nbackground=%.3f\nrumble=%.3f\nfps=%d\n", g_set.particles, g_set.background, g_set.rumble, (int)g_set.fps);
        fclose(f);
    }
}
int cur_preset() {   // -1 = custom values (set from the game's Options > Graphics)
    for (int i = 0; i < kNumPresets; i++)
        if (fabsf(kPresets[i].particles - g_set.particles) < 0.02f && fabsf(kPresets[i].background - g_set.background) < 0.02f) return i;
    return -1;
}
uint32_t g_fps_t0, g_fps_frames; int g_fps = 0;
std::string ach_path() { return g_data_dir + "/achievements.txt"; }
int ach_count() { int n = 0; for (auto& d : g_ach_date) n += !d.empty(); return n; }
const int kFpsItem = kNumPresets, kAchItem = kNumPresets + 1, kItems = kNumPresets + 2;   // main page rows

enum { B_A = 1 << 0, B_B = 1 << 1, B_UP = 1 << 8, B_DOWN = 1 << 9, B_BACK = 1 << 13 };

// ---- drawing
GLuint g_prog, g_tex;
GLint g_u_screen, g_u_color;
std::vector<float> g_verts;   // x, y, u, v
const int kSolid = 128;       // atlas slot that is fully lit (boxes)

void build() {
    const char* vs = "attribute vec2 p; attribute vec2 t; uniform vec2 s; varying vec2 v;"
                     "void main(){ v = t; gl_Position = vec4(p.x / s.x * 2.0 - 1.0, 1.0 - p.y / s.y * 2.0, 0.0, 1.0); }";
    const char* fs = "precision mediump float; varying vec2 v; uniform sampler2D f; uniform vec4 c;"
                     "void main(){ gl_FragColor = vec4(c.rgb, c.a * texture2D(f, v).a); }";
    auto sh = [](GLenum k, const char* src) { GLuint s = glCreateShader(k); glShaderSource(s, 1, &src, nullptr); glCompileShader(s); return s; };
    g_prog = glCreateProgram();
    glAttachShader(g_prog, sh(GL_VERTEX_SHADER, vs));
    glAttachShader(g_prog, sh(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(g_prog, 0, "p");
    glBindAttribLocation(g_prog, 1, "t");
    glLinkProgram(g_prog);
    g_u_screen = glGetUniformLocation(g_prog, "s");
    g_u_color = glGetUniformLocation(g_prog, "c");
    // 129 glyphs of 8x8 side by side, alpha only
    std::vector<uint8_t> px(129 * 8 * 8);
    for (int g = 0; g < 129; g++)
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                px[y * 129 * 8 + g * 8 + x] = g == kSolid || (font8x8_basic[g][y] >> x & 1) ? 255 : 0;
    glGenTextures(1, &g_tex);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, 129 * 8, 8, 0, GL_ALPHA, GL_UNSIGNED_BYTE, px.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void quad(float x, float y, float w, float h, int glyph) {
    // inset u by a hair so nearest sampling never bleeds into the next glyph
    float u0 = (glyph * 8 + 0.01f) / (129 * 8.f), u1 = (glyph * 8 + 7.99f) / (129 * 8.f);
    float v[] = {x, y, u0, 0, x + w, y, u1, 0, x + w, y + h, u1, 1, x, y, u0, 0, x + w, y + h, u1, 1, x, y + h, u0, 1};
    g_verts.insert(g_verts.end(), v, v + 24);
}
void flush(float r, float g, float b, float a) {
    if (g_verts.empty()) return;
    glUniform4f(g_u_color, r, g, b, a);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, g_verts.data());
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, g_verts.data() + 2);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(g_verts.size() / 4));
    g_verts.clear();
}
void text(float x, float y, float sz, const char* s) {
    for (; *s; s++, x += sz) if ((unsigned char)*s < 128 && *s != ' ') quad(x, y, sz, sz, *s);
}

// ---- the game's own font (BMFont text descriptor + PNG atlas, read from the OBB at runtime)
struct Glyph { float x, y, w, h, xo, yo, xa; bool ok; };
struct Font { GLuint tex = 0, glow = 0; float sw = 1, sh = 1, line = 0, base = 0; Glyph g[256] = {}; bool tried = false, ok = false; };
Font g_f30, g_f22;
GLuint upload_png(const std::vector<uint8_t>& png) {
    int w, h, n;
    unsigned char* px = stbi_load_from_memory(png.data(), (int)png.size(), &w, &h, &n, 4);
    if (!px) return 0;
    GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    stbi_image_free(px);
    return t;
}
bool load_font(Font& f, const char* name) {
    if (f.tried) return f.ok;
    f.tried = true;
    std::string base = std::string("Common/Textures/1080/Fonts/") + name;
    std::vector<uint8_t> fnt, png, glow;
    if (!asset_load((base + ".fnt").c_str(), fnt) || !asset_load((base + ".png").c_str(), png)) { logf("overlay: font %s not in the OBB", name); return false; }
    std::string txt(fnt.begin(), fnt.end());
    auto num = [](const std::string& l, const char* k) { size_t p = l.find(std::string(" ") + k + "="); return p == std::string::npos ? 0.f : (float)atof(l.c_str() + p + strlen(k) + 2); };
    size_t pos = 0;
    while (pos < txt.size()) {
        size_t e = txt.find('\n', pos); if (e == std::string::npos) e = txt.size();
        std::string l = " " + txt.substr(pos, e - pos); pos = e + 1;
        if (l.rfind(" common ", 0) == 0) { f.line = num(l, "lineHeight"); f.base = num(l, "base"); f.sw = num(l, "scaleW"); f.sh = num(l, "scaleH"); }
        else if (l.rfind(" char ", 0) == 0) {
            int id = (int)num(l, "id");
            if (id > 0 && id < 256) f.g[id] = {num(l, "x"), num(l, "y"), num(l, "width"), num(l, "height"), num(l, "xoffset"), num(l, "yoffset"), num(l, "xadvance"), true};
        }
    }
    f.tex = upload_png(png);
    if (asset_load((base + "_shadow.png").c_str(), glow)) f.glow = upload_png(glow);
    f.ok = f.tex && f.sw > 1;
    logf("overlay: game font %s %s", name, f.ok ? "loaded" : "failed");
    return f.ok;
}
float ftext_width(Font& f, const char* s, float k) { float w = 0; for (; *s; s++) if (f.g[(unsigned char)*s].ok) w += f.g[(unsigned char)*s].xa * k; return w; }
// draws s with its top-left at (x, y); k = pixels per font unit
void ftext(Font& f, float x, float y, float k, const char* s, float r, float g, float b, float a) {
    for (GLuint t : {f.glow, f.tex}) {
        if (!t) continue;
        float cx = x;
        for (const char* q = s; *q; q++) {
            const Glyph& gl = f.g[(unsigned char)*q];
            if (!gl.ok) continue;
            float x0 = cx + gl.xo * k, y0 = y + gl.yo * k, x1 = x0 + gl.w * k, y1 = y0 + gl.h * k;
            float u0 = gl.x / f.sw, v0 = gl.y / f.sh, u1 = (gl.x + gl.w) / f.sw, v1 = (gl.y + gl.h) / f.sh;
            float v[] = {x0, y0, u0, v0, x1, y0, u1, v0, x1, y1, u1, v1, x0, y0, u0, v0, x1, y1, u1, v1, x0, y1, u0, v1};
            g_verts.insert(g_verts.end(), v, v + 24);
            cx += gl.xa * k;
        }
        glBindTexture(GL_TEXTURE_2D, t);
        if (t == f.glow) flush(r * 0.5f, g * 0.8f, b, a * 0.55f); else flush(r, g, b, a);
    }
    glBindTexture(GL_TEXTURE_2D, g_tex);
}

// Achievement pop-up in the style of the game's own pop-ups: dark header strip, translucent blue body,
// thin light-cyan frame, Futura text with its glow. Fades in, holds, fades out.
void draw_achievement_popup(int w, int h, uint32_t now) {
    if (!load_font(g_f30, "futura30base10") || !load_font(g_f22, "futura22base7")) return;
    float k = h / 1080.f;
    float t = (now - g_ach_start) / 1000.f, left = (g_ach_until - now) / 1000.f;
    float a = std::min(1.0f, std::min(t / 0.25f, left / 0.5f));
    float slide = (1 - std::min(1.0f, t / 0.25f)) * 24 * k;
    float k30 = k * 1.15f, k22 = k * 1.05f;
    float tw = std::max({ftext_width(g_f22, g_ach_head.c_str(), k22), ftext_width(g_f30, g_ach_name.c_str(), k30), ftext_width(g_f22, g_ach_desc.c_str(), k22)});
    float pw = std::max(640 * k, tw + 96 * k), hh = 46 * k, bh = 124 * k;
    float x0 = (w - pw) / 2, y0 = h - 300 * k + slide;
    quad(x0, y0, pw, hh, kSolid); flush(0.03f, 0.07f, 0.17f, 0.92f * a);           // header strip
    quad(x0, y0 + hh, pw, bh, kSolid); flush(0.28f, 0.60f, 0.92f, 0.42f * a);      // body
    float b = 2 * k;                                                                  // frame
    quad(x0, y0, pw, b, kSolid); quad(x0, y0 + hh + bh - b, pw, b, kSolid);
    quad(x0, y0, b, hh + bh, kSolid); quad(x0 + pw - b, y0, b, hh + bh, kSolid);
    quad(x0, y0 + hh - b / 2, pw, b / 2, kSolid); flush(0.55f, 0.86f, 1.0f, 0.9f * a);
    auto centered = [&](Font& f, float kk, const std::string& str, float y, float r, float g, float bl, float al) {
        ftext(f, (w - ftext_width(f, str.c_str(), kk)) / 2, y, kk, str.c_str(), r, g, bl, al);
    };
    centered(g_f22, k22, g_ach_head, y0 + (hh - g_f22.line * k22) / 2, 0.55f, 0.86f, 1.0f, a);
    centered(g_f30, k30, g_ach_name, y0 + hh + 18 * k, 1, 1, 1, a);
    centered(g_f22, k22, g_ach_desc, y0 + hh + 18 * k + g_f30.line * k30 + 8 * k, 0.88f, 0.93f, 1.0f, a);
}

// Everything the overlay changes, saved before and put back after.
struct GLSave {
    GLint prog, active, tex, abuf, fbo, vp[4], bsrc_rgb, bdst_rgb, bsrc_a, bdst_a, beq_rgb, beq_a;
    GLboolean blend, depth, cull, scissor, stencil, cmask[4];
    struct Attr { GLint on, size, type, norm, stride, buf; void* ptr; } attr[2];
    void save() {
        glGetIntegerv(GL_CURRENT_PROGRAM, &prog); glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        glActiveTexture(GL_TEXTURE0); glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &abuf); glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo); glGetIntegerv(GL_VIEWPORT, vp);
        glGetIntegerv(GL_BLEND_SRC_RGB, &bsrc_rgb); glGetIntegerv(GL_BLEND_DST_RGB, &bdst_rgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &bsrc_a); glGetIntegerv(GL_BLEND_DST_ALPHA, &bdst_a);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &beq_rgb); glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &beq_a);
        blend = glIsEnabled(GL_BLEND); depth = glIsEnabled(GL_DEPTH_TEST); cull = glIsEnabled(GL_CULL_FACE);
        scissor = glIsEnabled(GL_SCISSOR_TEST); stencil = glIsEnabled(GL_STENCIL_TEST); glGetBooleanv(GL_COLOR_WRITEMASK, cmask);
        for (int i = 0; i < 2; i++) {
            Attr& a = attr[i];
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a.on); glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a.size);
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a.type); glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a.norm);
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a.stride); glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a.buf);
            glGetVertexAttribPointerv(i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a.ptr);
        }
    }
    void restore() {
        for (int i = 0; i < 2; i++) {
            Attr& a = attr[i];
            glBindBuffer(GL_ARRAY_BUFFER, a.buf);
            glVertexAttribPointer(i, a.size, a.type, (GLboolean)a.norm, a.stride, a.ptr);
            if (a.on) glEnableVertexAttribArray(i); else glDisableVertexAttribArray(i);
        }
        glBindBuffer(GL_ARRAY_BUFFER, abuf);
        glUseProgram(prog); glBindTexture(GL_TEXTURE_2D, tex); glActiveTexture(active);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo); glViewport(vp[0], vp[1], vp[2], vp[3]);
        glBlendFuncSeparate(bsrc_rgb, bdst_rgb, bsrc_a, bdst_a); glBlendEquationSeparate(beq_rgb, beq_a);
        auto set = [](GLenum c, GLboolean on) { if (on) glEnable(c); else glDisable(c); };
        set(GL_BLEND, blend); set(GL_DEPTH_TEST, depth); set(GL_CULL_FACE, cull); set(GL_SCISSOR_TEST, scissor); set(GL_STENCIL_TEST, stencil);
        glColorMask(cmask[0], cmask[1], cmask[2], cmask[3]);
    }
};
}  // namespace

void overlay_init() {
    if (FILE* f = fopen(ini_path().c_str(), "r")) {
        char k[32]; float v;
        while (fscanf(f, " %31[^=]=%f", k, &v) == 2) {
            std::string key = k;
            if (key == "particles") g_set.particles = v;
            else if (key == "background") g_set.background = v;
            else if (key == "rumble") g_set.rumble = v;
            else if (key == "fps") g_set.fps = v != 0;
            else if (key == "effects" && v >= 0 && v < kNumPresets) { g_set.particles = kPresets[(int)v].particles; g_set.background = kPresets[(int)v].background; }
        }
        fclose(f);
    }
    g_cursor = std::max(cur_preset(), 0);
    logf("settings: particles %.2f, background %.2f, rumble %.2f, fps counter %s", g_set.particles, g_set.background, g_set.rumble, g_set.fps ? "on" : "off");
    if (FILE* f = fopen(ach_path().c_str(), "r")) {   // lines: "<index> <date> <name>"
        int i; char date[32];
        while (fscanf(f, "%d %31s%*[^\n]", &i, date) == 2) if (i >= 0 && i < kNumAch) g_ach_date[i] = date;
        fclose(f);
    }
    logf("overlay: %d/%d achievements", ach_count(), kNumAch);
}

void overlay_achievement(int i, uint32_t now_ms) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (i < 0 || i >= kNumAch) { logf("achievement %d: not in the list", i); return; }
    if (!g_ach_date[i].empty()) return;
    char date[32]; time_t t = time(nullptr); strftime(date, sizeof date, "%Y-%m-%d", localtime(&t));
    g_ach_date[i] = date;
    if (FILE* f = fopen(ach_path().c_str(), "a")) { fprintf(f, "%d %s %s\n", i, date, kAch[i].name); fclose(f); }
    g_ach_head = "ACHIEVEMENT UNLOCKED";
    g_ach_name = kAch[i].name;
    for (char& ch : g_ach_name) ch = (char)toupper((unsigned char)ch);
    g_ach_desc = kAch[i].desc;
    g_ach_start = now_ms; g_ach_until = now_ms + 5000;
    logf("achievement %d unlocked: %s", i, kAch[i].name);
}

std::string overlay_achievements_xml() {
    std::lock_guard<std::mutex> lk(g_mx);
    auto esc = [](const char* s) {
        std::string r;
        for (; *s; s++) r += *s == '&' ? "&amp;" : *s == '"' ? "&quot;" : *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : std::string(1, *s);
        return r;
    };
    char head[64]; snprintf(head, sizeof head, "%d of %d unlocked", ach_count(), kNumAch);
    std::string x = "<credits>\n\t<Company Name = \"" + std::string(head) + "\" >\n";
    for (int pass = 0; pass < 2; pass++) {
        bool any = false;
        for (int i = 0; i < kNumAch; i++) any |= g_ach_date[i].empty() == (pass == 1);
        if (!any) continue;
        x += std::string("\t\t<Group Name = \"") + (pass == 0 ? "UNLOCKED" : "LOCKED") + "\">\n";
        for (int i = 0; i < kNumAch; i++) {
            if (g_ach_date[i].empty() != (pass == 1)) continue;
            // the credits page prints JobTitle first, then Name: name on top, description (and date) under it
            std::string name = kAch[i].name, desc = kAch[i].desc;
            for (char& ch : name) ch = (char)toupper((unsigned char)ch);
            if (pass == 0) desc += " - " + g_ach_date[i];
            x += "\t\t\t<Person JobTitle = \"" + esc(name.c_str()) + "\" Name = \"" + esc(desc.c_str()) + "\" />\n";
        }
        x += "\t\t</Group>\n";
    }
    x += "\t</Company>\n</credits>\n";
    return x;
}

void overlay_show_achievements() { std::lock_guard<std::mutex> lk(g_mx); g_open = true; g_page = ACH; g_back_held = -1000; }

void overlay_detail(float& particles, float& background) { particles = g_set.particles; background = g_set.background; }
GwSettings& gw_settings() { return g_set; }
int gw_preset_count() { return kNumPresets; }
const char* gw_preset_name(int i) { return kPresets[i].name; }
void gw_preset_values(int i, float& particles, float& background) { particles = kPresets[i].particles; background = kPresets[i].background; }
int gw_preset_match(float particles, float background) {
    for (int i = 0; i < kNumPresets; i++)
        if (fabsf(kPresets[i].particles - particles) < 0.02f && fabsf(kPresets[i].background - background) < 0.02f) return i;
    return -1;
}
void gw_settings_save() { save(); }

bool overlay_input(uint32_t* masks, int n, uint32_t now_ms) {
    uint32_t m = 0;
    for (int i = 0; i < n; i++) m |= masks[i];
    uint32_t pressed = m & ~g_prev;
    g_prev = m;
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_open) {
        g_back_held = (m & B_BACK) ? g_back_held + 1 : 0;
        if (g_back_held == 60) { g_open = true; g_page = MAIN; g_cursor = std::max(cur_preset(), 0); logf("overlay: opened"); }
        return false;
    }
    if (g_page == ACH) {
        if (pressed & B_UP) g_ach_cursor = (g_ach_cursor + kNumAch - 1) % kNumAch;
        if (pressed & B_DOWN) g_ach_cursor = (g_ach_cursor + 1) % kNumAch;
        if (pressed & B_B) g_page = MAIN;
    } else {
        if (pressed & B_UP) g_cursor = (g_cursor + kItems - 1) % kItems;
        if (pressed & B_DOWN) g_cursor = (g_cursor + 1) % kItems;
        if ((pressed & B_A) && g_cursor == kAchItem) g_page = ACH;
        else if ((pressed & B_A) && g_cursor == kFpsItem) { g_set.fps = !g_set.fps; save(); }
        else if (pressed & B_A) {
            g_set.particles = kPresets[g_cursor].particles; g_set.background = kPresets[g_cursor].background; save(); g_open = false;
            g_toast = std::string("Effects: ") + kPresets[g_cursor].name + " - applies from the next level";
            g_toast_until = now_ms + 3000;
            logf("overlay: effects -> %s", kPresets[g_cursor].name);
        }
        else if (pressed & B_B) g_open = false;
    }
    if (!g_open) g_back_held = -1000;   // don't reopen until - is released and held again
    for (int i = 0; i < n; i++) masks[i] = 0;   // the game sees no input while the menu is up
    return true;
}

void overlay_draw(int w, int h, uint32_t now_ms) {
    std::lock_guard<std::mutex> lk(g_mx);
    g_fps_frames++;
    if (now_ms - g_fps_t0 >= 1000) { g_fps = (int)(g_fps_frames * 1000 / std::max<uint32_t>(now_ms - g_fps_t0, 1)); g_fps_frames = 0; g_fps_t0 = now_ms; }
    bool toast = now_ms < g_toast_until, popup = now_ms < g_ach_until;
    // the first frame goes through once anyway, to load the game font up front (no hitch at the first unlock)
    if (!g_open && !toast && !popup && !g_set.fps && g_f22.tried) return;
    if (!g_prog) build();
    GLSave s; s.save();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glUseProgram(g_prog);
    glUniform2f(g_u_screen, (float)w, (float)h);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, g_tex);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
    glEnable(GL_BLEND); glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    float sz = h / 720.f * 24;   // character size: 24 px at 720p
    if (g_open && g_page == ACH) {
        const int cols = 44, rows = 16, vis = 10;
        float bw = cols * sz, bh = rows * sz, x0 = (w - bw) / 2, y0 = (h - bh) / 2;
        quad(x0, y0, bw, bh, kSolid); flush(0.02f, 0.05f, 0.12f, 0.9f);
        quad(x0, y0, bw, sz / 6, kSolid); quad(x0, y0 + bh - sz / 6, bw, sz / 6, kSolid);
        quad(x0, y0, sz / 6, bh, kSolid); quad(x0 + bw - sz / 6, y0, sz / 6, bh, kSolid); flush(0.25f, 0.75f, 1.0f, 1.0f);
        char title[64]; snprintf(title, sizeof title, "Achievements  %d/%d", ach_count(), kNumAch);
        text(x0 + sz, y0 + sz * 0.8f, sz, title); flush(0.4f, 0.85f, 1.0f, 1.0f);
        int first = std::min(std::max(g_ach_cursor - vis / 2, 0), kNumAch - vis);
        for (int i = first; i < first + vis; i++) {
            float y = y0 + sz * (2.4f + (i - first) * 1.05f);
            bool got = !g_ach_date[i].empty();
            if (i == g_ach_cursor) { quad(x0 + sz * 0.5f, y - sz * 0.15f, bw - sz, sz * 1.1f, kSolid); flush(0.2f, 0.5f, 0.9f, 0.45f); }
            text(x0 + sz, y, sz * 0.8f, got ? "*" : "-"); text(x0 + sz * 2.2f, y, sz * 0.8f, kAch[i].name);
            if (got) text(x0 + bw - sz * 9.5f, y, sz * 0.8f, g_ach_date[i].c_str());
            flush(got ? 1.0f : 0.55f, got ? 0.9f : 0.6f, got ? 0.45f : 0.7f, 1);
        }
        text(x0 + sz, y0 + bh - sz * 2.6f, sz * 0.7f, kAch[g_ach_cursor].desc); flush(0.85f, 0.9f, 1.0f, 1);
        text(x0 + sz, y0 + bh - sz * 1.5f, sz * 0.7f, "Up/Down: scroll   B: back"); flush(0.7f, 0.75f, 0.85f, 1);
    } else if (g_open) {
        const int cols = 30, rows = 13;
        float bw = cols * sz, bh = rows * sz, x0 = (w - bw) / 2, y0 = (h - bh) / 2;
        quad(x0, y0, bw, bh, kSolid); flush(0.02f, 0.05f, 0.12f, 0.88f);
        quad(x0, y0, bw, sz / 6, kSolid); quad(x0, y0 + bh - sz / 6, bw, sz / 6, kSolid);
        quad(x0, y0, sz / 6, bh, kSolid); quad(x0 + bw - sz / 6, y0, sz / 6, bh, kSolid); flush(0.25f, 0.75f, 1.0f, 1.0f);
        text(x0 + sz, y0 + sz * 0.8f, sz, "gw3nx menu - Effects detail"); flush(0.4f, 0.85f, 1.0f, 1.0f);
        for (int i = 0; i < kNumPresets; i++) {
            float y = y0 + sz * (2.6f + i * 1.4f);
            if (i == g_cursor) { quad(x0 + sz * 0.5f, y - sz * 0.25f, bw - sz, sz * 1.5f, kSolid); flush(0.2f, 0.5f, 0.9f, 0.45f); text(x0 + sz, y, sz, ">"); }
            text(x0 + sz * 2.5f, y, sz, kPresets[i].name);
            if (i == cur_preset()) text(x0 + bw - sz * 3, y, sz, "*");
            flush(1, 1, 1, 1);
        }
        {
            float y = y0 + sz * (2.6f + kNumPresets * 1.4f + 0.3f);
            if (g_cursor == kFpsItem) { quad(x0 + sz * 0.5f, y - sz * 0.25f, bw - sz, sz * 1.5f, kSolid); flush(0.2f, 0.5f, 0.9f, 0.45f); text(x0 + sz, y, sz, ">"); }
            text(x0 + sz * 2.5f, y, sz, g_set.fps ? "FPS counter    On" : "FPS counter    Off"); flush(1, 1, 1, 1);
        }
        {
            float y = y0 + sz * (2.6f + (kNumPresets + 1) * 1.4f + 0.3f);
            if (g_cursor == kAchItem) { quad(x0 + sz * 0.5f, y - sz * 0.25f, bw - sz, sz * 1.5f, kSolid); flush(0.2f, 0.5f, 0.9f, 0.45f); text(x0 + sz, y, sz, ">"); }
            char t[48]; snprintf(t, sizeof t, "Achievements %d/%d", ach_count(), kNumAch);
            text(x0 + sz * 2.5f, y, sz, t); flush(1.0f, 0.9f, 0.45f, 1);
        }
        text(x0 + sz, y0 + bh - sz * 2.6f, sz * 0.75f, "Effects apply from the next level"); flush(0.7f, 0.75f, 0.85f, 1);
        text(x0 + sz, y0 + bh - sz * 1.5f, sz * 0.75f, "A: choose   B: close"); flush(0.7f, 0.75f, 0.85f, 1);
    } else if (toast) {
        float tsz = sz * 0.75f, tw = (g_toast.size() + 2) * tsz;
        quad((w - tw) / 2, h - tsz * 4, tw, tsz * 2, kSolid); flush(0.02f, 0.05f, 0.12f, 0.85f);
        text((w - tw) / 2 + tsz, h - tsz * 3.5f, tsz, g_toast.c_str()); flush(1, 1, 1, 1);
    }
    if (!g_f22.tried) { load_font(g_f30, "futura30base10"); load_font(g_f22, "futura22base7"); }
    if (popup) draw_achievement_popup(w, h, now_ms);
    if (g_set.fps) {
        char t[16]; snprintf(t, sizeof t, "%d FPS", g_fps);
        float fsz = sz * 0.6f;
        quad(fsz * 0.5f, fsz * 0.5f, (strlen(t) + 1) * fsz, fsz * 1.5f, kSolid); flush(0, 0, 0, 0.6f);
        text(fsz, fsz * 0.75f, fsz, t); flush(g_fps >= 55 ? 0.5f : 1.0f, g_fps >= 55 ? 1.0f : 0.7f, 0.5f, 1);
    }
    s.restore();
}

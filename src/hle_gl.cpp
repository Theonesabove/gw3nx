// GLES2 bridge: guest GL calls go straight to the host GLES2 driver (pointers translated).
#include "core.hpp"

#include <GLES2/gl2.h>
#include <unordered_map>
#include <string>
#include <vector>
#include <utility>

// Pointer args are buffer offsets while a buffer is bound. Ask GL for the binding instead of tracking
// glBindBuffer: deleting a bound buffer silently unbinds it, and a stale guess hangs the GPU.
static GLint bound(GLenum which) { GLint b = 0; glGetIntegerv(which, &b); return b; }

// The engine only sets a viewport when it binds an off-screen target, never for the screen itself,
// so the screen kept the last (smaller) target's viewport: restore it whenever FBO 0 is bound.
int g_drawable_w = 1280, g_drawable_h = 720;
extern int g_screen_w, g_screen_h;   // the size the game renders at (fixed at startup)
static bool g_on_screen = true;
// After a dock/undock the real screen differs from the game's size: scale screen-space rects to it.
static void to_screen(GLint& x, GLint& y, GLsizei& w, GLsizei& h) {
    if (!g_on_screen || (g_drawable_w == g_screen_w && g_drawable_h == g_screen_h)) return;
    x = x * g_drawable_w / g_screen_w; w = w * g_drawable_w / g_screen_w;
    y = y * g_drawable_h / g_screen_h; h = h * g_drawable_h / g_screen_h;
}
static void h_glBindFramebuffer(Ctx& c) {
    glBindFramebuffer(c.u(0), c.u(1));
    g_on_screen = c.u(1) == 0;
    if (g_on_screen) glViewport(0, 0, g_drawable_w, g_drawable_h);
}
static void h_glViewport(Ctx& c) { GLint x = c.i(0), y = c.i(1); GLsizei w = c.i(2), h = c.i(3); to_screen(x, y, w, h); glViewport(x, y, w, h); }
static void h_glScissor(Ctx& c) { GLint x = c.i(0), y = c.i(1); GLsizei w = c.i(2), h = c.i(3); to_screen(x, y, w, h); glScissor(x, y, w, h); }

static void h_glVertexAttribPointer(Ctx& c) {
    uint32_t p = c.word(5);
    glVertexAttribPointer(c.u(0), c.i(1), c.u(2), (GLboolean)c.u(3), c.i(4), bound(GL_ARRAY_BUFFER_BINDING) ? (void*)(uintptr_t)p : g2h(p));
}
static void h_glDrawElements(Ctx& c) {
    uint32_t p = c.u(3);
    glDrawElements(c.u(0), c.i(1), c.u(2), bound(GL_ELEMENT_ARRAY_BUFFER_BINDING) ? (void*)(uintptr_t)p : g2h(p));
}
static void h_glCompileShader(Ctx& c) { uint64_t t = now_us(); glCompileShader(c.u(0)); g_t_shader_us += now_us() - t; }
static void h_glLinkProgram(Ctx& c) { uint64_t t = now_us(); glLinkProgram(c.u(0)); g_t_shader_us += now_us() - t; }
static void h_glShaderSource(Ctx& c) {
    GLsizei n = c.i(1);
    std::vector<const GLchar*> src(n);
    for (GLsizei i = 0; i < n; i++) src[i] = gstr(gref(c.u(2) + 4 * i));
    glShaderSource(c.u(0), n, src.data(), (const GLint*)c.p(3));
}
static void h_glGetString(Ctx& c) {
    static std::unordered_map<uint32_t, gaddr> cache;
    auto it = cache.find(c.u(0));
    if (it != cache.end()) { c.ret(it->second); return; }
    const char* s = (const char*)glGetString(c.u(0));
    std::string str = s ? s : "";
    if (c.u(0) == GL_EXTENSIONS) {
        // Only advertise extensions that need no extra entry points (mapped buffers would point
        // outside the 32-bit guest address space).
        static const char* keep[] = {"GL_OES_depth24", "GL_OES_packed_depth_stencil", "GL_OES_rgb8_rgba8", "GL_OES_texture_npot",
            "GL_OES_element_index_uint", "GL_OES_standard_derivatives", "GL_OES_compressed_ETC1_RGB8_texture", "GL_OES_depth_texture",
            "GL_EXT_texture_format_BGRA8888", "GL_OES_texture_half_float", "GL_OES_texture_float", "GL_EXT_blend_minmax"};
        std::string out;
        for (const char* k : keep) if ((" " + str + " ").find(std::string(" ") + k + " ") != std::string::npos) out += std::string(k) + " ";
        str = out;
    }
    gaddr g = s ? gstrdup(str.c_str()) : 0;
    logf("glGetString(0x%x) = %s", c.u(0), str.c_str());
    cache[c.u(0)] = g; c.ret(g);
}
static void h_eglGetProcAddress(Ctx& c) {
    gaddr a = hle_lookup(c.s(0));
    if (!a) logf("eglGetProcAddress(%s) -> NULL", c.s(0));
    c.ret(a);
}

void hle_gl_init() {
    struct { const char* n; HleFn f; } t[] = {
#include "gl_table.inc"
        {"glBindFramebuffer", h_glBindFramebuffer}, {"glViewport", h_glViewport}, {"glScissor", h_glScissor}, {"glVertexAttribPointer", h_glVertexAttribPointer},
        {"glDrawElements", h_glDrawElements}, {"glShaderSource", h_glShaderSource}, {"glGetString", h_glGetString},
        {"eglGetProcAddress", h_eglGetProcAddress}, {"glCompileShader", h_glCompileShader}, {"glLinkProgram", h_glLinkProgram},
    };
    for (auto& e : t) hle_register(e.n, e.f);
}

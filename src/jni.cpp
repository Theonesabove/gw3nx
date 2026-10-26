// Fake Java VM / JNIEnv. Every table slot is an HLE stub named "JNI_<function>", all routed to one
// dispatcher. Java methods the game calls are answered by name in java_method().
#include "core.hpp"

#include <functional>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <unistd.h>

extern std::string g_data_dir;
extern int g_screen_w, g_screen_h;

static const char* kJniNames[] = {
#include "jni_names.inc"
};
static gaddr g_env, g_vm;
bool g_tv_mode = false;
gaddr jni_env() { return g_env; }
gaddr jni_vm() { return g_vm; }

// ---- object model: every Java object is a unique 4-byte guest allocation
struct JArray { char type; uint32_t len; gaddr data; };
static std::mutex g_jmx;
static std::unordered_map<gaddr, std::string> g_strings, g_classes;
static std::unordered_map<gaddr, JArray> g_arrays;
static std::unordered_map<gaddr, std::string> g_objtag;   // debug: what kind of fake object
struct JMethod { std::string name, sig; bool is_static; };
static std::vector<JMethod> g_methods;   // id = index + 1

gaddr jni_obj(const char* tag) { gaddr o = gmalloc(4); std::lock_guard<std::mutex> lk(g_jmx); g_objtag[o] = tag; return o; }
gaddr jni_new_string(const char* s) { gaddr o = gmalloc(4); std::lock_guard<std::mutex> lk(g_jmx); g_strings[o] = s; return o; }
static std::string jstr(gaddr o) { std::lock_guard<std::mutex> lk(g_jmx); auto it = g_strings.find(o); return it == g_strings.end() ? "" : it->second; }
static gaddr new_array(char type, uint32_t len) {
    uint32_t esz = type == 'J' || type == 'D' ? 8 : type == 'S' || type == 'C' ? 2 : type == 'B' || type == 'Z' ? 1 : 4;
    gaddr o = gmalloc(4), d = gmalloc(len * esz + 8);
    memset(g2h(d), 0, len * esz);
    std::lock_guard<std::mutex> lk(g_jmx);
    g_arrays[o] = {type, len, d};
    return o;
}

// ---- arguments of a Java call, decoded from the method signature
struct JArgs {
    std::vector<uint64_t> v;
    uint32_t i(size_t k) const { return k < v.size() ? (uint32_t)v[k] : 0; }
    float f(size_t k) const { uint32_t w = i(k); float x; memcpy(&x, &w, 4); return x; }
};
static std::vector<char> sig_args(const std::string& sig) {
    std::vector<char> out;
    for (size_t k = 1; k < sig.size() && sig[k] != ')'; k++) {
        char t = sig[k];
        while (sig[k] == '[') { k++; t = 'L'; }
        if (sig[k] == 'L') { k = sig.find(';', k); t = 'L'; }
        out.push_back(t);
    }
    return out;
}
static char sig_ret(const std::string& sig) { size_t p = sig.find(')'); return p == std::string::npos ? 'V' : sig[p + 1] == '[' ? 'L' : sig[p + 1]; }

// Work the Java side does "later" (callbacks into the game); run by the main loop between frames.
static std::mutex g_post_mx;
static std::vector<std::function<void()>> g_posted;
void jni_run_posted() {
    std::vector<std::function<void()>> todo;
    { std::lock_guard<std::mutex> lk(g_post_mx); todo.swap(g_posted); }
    for (auto& f : todo) f();
}
static void post(std::function<void()> f) { std::lock_guard<std::mutex> lk(g_post_mx); g_posted.push_back(std::move(f)); }
static std::string save_path() { return g_data_dir + "/cloudsave.bin"; }

// ---- the Java side of the game: answers by method name
static uint64_t java_method(const JMethod& m, gaddr self, const JArgs& a) {
    const std::string& n = m.name;
    // Cloud saves (1.3.0 console UI): keep them in a file next to the game.
    if (n == "CloudSaveRead") {
        post([] {
            gaddr cb = any_sym("Java_com_activision_gw3_common_GW3JNILib_onSaveGameReadComplete");
            if (!cb) return;
            gaddr arr = 0;
            if (FILE* f = fopen(save_path().c_str(), "rb")) {
                fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
                arr = new_array('B', len);
                { std::lock_guard<std::mutex> lk(g_jmx); if (fread(g2h(g_arrays[arr].data), 1, len, f) != (size_t)len) len = 0; }
                fclose(f);
                logf("java: CloudSaveRead -> %ld bytes", len);
            } else logf("java: CloudSaveRead -> no save yet");
            call_guest(cb, {jni_env(), jni_obj("GW3JNILib"), arr});
        });
        return 0;
    }
    if (n.rfind("CloudSave", 0) == 0 && n != "CloudSaveRead") {
        for (size_t k = 0; k < a.v.size(); k++) {
            std::lock_guard<std::mutex> lk(g_jmx);
            auto it = g_arrays.find(a.i(k));
            if (it == g_arrays.end()) continue;
            if (FILE* f = fopen(save_path().c_str(), "wb")) { fwrite(g2h(it->second.data), 1, it->second.len, f); fclose(f); }
            logf("java: %s -> wrote %u bytes", n.c_str(), it->second.len);
            return 1;
        }
    }
    auto S = [](const char* s) -> uint64_t { return jni_new_string(s); };
    if (n == "getGameFilesDir" || n == "getPrivateFilesDir" || n == "getFilesDir" || n == "getObbDir") return S((g_data_dir + "/").c_str());
    if (n == "getExpansionFilePath" || n == "getObbPath" || n == "getMainObbPath")
        { extern std::string obb_path(); return S(obb_path().c_str()); }
    if (n == "getDeviceName" || n == "getDeviceModel") return S("Nintendo Switch");
    if (n == "getLanguage" || n == "getLocale") return S("en");
    if (n == "getCountry") return S("US");
    if (n == "getScreenWidth" || n == "getWidth") return g_screen_w;
    if (n == "getScreenHeight" || n == "getHeight") return g_screen_h;
    if (n == "isTVDevice" || n == "isTv" || n == "getTvDevice") return 1;
    if (n == "hasJoystick" || n == "isControllerConnected") return 1;
    if (n == "AppShutdown") {   // the game asks Java to close the app (Quit from the menu)
        logf("java: AppShutdown -> exiting");
        fflush(stderr);
        _exit(0);
    }
    static std::unordered_set<std::string> seen;
    static std::mutex mx;
    { std::lock_guard<std::mutex> lk(mx); if (seen.insert(n + m.sig).second) logf("java: unhandled %s%s %s (args %zu)", m.is_static ? "static " : "", n.c_str(), m.sig.c_str(), a.v.size()); }
    char r = sig_ret(m.sig);
    if (r == 'L' && m.sig.find(")Ljava/lang/String;") != std::string::npos) return S("");
    return 0;
}
static uint64_t java_field(const std::string& name, const std::string& sig) {
    if (name == "tvDevice") return g_tv_mode;   // Android TV build of the UI
    static std::unordered_set<std::string> seen;
    if (seen.insert(name).second) logf("java: field %s %s -> 0", name.c_str(), sig.c_str());
    return 0;
}
static std::vector<std::pair<std::string, std::string>> g_fields;  // id = index + 1

// ---- dispatcher
// Reads Java call arguments: from registers/stack ("..."), a va_list (V) or a jvalue array (A).
static JArgs read_args(Ctx& c, unsigned first_slot, const std::string& sig, char mode) {
    JArgs a;
    auto types = sig_args(sig);
    if (mode == 'A') {
        gaddr arr = c.u(first_slot);
        for (size_t k = 0; k < types.size(); k++) {
            uint64_t raw = gref<uint64_t>(arr + 8 * k);
            if (types[k] != 'J' && types[k] != 'D') raw &= 0xFFFFFFFF;
            a.v.push_back(raw);
        }
        return a;
    }
    gaddr va = mode == 'V' ? c.u(first_slot) : 0;
    c.slot = first_slot;
    auto n32 = [&]() -> uint32_t { if (mode == 'V') { uint32_t w = gref(va); va += 4; return w; } return c.next(); };
    auto n64 = [&]() -> uint64_t { if (mode == 'V') { va = (va + 7) & ~7u; uint64_t w = gref<uint64_t>(va); va += 8; return w; } return c.next64(); };
    for (char t : types) {
        if (t == 'J' || t == 'D') a.v.push_back(n64());
        else if (t == 'F') {  // float promoted to double through "..."
            uint64_t w = n64(); double d; memcpy(&d, &w, 8); float f = (float)d; uint32_t fw; memcpy(&fw, &f, 4); a.v.push_back(fw);
        } else a.v.push_back(n32());
    }
    return a;
}

static void ret_typed(Ctx& c, char t, uint64_t v) {
    if (t == 'J' || t == 'D') c.ret64(v); else c.ret((uint32_t)v);
}

static void jni_dispatch(Ctx& c) {
    std::string fn = g_cur_hle + 4;  // strip "JNI_"
    // Call[Static|Nonvirtual]<Type>Method[V|A]
    if (fn.rfind("Call", 0) == 0) {
        size_t mpos = fn.find("Method");
        char mode = fn.size() > mpos + 6 ? fn[mpos + 6] : 0;
        bool is_static = fn.find("Static") != std::string::npos, nonvirt = fn.find("Nonvirtual") != std::string::npos;
        unsigned mid_slot = nonvirt ? 3 : 2;
        uint32_t mid = c.u(mid_slot);
        if (!mid || mid > g_methods.size()) { logf("jni: %s with bad method id %u", fn.c_str(), mid); c.ret64(0); return; }
        const JMethod& m = g_methods[mid - 1];
        JArgs a = read_args(c, mid_slot + 1, m.sig, mode);
        uint64_t r = java_method(m, is_static ? 0 : c.u(1), a);
        ret_typed(c, sig_ret(m.sig), r);
        return;
    }
    static std::unordered_map<std::string, std::function<void(Ctx&)>> h = {
        {"GetVersion", [](Ctx& c) { c.ret(0x00010006); }},
        {"FindClass", [](Ctx& c) { gaddr o = gmalloc(4); std::lock_guard<std::mutex> lk(g_jmx); g_classes[o] = c.s(1); c.ret(o); }},
        {"GetObjectClass", [](Ctx& c) { c.ret(jni_obj("class")); }},
        {"GetSuperclass", [](Ctx& c) { c.ret(0); }},
        {"IsInstanceOf", [](Ctx& c) { c.ret(1); }},
        {"IsSameObject", [](Ctx& c) { c.ret(c.u(1) == c.u(2)); }},
        {"ExceptionOccurred", [](Ctx& c) { c.ret(0); }},
        {"ExceptionCheck", [](Ctx& c) { c.ret(0); }},
        {"NewGlobalRef", [](Ctx& c) { c.ret(c.u(1)); }}, {"NewLocalRef", [](Ctx& c) { c.ret(c.u(1)); }},
        {"NewWeakGlobalRef", [](Ctx& c) { c.ret(c.u(1)); }},
        {"PushLocalFrame", [](Ctx& c) { c.ret(0); }}, {"PopLocalFrame", [](Ctx& c) { c.ret(c.u(1)); }},
        {"EnsureLocalCapacity", [](Ctx& c) { c.ret(0); }},
        {"GetObjectRefType", [](Ctx& c) { c.ret(2); }},
        {"AllocObject", [](Ctx& c) { c.ret(jni_obj("alloc")); }},
        {"NewObject", [](Ctx& c) { c.ret(jni_obj("new")); }}, {"NewObjectV", [](Ctx& c) { c.ret(jni_obj("new")); }},
        {"NewObjectA", [](Ctx& c) { c.ret(jni_obj("new")); }},
        {"GetMethodID", [](Ctx& c) { g_methods.push_back({c.s(2), c.s(3), false}); c.ret(g_methods.size()); }},
        {"GetStaticMethodID", [](Ctx& c) { g_methods.push_back({c.s(2), c.s(3), true}); c.ret(g_methods.size()); }},
        {"GetFieldID", [](Ctx& c) { g_fields.push_back({c.s(2), c.s(3)}); c.ret(g_fields.size()); }},
        {"GetStaticFieldID", [](Ctx& c) { g_fields.push_back({c.s(2), c.s(3)}); c.ret(g_fields.size()); }},
        {"NewStringUTF", [](Ctx& c) { c.ret(c.u(1) ? jni_new_string(c.s(1)) : 0); }},
        {"GetStringUTFLength", [](Ctx& c) { c.ret(jstr(c.u(1)).size()); }},
        {"GetStringLength", [](Ctx& c) { c.ret(jstr(c.u(1)).size()); }},
        {"GetStringUTFChars", [](Ctx& c) { if (c.u(2)) gref<uint8_t>(c.u(2)) = 1; c.ret(gstrdup(jstr(c.u(1)).c_str())); }},
        {"ReleaseStringUTFChars", [](Ctx& c) { gfree(c.u(2)); }},
        {"GetStringUTFRegion", [](Ctx& c) { std::string s = jstr(c.u(1)).substr(c.u(2), c.u(3)); memcpy(g2h(c.word(4)), s.c_str(), s.size() + 1); }},
        {"GetArrayLength", [](Ctx& c) { std::lock_guard<std::mutex> lk(g_jmx); auto it = g_arrays.find(c.u(1)); c.ret(it == g_arrays.end() ? 0 : it->second.len); }},
        {"NewObjectArray", [](Ctx& c) { c.ret(new_array('L', c.u(1))); }},
        {"GetObjectArrayElement", [](Ctx& c) { std::lock_guard<std::mutex> lk(g_jmx); auto it = g_arrays.find(c.u(1)); c.ret(it == g_arrays.end() ? 0 : gref(it->second.data + 4 * c.u(2))); }},
        {"SetObjectArrayElement", [](Ctx& c) { std::lock_guard<std::mutex> lk(g_jmx); auto it = g_arrays.find(c.u(1)); if (it != g_arrays.end()) gref(it->second.data + 4 * c.u(2)) = c.u(3); }},
        {"RegisterNatives", [](Ctx& c) { c.ret(0); }}, {"UnregisterNatives", [](Ctx& c) { c.ret(0); }},
        {"MonitorEnter", [](Ctx& c) { c.ret(0); }}, {"MonitorExit", [](Ctx& c) { c.ret(0); }},
        {"GetJavaVM", [](Ctx& c) { gref(c.u(1)) = g_vm; c.ret(0); }},
        {"ExceptionDescribe", [](Ctx&) {}}, {"ExceptionClear", [](Ctx&) {}},
        {"DeleteGlobalRef", [](Ctx&) {}}, {"DeleteLocalRef", [](Ctx&) {}}, {"DeleteWeakGlobalRef", [](Ctx&) {}},
    };
    auto it = h.find(fn);
    if (it != h.end()) { it->second(c); return; }
    // New<Type>Array / Get<Type>ArrayElements / Release... / Get/Set<Type>ArrayRegion
    static const char* types[] = {"Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double"};
    static const char codes[] = "ZBCSIJFD";
    for (int k = 0; k < 8; k++) {
        std::string T = types[k];
        if (fn == "New" + T + "Array") { c.ret(new_array(codes[k], c.u(1))); return; }
        if (fn == "Get" + T + "ArrayElements") {
            if (c.u(2)) gref<uint8_t>(c.u(2)) = 0;
            std::lock_guard<std::mutex> lk(g_jmx); auto a = g_arrays.find(c.u(1)); c.ret(a == g_arrays.end() ? 0 : a->second.data); return;
        }
        if (fn == "Release" + T + "ArrayElements") return;
        if (fn == "Get" + T + "ArrayRegion" || fn == "Set" + T + "ArrayRegion") {
            std::lock_guard<std::mutex> lk(g_jmx); auto a = g_arrays.find(c.u(1));
            if (a == g_arrays.end()) return;
            uint32_t esz = codes[k] == 'J' || codes[k] == 'D' ? 8 : codes[k] == 'S' || codes[k] == 'C' ? 2 : codes[k] == 'B' || codes[k] == 'Z' ? 1 : 4;
            uint8_t* arr = (uint8_t*)g2h(a->second.data) + c.u(2) * esz; uint8_t* buf = (uint8_t*)g2h(c.word(4));
            if (fn[0] == 'G') memcpy(buf, arr, c.u(3) * esz); else memcpy(arr, buf, c.u(3) * esz);
            return;
        }
        // Get[Static]<Type>Field / Set[Static]<Type>Field
        if (fn == "Get" + T + "Field" || fn == "GetStatic" + T + "Field") {
            uint32_t id = c.u(2); auto& f = g_fields[id ? id - 1 : 0];
            ret_typed(c, codes[k], java_field(f.first, f.second)); return;
        }
        if (fn == "Set" + T + "Field" || fn == "SetStatic" + T + "Field") return;
    }
    if (fn == "GetObjectField" || fn == "GetStaticObjectField") { uint32_t id = c.u(2); auto& f = g_fields[id ? id - 1 : 0]; c.ret((uint32_t)java_field(f.first, f.second)); return; }
    if (fn == "SetObjectField" || fn == "SetStaticObjectField") return;
    logf("jni: UNIMPLEMENTED %s (lr=%s)", fn.c_str(), addr_name(reg(14)).c_str());
    c.ret(0);
}

static void jvm_dispatch(Ctx& c) {
    std::string fn = g_cur_hle + 4;
    if (fn == "GetEnv" || fn == "AttachCurrentThread" || fn == "AttachCurrentThreadAsDaemon") { if (c.u(1)) gref(c.u(1)) = g_env; c.ret(0); return; }
    c.ret(0);
}

void jni_init() {
    constexpr uint32_t N = sizeof(kJniNames) / sizeof(*kJniNames);
    gaddr table = gmalloc(N * 4);
    for (uint32_t i = 0; i < N; i++) {
        static std::vector<std::string> names;   // stub names must outlive the stubs
        names.push_back(std::string("JNI_") + kJniNames[i]);
        gref(table + 4 * i) = hle_stub(strdup(names.back().c_str()), jni_dispatch);
    }
    g_env = gmalloc(4); gref(g_env) = table;
    static const char* vmn[] = {"JVM_r0", "JVM_r1", "JVM_r2", "JVM_DestroyJavaVM", "JVM_AttachCurrentThread", "JVM_DetachCurrentThread", "JVM_GetEnv", "JVM_AttachCurrentThreadAsDaemon"};
    gaddr vt = gmalloc(8 * 4);
    for (int i = 0; i < 8; i++) gref(vt + 4 * i) = hle_stub(vmn[i], jvm_dispatch);
    g_vm = gmalloc(4); gref(g_vm) = vt;
}

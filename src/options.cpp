// A "Graphics" page inside the game's own Options screen (console UI).
// The Options list gets one more entry; picking it opens the game's Audio page in "graphics mode",
// built with the same widgets: particle and background detail sliders (the page only drives its first
// two slider rows), then PRESET and RUMBLE rows that cycle with A.
// Values live in gw3nx.ini (see overlay.cpp) and apply when the game next rebuilds its effects.
// Also: the game's Achievements tile opens the Credits screen in "achievements mode", fed an
// achievements list in the credits XML format, so the list uses the game's own scrolling page.
#include "core.hpp"
#include "overlay.hpp"
#include <cstring>
#include <cmath>
#include <cctype>
#include <string>
#include <strings.h>

namespace {
GLib* g_gw;
gaddr sym(const char* n) { gaddr a = lib_sym(g_gw, n); if (!a) logf("options: %s not found", n); return a; }

gaddr o_setmenu, o_opt_event, o_aud_init, o_aud_back, o_aud_event, o_title, o_title2;   // originals
gaddr f_get_label, f_set_text;     // UIConsoleMenuItem::GetLabel, UILabel::SetText
gaddr g_preset_item, g_rumble_item;
int g_preset = -1, g_rumble = 3;   // shown values (-1 = custom)
const float kRumble[] = {0.0f, 0.35f, 0.65f, 1.0f};
const char* kRumbleName[] = {"OFF", "LOW", "MEDIUM", "HIGH"};
gaddr L_preset[8], L_rumble_v[4];  // "PRESET: ..." / "RUMBLE: ..." labels (guest strings, made once)
gaddr vt_options, f_text, f_menu_create, f_base_init, f_set_slider, f_get_slider, f_front_back;
gaddr g_item;           // our entry in the Options list
gaddr g_audio_item;     // the Options entry that opens the Audio page
bool g_mode;            // the next Audio page is the Graphics page
gaddr g_page;           // the live Graphics page (an AudioOptionsConsoleController)
gaddr g_sl[2];          // its slider items
gaddr L_title, L_part, L_bg, L_rumble, L_ach;
gaddr f_enter, g_menuseq;   // C_MenuSequence::EnterMenu, gMenuSequence
bool g_ach_mode;                     // the Credits screen being entered / shown is the achievements list
const float kDetailMax = 1.5f;   // slider end = the "Max" preset

uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

// UIConsoleMenuController::SetMenu(this, menu): append our entry before the Options list is laid out
void h_setmenu(Ctx& c) {
    gaddr self = c.u(0), menu = c.u(1);
    if (gref(self) == vt_options) {
        g_item = (gaddr)call_guest(gref(gref(menu) + 0x58), {menu, L_title, 3});
        logf("options: added Graphics entry");
    }
    c.ret((uint32_t)call_guest(o_setmenu, {self, menu}));
}

// OptionsConsoleController::OnMenuEvent(this, node, event, ctx)
void h_opt_event(Ctx& c) {
    gaddr self = c.u(0), node = c.u(1);
    g_mode = false;
    if (node && node == g_item && g_audio_item) { g_mode = true; node = gref(self + g_audio_item); }
    c.ret((uint32_t)call_guest(o_opt_event, {self, node, c.u(2), c.u(3)}));
}

// Borrowed pages are titled after what they show (Audio -> GRAPHICS, Credits -> ACHIEVEMENTS)
gaddr retitle(gaddr text) {
    if (!text) return text;
    if ((g_mode || g_page) && !strcmp(gstr(text), "AUDIO")) return L_title;
    if (g_ach_mode && !strcmp(gstr(text), "CREDITS")) return L_ach;
    return text;
}
// FrontendTitleBar::UpdateMainTitleText(this, text)
void h_title(Ctx& c) { call_guest(o_title, {c.u(0), retitle(c.u(1))}); }
// NavigationManager::SetMainTitleText(this, text, transition): the path page changes use
void h_title2(Ctx& c) { call_guest(o_title2, {c.u(0), retitle(c.u(1)), c.u(2)}); }

// AudioOptionsConsoleController::Init(this): the same page, our sliders
void h_aud_init(Ctx& c) {
    gaddr self = c.u(0);
    if (!g_mode) { call_guest(o_aud_init, {self}); return; }
    g_mode = false;
    g_page = self;
    call_guest(f_base_init, {self});
    gref(self + 0x1ac) = fbits(180.0f);   // as the Audio page does
    gaddr menu = (gaddr)call_guest(f_menu_create, {self});
    gaddr add_slider = gref(gref(menu) + 0x88);
    g_sl[0] = (gaddr)call_guest(add_slider, {menu, L_part});
    g_sl[1] = (gaddr)call_guest(add_slider, {menu, L_bg});
    GwSettings& st = gw_settings();
    g_preset = gw_preset_match(st.particles, st.background);
    g_rumble = 0;
    for (int i = 0; i < 4; i++) if (fabsf(kRumble[i] - st.rumble) < fabsf(kRumble[g_rumble] - st.rumble)) g_rumble = i;
    gaddr add_item = gref(gref(menu) + 0x58);
    g_preset_item = (gaddr)call_guest(add_item, {menu, L_preset[g_preset + 1], 3});
    g_rumble_item = (gaddr)call_guest(add_item, {menu, L_rumble_v[g_rumble], 3});
    gref(self + 0x1b0) = g_sl[0];   // the page's own fields (its input handler looks at the first two)
    gref(self + 0x1b4) = g_sl[1];
    gref(self + 0x198) = 4;
    call_guest(o_setmenu, {self, menu});
    float pos[2] = {st.particles / kDetailMax, st.background / kDetailMax};
    for (int i = 0; i < 2; i++) call_guest(f_set_slider, {gref(g_sl[i] + 0x248), fbits(pos[i])});
    gref(self + 0x1c4) = fbits(-1.0f);
    logf("options: Graphics page open");
}

void set_label(gaddr item, gaddr text) {
    if (gaddr label = (gaddr)call_guest(f_get_label, {item})) call_guest(f_set_text, {label, text});
}

// AudioOptionsConsoleController::OnMenuEvent(this, node, event, ctx): A on PRESET / RUMBLE cycles them
void h_aud_event(Ctx& c) {
    gaddr self = c.u(0), node = c.u(1);
    if (self == g_page && node && (node == g_preset_item || node == g_rumble_item)) {
        if (node == g_preset_item) {
            g_preset = (g_preset + 1) % gw_preset_count();   // custom -> first preset
            float p, b; gw_preset_values(g_preset, p, b);
            call_guest(f_set_slider, {gref(g_sl[0] + 0x248), fbits(p / kDetailMax)});
            call_guest(f_set_slider, {gref(g_sl[1] + 0x248), fbits(b / kDetailMax)});
            set_label(g_preset_item, L_preset[g_preset + 1]);
        } else {
            g_rumble = (g_rumble + 1) % 4;
            set_label(g_rumble_item, L_rumble_v[g_rumble]);
        }
        c.ret(1);
        return;
    }
    c.ret((uint32_t)call_guest(o_aud_event, {self, node, c.u(2), c.u(3)}));
}

// AudioOptionsConsoleController::MenuBack(this): store our sliders instead of the volumes
void h_aud_back(Ctx& c) {
    gaddr self = c.u(0);
    if (self != g_page) { c.ret((uint32_t)call_guest(o_aud_back, {self})); return; }
    g_page = 0;
    float v[2];
    for (int i = 0; i < 2; i++) v[i] = bitsf((uint32_t)call_guest(f_get_slider, {gref(g_sl[i] + 0x248)}));
    GwSettings& s = gw_settings();
    s.particles = v[0] * kDetailMax; s.background = v[1] * kDetailMax; s.rumble = kRumble[g_rumble];
    gw_settings_save();
    logf("options: Graphics -> particles %.2f, background %.2f, rumble %.2f", s.particles, s.background, s.rumble);
    uint32_t r = (uint32_t)call_guest(f_front_back, {self});
    gref<uint8_t>(self + 0x1c8) = (uint8_t)r;
    c.ret(r);
}
}  // namespace

bool options_show_achievements() {
    if (!f_enter || !g_menuseq) return false;
    g_ach_mode = true;
    const uint32_t id = 9;   // the Credits screen (OptionsConsoleController::OnMenuEvent: CREDITS entry -> EnterMenu(9))
    logf("options: Achievements -> credits screen");
    call_guest(f_enter, {g_menuseq, id});
    return true;
}

const std::string* options_asset(const char* name) {
    static std::string xml;
    size_t n = strlen(name);
    if (!g_ach_mode || n < 16 || strcasecmp(name + n - 16, "GWarsCredits.xml")) return nullptr;
    xml = overlay_achievements_xml();
    return &xml;
}

void options_install(GLib* gw) {
    g_gw = gw;
    gaddr a_setmenu = sym("_ZN23UIConsoleMenuController7SetMenuEP13UIConsoleMenu");
    gaddr a_opt_event = sym("_ZN24OptionsConsoleController11OnMenuEventEP6UINode14E_UIInputEventRK9GfContext");
    gaddr a_aud_init = sym("_ZN29AudioOptionsConsoleController4InitEv");
    gaddr a_aud_back = sym("_ZN29AudioOptionsConsoleController8MenuBackEv");
    gaddr vt = sym("_ZTV24OptionsConsoleController");
    f_text = sym("_Z16GetLocalisedText5E_LID");
    f_menu_create = sym("_ZN25UIConsoleMenuNonScrolling6CreateEP24C_FrontendMenuController");
    f_base_init = sym("_ZN23UIConsoleMenuController4InitEv");
    f_set_slider = sym("_ZN8UISlider17SetSliderPositionEf");
    f_get_slider = sym("_ZNK8UISlider16GetSliderPositonEv");
    f_front_back = sym("_ZN24C_FrontendMenuController8MenuBackEv");
    if (!a_setmenu || !a_opt_event || !a_aud_init || !a_aud_back || !vt || !f_menu_create || !f_base_init || !f_set_slider || !f_get_slider || !f_front_back) return;
    vt_options = vt + 8;
    L_title = gstrdup("GRAPHICS");
    L_part = gstrdup("PARTICLE DETAIL");
    L_bg = gstrdup("BACKGROUND DETAIL");
    L_rumble = gstrdup("RUMBLE");
    L_preset[0] = gstrdup("PRESET: CUSTOM");
    for (int i = 0; i < gw_preset_count() && i < 7; i++) {
        std::string full = gw_preset_name(i), n = "PRESET: " + full.substr(0, full.find(' '));   // short: ANDROID / CONSOLE / MAX
        for (char& ch : n) ch = (char)toupper((unsigned char)ch);
        L_preset[i + 1] = gstrdup(n.c_str());
    }
    for (int i = 0; i < 4; i++) L_rumble_v[i] = gstrdup((std::string("RUMBLE: ") + kRumbleName[i]).c_str());
    f_get_label = sym("_ZN17UIConsoleMenuItem8GetLabelEv");
    f_set_text = sym("_ZN7UILabel7SetTextEPKc");
    g_audio_item = 0x1b0;   // the AUDIO entry (text 112), first in the list
    if (const char* e = getenv("GW3_AUDIO_ITEM")) g_audio_item = strtoul(e, nullptr, 0);
    o_setmenu = hle_hook_wrap(a_setmenu, "UIConsoleMenuController::SetMenu", h_setmenu);
    o_opt_event = hle_hook_wrap(a_opt_event, "OptionsConsoleController::OnMenuEvent", h_opt_event);
    o_aud_init = hle_hook_wrap(a_aud_init, "AudioOptionsConsoleController::Init", h_aud_init);
    o_aud_back = hle_hook_wrap(a_aud_back, "AudioOptionsConsoleController::MenuBack", h_aud_back);
    if (gaddr a = sym("_ZN29AudioOptionsConsoleController11OnMenuEventEP6UINode14E_UIInputEventRK9GfContext"))
        o_aud_event = hle_hook_wrap(a, "AudioOptionsConsoleController::OnMenuEvent", h_aud_event);
    if (gaddr a = sym("_ZN16FrontendTitleBar19UpdateMainTitleTextEPKc")) o_title = hle_hook_wrap(a, "FrontendTitleBar::UpdateMainTitleText", h_title);
    if (gaddr a = sym("_ZN17NavigationManager16SetMainTitleTextEPKc20E_TitleBarTransition")) o_title2 = hle_hook_wrap(a, "NavigationManager::SetMainTitleText", h_title2);
    // achievements list on the Credits screen
    L_ach = gstrdup("ACHIEVEMENTS");
    f_enter = sym("_ZN14C_MenuSequence9EnterMenuEi");
    g_menuseq = sym("gMenuSequence");
    if (gaddr a = sym("_ZN24CreditsConsoleControllerD2Ev")) hle_hook_pre(a, "CreditsConsoleController::~", [](Ctx&) { g_ach_mode = false; });
    // The main menu's Achievements tile asks SignedInToAppStore() first and otherwise shows "You need to be
    // signed in to Google Play". Answer yes at that one call site (1.3.0 build 101; checked before patching).
    if (gaddr show = sym("_ZN8Services18ShowAchievementsUIEi")) {
        gaddr at = show - 0x4367a0 + 0x318fdc;
        if (gref(at) == 0xeb0472a3) { gref(at) = 0xE3A00001; logf("options: Achievements tile no longer needs Google Play"); }   // mov r0, #1
        else logf("options: Achievements tile check not found (different game version)");
    }
}

// debug: log the labels of the console Options entries (to see which one is Audio)
void options_log_labels() {
    if (!f_text) return;
    for (uint32_t lid : {112u, 113u, 283u, 11u, 12u, 13u})
        if (gaddr t = (gaddr)call_guest(f_text, {lid})) logf("options: text %u = %s", lid, gstr(t));
}

#pragma once
struct GLib;
void options_install(GLib* gw);   // Options > Graphics (options.cpp)
void options_log_labels();
bool options_show_achievements();                 // open the native list (false: not available)
#include <string>
const std::string* options_asset(const char* name);   // replacement asset contents, or nullptr
std::string overlay_achievements_xml();             // the achievements as a credits-format XML
#include <cstdint>
// Port settings, saved in gw3nx.ini; shared by the gw3nx menu and the game's Options > Graphics page.
struct GwSettings { float particles = 0.45f, background = 0.325f, rumble = 1.0f; bool fps = false; };
GwSettings& gw_settings();
int gw_preset_count();                                   // effect presets (Android High / Console / Max)
const char* gw_preset_name(int i);
void gw_preset_values(int i, float& particles, float& background);
int gw_preset_match(float particles, float background);  // -1 = custom
void gw_settings_save();
// Port settings overlay (hold - / Back for 1 s): effects detail presets, saved to gw3nx.ini.
#include <vector>
bool asset_load(const char* path, std::vector<uint8_t>& out);   // a file from the game's OBB (hle_android.cpp)
void overlay_init();                                       // after g_data_dir is set
void overlay_detail(float& particles, float& background);  // the chosen preset
bool overlay_input(uint32_t* masks, int n, uint32_t now_ms); // true = menu is up, masks were cleared
void overlay_draw(int w, int h, uint32_t now_ms);          // after the game's frame, before swap
void overlay_achievement(int index, uint32_t now_ms);    // the game unlocked E_Achievement index
void overlay_show_achievements();                        // open the list (the game's Achievements button)

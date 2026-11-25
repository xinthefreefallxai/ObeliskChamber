// SPDX-License-Identifier: GPL-3.0-or-later
// Fire Emblem: Fortune's Weave: the recomp's settings as a screen of the game's own ([fefw] native_options, default on). "Recomp Options"
// under + > System (ObeliskChamber_sysmenu.cpp) opens a second copy of the game's Options screen whose rows and tabs are ours: Display, Graphics,
// Frame Rate, Audio. No lifted-code site is needed: everything goes through the game's vtables.
//
// The game's Options screen (main of update 131072; names after the community decompilation's, addresses are this build's):
//   create                0x16a3f80(parent, 0, flag)  allocates the menu (0x440 bytes, class ProcConfigMenu, vtable 0x20ba658, 143 slots), sets its
//                         three tabs (PageTab at +0x290: last index, index, previous) and the per-tab cursor vectors (+0x310, +0x328: one
//                         int per tab), makes the process, then calls slot 128 (builds the rows) and slot 129 (shows the current tab's rows)
//   slot 128  0x16a4a00   one item per entry of the table at +0x428 (count +0x430): allocate, fill, BasicMenu::AddItem
//   slot 129  0x16a5840   keeps the items whose entry's first byte is the tab's page (page = tab index when the order vector +0x2a0 is empty)
//   slot 84   0x16a4620   OnInit: the tab bar ("L_PageTab") gets its labels from a std::function (page -> std::string) through
//                         PageTab::SetupText 0x171aec0(tab, weak part, function, input layer = slot 24, 4, 5); the game's function reads a
//                         table of three message ids, so a fourth tab needs ours
//   slot 88   0x16a4710   the help line: "L_InfoTxt/T_Txt" of the menu's layout (+0x350) = the message of the current item's (+0x150) entry
//   slot 114  0x16a48e0   minus: "reset the game's options?"            slot 142  0x16a5940  key help (adds Reset and ItemChange)
//   a choice row          class ListMenuItem, 0x90 bytes, vtable 0x20bb2a8 (71 slots): +0x50 = 1, +0x51 = 1, +0x52 = 1, +0x58 = its entry,
//                         +0x60 = 1 (decidable), +0x78 / +0x80 = weak "L_Base" part (set by slot 16), +0x88 = 0 (text, not a gauge),
//                         +0x8c = the game's setting id. Slot 2 GetText (the label), 39 next value, 40 previous value (both: (item, wrap)
//                         -> changed; A calls 39 with wrap), 55 redraw (calls 70), 70 writes the value's name to "T_Txt"
//   text                  0x1594ed0(layout, pane, utf8) sets a text box to a plain string (0x1594de0 is the same by message id)
//
// So: slot 128 of the game's menu vtable points at a thunk. Opened from our entry, it gives the new menu a vtable of ours (a copy with slots
// 84, 88, 114 and 142 replaced), a fourth tab, and one ListMenuItem per row below with a vtable of ours (a copy with slots 2, 39, 40 and 70
// replaced), so labels, values and help are plain strings and the values are the recomp's. Opened by the game, the thunk runs the game's.
// Every row is a choice row: numbers step through fixed values.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

#include "ObeliskChamber_game.h"
#include "ObeliskChamber_presets.h"
#include "nxrt/abi.h"
#include "nxrt/audio_sink.h"
#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/extras.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/nvn_backend.h"
#include "nxrt/mod_api.h"
#include "nxrt/platform.h"
#include "nxrt/services_api.h"
#include "play_selftest.h"   // play::exit_process (Quit Game)
#include "play_types.h"
#include "play_ui.h"

namespace nxrt::games::ObeliskChamber {
namespace {
constexpr const char* kTag = "fefw.optmenu";
// functions
constexpr uint64_t kCreate = 0x16a3f80, kBuild = 0x16a4a00, kOnInit = 0x16a4620, kHelp = 0x16a4710, kMinus = 0x16a48e0, kKeyHelp = 0x16a5940;
constexpr uint64_t kAllocate = 0x652310, kAddItem = 0x1714cc0, kVecAppend = 0x6e7500, kGetPart = 0x1592df0, kSetupText = 0x171aec0;
constexpr uint64_t kReleaseWeak = 0x1a41fb0, kSetText = 0x1594ed0, kFindAlign = 0x1596b80, kAlignUpdate = 0x56b600, kArrow = 0x15d1600;
constexpr uint64_t kMapNotify = 0x82a660, kItemNext = 0x16a6a40, kItemPrev = 0x16a6b10, kItemValue = 0x16a6d50, kTabInvoke = 0x16a5d30;
// data
constexpr uint64_t kHeapGot = 0x2169fa8, kVtMenu = 0x20ba658, kVtList = 0x20bb2a8, kVtTabFn = 0x20bac60, kMapInfo = 0x220d188, kUiCount = 0x2231f30;
constexpr uint64_t kStrTxt = 0x1af52b4, kStrInfoTxt = 0x1b192d8, kStrInfo = 0x1b19081, kStrPageTab = 0x1b083e6, kStrResetMid = 0x1ac44bf;
// layouts
constexpr int kMenuSlots = 143, kItemSlots = 71, kTabFnSlots = 9;
constexpr int kMSlotLayer = 24, kMSlotOnInit = 84, kMSlotHelp = 88, kMSlotMinus = 114, kMSlotBuild = 128, kMSlotKeyHelp = 142;
constexpr int kISlotText = 2, kISlotNext = 39, kISlotPrev = 40, kISlotRedraw = 55, kISlotReady = 58, kISlotValue = 70, kFSlotInvoke = 6;
constexpr uint64_t kMenuCurrent = 0x150, kMenuTab = 0x290, kMenuCursorA = 0x310, kMenuCursorB = 0x328, kMenuLayout = 0x350, kMenuCount = 0x430;
constexpr uint64_t kItemBytes = 0x90, kItemEntry = 0x58, kItemDecidable = 0x60, kItemBase = 0x78, kItemSetting = 0x8c;

uint64_t g_base = 0;
bool g_on = true, g_ready = false, g_failed = false;
// Two screens are made from the class: the options (g_mvt, g_ivt) and a pick list for the save slots (g_mvt2, g_ivt2: ObeliskChamber_saveslot.cpp).
enum { kScreenNone, kScreenOptions, kScreenList };
int g_pending = kScreenNone;         // our entry is inside the game's create call: the menu it builds is ours
uint64_t g_mvt[2 + kMenuSlots] = {}, g_ivt[2 + kItemSlots] = {}, g_fvt[2 + kTabFnSlots] = {};
uint64_t g_mvt2[2 + kMenuSlots] = {}, g_ivt2[2 + kItemSlots] = {};
OptList g_list;                      // the pick list being shown
constexpr int kListMax = 2001;       // a save tab ("New" and the folders 000 to 999) and a load tab (the folders)
constexpr int kListPages = 3;        // the tab bar's own count: no more cursor slots needed
constexpr uint64_t kBaseNext = 0x874dc0, kBasePrev = 0x874dd0;   // BasicMenuItem's own slots 39 / 40: no value, no change
constexpr int kISlotACall = 25, kISlotBCall = 26, kISlotUpdate = 18;
// The game's reset prompt (slot 114, 0x16a48e0): 0x878e20(menu, &message id, &yes, &no, &empty) with std::function objects whose first
// word is their vtable; "yes" (vtable 0x20bad10) holds the menu at +8, "no" (0x20bad90) holds nothing. Ours is the same prompt with a "yes"
// of our own: a copy of that vtable whose invoke (slot 6) and the two clones (slots 2 and 3, which write the vtable) are thunks.
constexpr uint64_t kPrompt = 0x878e20, kVtResetYes = 0x20bad10, kVtResetNo = 0x20bad90, kStrResetAsk = 0x1afac9e;
constexpr int kFSlotCloneNew = 2, kFSlotCloneAt = 3;
constexpr uint64_t kMenuItems = 0xe8;    // BasicMenu: its items, a vector of pointers (begin, end)
uint64_t g_yvt[2 + 9] = {};              // "yes" of the reset prompt (kTabFnSlots slots)
uint64_t g_yes_new = 0, g_yes_at = 0;    // the game's own clones
bool g_reset_ok = false;
constexpr uint32_t kClose = 3;       // what B answers: the menu closes and its parent takes over again
std::atomic<uint64_t> g_opened{0};
const char kEmpty[] = "";

template <class T> bool rd(uint64_t a, T* out) {
    if (!a || !mem::is_mapped(a, sizeof(T))) return false;
    std::memcpy(out, reinterpret_cast<const void*>(a), sizeof(T));
    return true;
}
template <class T> void wr(uint64_t a, T v) { std::memcpy(reinterpret_cast<void*>(a), &v, sizeof(T)); }
uint64_t ptr(const void* p) { return reinterpret_cast<uint64_t>(p); }

// fn(args...) on the guest thread, x8 for a returned object; the registers are put back, x0 is returned.
uint64_t call(::Ctx* c, uint64_t fn, std::initializer_list<uint64_t> args, uint64_t x8 = 0) {
    const ::Ctx saved = *c;
    int i = 0;
    for (uint64_t a : args) c->x[i++] = a;
    c->x[8] = x8;
    dispatch_call(c, fn);
    const uint64_t r = c->x[0];
    *c = saved;
    return r;
}
// the game's own function behind a slot we replaced, as if the slot still pointed at it
void pass(::Ctx* c, uint64_t fn) {
    const uint64_t lr = c->x[30], self = c->fn;
    dispatch_call(c, fn);
    c->x[30] = lr;
    c->fn = self;
}

// ---- the rows ---------------------------------------------------------------------------------------------------------------------------
// A row is a list of values: count(), the current one (get(), -1 when the setting holds something else), set(i) and name(i) (i = -1: what
// the setting holds now). The value box is narrow: names are short, the help line says the rest.
enum { kPageDisplay, kPageGraphics, kPageFrameRate, kPageAudio, kPages };
constexpr const char* kTabNames[kPages] = {"Display", "Graphics", "Frame Rate", "Audio"};

struct Row {
    int page;
    const char* label;
    const char* help;
    // a ObeliskChamber.ini key with fixed values
    const char* sec = nullptr;
    const char* key = nullptr;
    const char* def = nullptr;
    const char* const* names = nullptr;
    const char* const* vals = nullptr;
    int n = 0;
    void (*after)() = nullptr;       // applies the new value to the running game
    // or anything else
    int (*count)() = nullptr;
    int (*get)() = nullptr;
    void (*set)(int) = nullptr;
    void (*name)(int, char*, size_t) = nullptr;
};

std::string lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}
// Two ini values that mean the same: numbers by value, the yes / no words as 1 / 0, text without case.
std::string norm(const std::string& v) {
    const std::string l = lower(v);
    if (l == "true" || l == "yes" || l == "on") return "1";
    if (l == "false" || l == "no") return "0";
    char* end = nullptr;
    const double d = std::strtod(l.c_str(), &end);
    if (!l.empty() && end && !*end) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%g", d);
        return buf;
    }
    return l;
}
std::string ini(const char* sec, const char* key, const char* def) { return config_get_str(sec, key, def); }
void ini_write(const char* sec, const char* key, const std::string& value) {
    std::string err;
    if (config_write(sec, key, value, &err)) NXLOG_I(kTag, "[%s] %s = %s written to %s", sec, key, value.c_str(), config_file_path().c_str());
    else NXLOG_W(kTag, "cannot write [%s] %s: %s", sec, key, err.c_str());
}

play::Settings settings() { return play::ui_host()->settings(); }
template <class F> void edit(F&& f) {
    play::Settings s = settings();
    f(s);
    play::ui_host()->apply(s);       // queued: the main thread applies it at its next frame and writes controls.ini
}
void onoff(int i, char* out, size_t n) { std::snprintf(out, n, "%s", i == 1 ? "On" : "Off"); }
int two() { return 2; }

// Display
constexpr const char* kModeKeys[] = {"windowed", "borderless", "exclusive"};
constexpr const char* kModeNames[] = {"Windowed", "Borderless", "Exclusive"};
constexpr int kSizes[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
constexpr extras::UpscaleMode kFilterModes[] = {extras::UpscaleMode::Off, extras::UpscaleMode::Fsr1, extras::UpscaleMode::Nis};
constexpr const char* kFilterNames[] = {"Bilinear", "FSR 1", "NIS"};

int mode_get() {
    const std::string m = settings().display.window_mode;
    for (int i = 0; i < 3; ++i)
        if (m == kModeKeys[i]) return i;
    return -1;
}
int monitor_count() { return 1 + std::max(0, platform::display_count()); }
int monitor_get() {
    const int m = settings().display.monitor;
    return m >= 0 && m < monitor_count() ? m : -1;
}
void monitor_name(int i, char* out, size_t n) {
    char buf[192] = {};
    if (i == 0) std::snprintf(out, n, "Automatic");
    else if (i > 0 && platform::display_name(i, buf, sizeof buf)) std::snprintf(out, n, "%d: %.26s", i, buf);
    else std::snprintf(out, n, "Not connected");
}
int size_get() {
    const play::DisplaySettings d = settings().display;
    for (int i = 0; i < 5; ++i)
        if (kSizes[i][0] == d.width && kSizes[i][1] == d.height) return i;
    return -1;
}
void size_name(int i, char* out, size_t n) {
    const play::DisplaySettings d = settings().display;
    if (i >= 0 && i < 5) std::snprintf(out, n, "%d x %d", kSizes[i][0], kSizes[i][1]);
    else std::snprintf(out, n, "%d x %d", d.width, d.height);
}
int filter_get() {
    const extras::UpscaleMode m = settings().extras.upscale.mode;
    for (int i = 0; i < 3; ++i)
        if (m == kFilterModes[i]) return i;
    return -1;
}

// Graphics
constexpr const char* kWideNames[] = {"Auto", "Off"};
constexpr const char* kWideVals[] = {"auto", "off"};
constexpr const char* kRatioNames[] = {"Game default", "33%", "50%", "59%", "67%", "75%", "85%", "100%"};
constexpr const char* kRatioVals[] = {"0", "0.33", "0.5", "0.59", "0.67", "0.75", "0.85", "1.0"};
constexpr const char* kReflNames[] = {"Full", "Reduced", "Off"};
constexpr const char* kReflVals[] = {"full", "reduced", "off"};
constexpr const char* kOnOffAoNames[] = {"On", "Off"};
constexpr const char* kOnOffAoVals[] = {"1", "0"};
constexpr const char* kDetailNames[] = {"Full", "Reduced"};
constexpr const char* kDetailVals[] = {"full", "reduced"};
constexpr const char* kNpcNames[] = {"0.5x (fewer)", "0.75x (fewer)", "Console", "1.5x", "2x", "3x", "4x"};   // [fefw] npc_distance (ObeliskChamber_crowd.cpp)
constexpr const char* kNpcVals[] = {"0.5", "0.75", "1", "1.5", "2", "3", "4"};
constexpr const char* kOffOnNames[] = {"Off", "On"};
constexpr const char* kOffOnVals[] = {"0", "1"};
constexpr const char* kTargetNames[] = {"Paused", "60 fps", "72 fps", "80 fps", "90 fps", "100 fps", "110 fps", "120 fps", "144 fps"};
constexpr const char* kTargetVals[] = {"0", "60", "72", "80", "90", "100", "110", "120", "144"};
constexpr const char* kMinNames[] = {"50%", "60%", "67%", "70%", "75%", "80%", "85%"};
constexpr const char* kMinVals[] = {"0.5", "0.6", "0.67", "0.7", "0.75", "0.8", "0.85"};
constexpr const char* kGainNames[] = {"Off", "1%", "2%", "3%", "5%"};
constexpr const char* kGainVals[] = {"0", "1", "2", "3", "5"};
constexpr const char* kShadowNames[] = {"Game default", "1024", "1536", "2048", "3072", "4096", "6144", "8192"};
constexpr const char* kShadowVals[] = {"0", "1024", "1536", "2048", "3072", "4096", "6144", "8192"};
constexpr const char* kQualityNames[] = {"Console", "Docked", "Handheld"};
constexpr const char* kQualityVals[] = {"console", "docked", "handheld"};
constexpr const char* kUpscalerNames[] = {"TAAU", "DLSS", "FSR 3", "XeSS", "Bilinear", "Auto"};
constexpr const char* kUpscalerVals[] = {"taau", "dlss", "fsr3", "xess", "bilinear", "auto"};
constexpr const char* kPresetNames[] = {"Default", "K", "J", "L", "M", "E", "F"};
constexpr const char* kPresetVals[] = {"default", "k", "j", "l", "m", "e", "f"};
constexpr const char* kRateNames[] = {"Off", "2x1", "1x2", "2x2"};
constexpr const char* kRateVals[] = {"off", "scene:2x1", "scene:1x2", "scene:2x2"};
constexpr const char* kSharpNames[] = {"Off", "0.25", "0.5", "0.75", "1"};
constexpr const char* kSharpVals[] = {"0", "0.25", "0.5", "0.75", "1"};
constexpr const char* kHistoryNames[] = {"6", "8", "12", "16", "24"};
constexpr const char* kHistoryVals[] = {"6", "8", "12", "16", "24"};
constexpr const char* kFgNames[] = {"Off", "DLSS", "FSR 3"};
constexpr const char* kFgVals[] = {"0", "dlss", "fsr3"};

void dynres_after() {
    nxrt::nvn::backend_dynres_set(std::atof(ini("nvn", "dynres_target", "0").c_str()), std::atof(ini("nvn", "dynres_min", "0.7").c_str()),
                                  std::atof(ini("nvn", "dynres_gain", "3").c_str()));
}
// Settings that used to need a restart (ObeliskChamber_res.cpp, ObeliskChamber_play.cpp, the upscaler): the new value is applied to the running game.
void scale_after() { res_live_apply(false); }
void rebuild_after() { res_live_apply(true); }
void dynres_switch_after() {
    if (res_dynres_live(norm(ini("nvn", "dynres", "0")) == "1")) dynres_after();
}
void tuning_after() {
    nxrt::nvn::backend_upscale_tuning(std::atof(ini("dlss", "history_weight", "12").c_str()), std::atof(ini("dlss", "sharpness", "0").c_str()));
}
void smooth_after() {
    const std::string v = norm(ini("fefw", "smooth_step", "auto"));
    play_set_smooth(v == "1" ? 1 : v == "0" ? 0 : -1);
}
void frame_fixes_after() { frame_set_enabled(norm(ini("fefw", "frame_fixes", "1")) != "0"); }
void upscaler_after() { nxrt::nvn::backend_set_upscaler(ini("dlss", "upscaler", "taau").c_str()); }
void preset_after() { nxrt::extras::set_dlss_preset_override(std::max(0, nxrt::extras::dlss_preset_from_word(lower(ini("dlss", "preset", "default"))))); }
void rate_after() { nxrt::nvn::backend_set_shading_rate(lower(ini("nvn", "shading_rate", "off")).c_str()); }
// Output Size: the shared list (ObeliskChamber_presets.h kOutputChoices), then "Monitor WxH" (the display the window is on) when known.
// Names are short for the narrow value box: the shape first, so the ultrawide sizes read as such.
int output_count() { return kOutputChoiceCount + (monitor_output_value().empty() ? 0 : 1); }
std::string output_val(int i) {
    if (i >= 0 && i < kOutputChoiceCount) return kOutputChoices[i].value;
    return i == kOutputChoiceCount ? monitor_output_value() : std::string();
}
int output_get() {
    std::string v = lower(ini("fefw", "output", "console"));
    if (v == "4k") v = "2160p";
    for (int i = 0; i < output_count(); ++i)
        if (v == output_val(i)) return i;
    return -1;
}
void output_name(int i, char* out, size_t n) {
    if (i >= 0 && i < kOutputChoiceCount) {
        const OutputChoice& o = kOutputChoices[i];
        if (!*o.shape) std::snprintf(out, n, "%s", !std::strcmp(o.value, "window") ? "Window Size" : o.size);
        else {
            int w = 0, h = 0;
            std::sscanf(o.size, "%d x %d", &w, &h);
            std::snprintf(out, n, "%s  %dx%d", o.shape, w, h);
        }
    } else if (i == kOutputChoiceCount && !monitor_output_value().empty()) {
        std::snprintf(out, n, "Monitor %s", monitor_output_value().c_str());
    } else {
        std::snprintf(out, n, "%.24s", ini("fefw", "output", "console").c_str());
    }
}
void output_set(int i) {
    const std::string v = output_val(i);
    if (!v.empty()) ini_write("fefw", "output", v);
}
// Graphics Preset: render scale, shadow resolution and quality together (ObeliskChamber_presets.h); -1 (Custom) when they hold anything else.
int preset_count() { return kGraphicsPresetCount; }
int preset_get() { return graphics_preset_current(); }
void preset_set(int i) {
    const GraphicsPreset& p = kGraphicsPresets[std::clamp(i, 0, kGraphicsPresetCount - 1)];
    for (int k = 0; k < 7; ++k) ini_write("fefw", kPresetKeys[k], preset_value(p, k));
    NXLOG_I(kTag, "graphics preset %s: render_ratio %s, shadow_size %s, quality %s, reflections %s, ambient_occlusion %s, crowd_detail %s, scenery_detail %s", p.name,
                p.render_ratio, p.shadow_size, p.quality, p.reflections, p.ambient_occlusion, p.crowd_detail, p.scenery_detail);
    rebuild_after();     // one live rebuild for all three
}
void preset_name(int i, char* out, size_t n) { std::snprintf(out, n, "%s", i >= 0 && i < kGraphicsPresetCount ? kGraphicsPresets[i].name : "Custom"); }
int fg_get() {
    std::string v = lower(ini("dlss", "frame_gen", "0"));
    if (v == "1") v = "dlss";
    if (v == "3") v = "fsr3";
    for (int i = 0; i < 3; ++i)
        if (norm(v) == norm(kFgVals[i])) return i;
    return -1;
}
int three() { return 3; }
void fg_name(int i, char* out, size_t n) {
    if (i >= 0 && i < 3) std::snprintf(out, n, "%s", kFgNames[i]);
    else std::snprintf(out, n, "%.24s", ini("dlss", "frame_gen", "0").c_str());
}

// Frame rate
constexpr int kFpsChoices[] = {-1, 0, 30, 60, 72, 90, 120, 144, 165, 240, 360};   // -1 = the refresh rate (play_ui.cpp)
constexpr const char* kSmoothNames[] = {"Auto", "On", "Off"};
constexpr const char* kSmoothVals[] = {"auto", "1", "0"};
constexpr const char* kLatencyNames[] = {"Off", "Auto", "On", "Boost"};
bool ini_on(const char* key) { return norm(ini("fefw", key, "0")) == "1"; }
int rate_get() { return ini_on("fps120") ? 2 : ini_on("fps60") ? 1 : 0; }
void rate_set(int i) {
    if (ini_on("fps240")) ini_write("fefw", "fps240", "0");   // the F1 menu's 240 (testing) gives way to a choice made here
    if (i == 2) {
        ini_write("fefw", "fps120", "1");
    } else {
        ini_write("fefw", "fps60", i == 1 ? "1" : "0");
        if (ini_on("fps120")) ini_write("fefw", "fps120", "0");
    }
    play_set_rate(i == 2 ? 120 : i == 1 ? 60 : 30);
}
void rate_name(int i, char* out, size_t n) { std::snprintf(out, n, "%s", i == 2 ? "120 fps" : i == 1 ? "60 fps" : "30 fps"); }
int cap_count() { return static_cast<int>(std::size(kFpsChoices)); }
int cap_get() {
    const int v = settings().display.fps_cap;
    for (int i = 0; i < cap_count(); ++i)
        if (kFpsChoices[i] == v) return i;
    return -1;
}
void cap_name(int i, char* out, size_t n) {
    const int v = i >= 0 && i < cap_count() ? kFpsChoices[i] : settings().display.fps_cap;
    if (v < 0) std::snprintf(out, n, "Refresh rate");
    else if (v == 0) std::snprintf(out, n, "Unlimited");
    else std::snprintf(out, n, "%d fps", v);
}
int four() { return 4; }

// Audio
int volume_count() { return 41; }   // 0 to 200% in steps of 5
int volume_get() { return std::clamp(static_cast<int>(std::lround(settings().audio.volume * 20.0)), 0, 40); }
void volume_set(int i) {
    const double v = std::clamp(i, 0, 40) * 0.05;
    audio::set_master_volume(static_cast<float>(v));
    edit([v](play::Settings& s) { s.audio.volume = v; });
}
void volume_name(int i, char* out, size_t n) { std::snprintf(out, n, "%d%%", (i < 0 ? volume_get() : i) * 5); }
std::vector<std::string>& devices() {
    static std::vector<std::string> names = [] {
        std::vector<std::string> v;
        for (const audio::OutputDevice& d : audio::list_output_devices())
            if (!d.name.empty()) v.push_back(d.name);
        return v;
    }();
    return names;
}
int device_count() { return 1 + static_cast<int>(devices().size()); }
int device_get() {
    const std::string d = settings().audio.device;
    if (d.empty() || d == "default") return 0;
    for (size_t i = 0; i < devices().size(); ++i)
        if (d == devices()[i]) return static_cast<int>(i) + 1;
    return -1;
}
void device_name(int i, char* out, size_t n) {
    if (i == 0) std::snprintf(out, n, "System default");
    else if (i > 0 && i < device_count()) std::snprintf(out, n, "%.30s", devices()[static_cast<size_t>(i - 1)].c_str());
    else std::snprintf(out, n, "%.30s", settings().audio.device.c_str());
}

#define INI_ROW(page, label, help, sec, key, def, names, vals, after) \
    Row { page, label, help, sec, key, def, names, vals, static_cast<int>(std::size(vals)), after }
#define FN_ROW(page, label, help, count, get, set, name) \
    Row { page, label, help, nullptr, nullptr, nullptr, nullptr, nullptr, 0, nullptr, count, get, set, name }

const Row kRows[] = {
    // Display
    FN_ROW(kPageDisplay, "Window Mode", "Borderless keeps the desktop resolution. Exclusive takes over the display.", three, mode_get,
           [](int i) { edit([i](play::Settings& s) { s.display.window_mode = kModeKeys[i]; }); },
           [](int i, char* o, size_t n) { std::snprintf(o, n, "%s", i >= 0 && i < 3 ? kModeNames[i] : "Other"); }),
    FN_ROW(kPageDisplay, "Monitor", "The display the window moves to. The fullscreen modes use it.", monitor_count, monitor_get,
           [](int i) { edit([i](play::Settings& s) { s.display.monitor = i; }); }, monitor_name),
    FN_ROW(kPageDisplay, "Window Size", "The window's size in windowed mode.", [] { return 5; }, size_get,
           [](int i) { edit([i](play::Settings& s) { s.display.width = kSizes[i][0], s.display.height = kSizes[i][1]; }); }, size_name),
    FN_ROW(kPageDisplay, "Keep 16:9", "On letterboxes the picture. Off stretches it to the window.", two,
           [] { return settings().display.keep_aspect ? 1 : 0; }, [](int i) { edit([i](play::Settings& s) { s.display.keep_aspect = i == 1; }); }, onoff),
    FN_ROW(kPageDisplay, "Filter", "How the game's output is scaled to the window. FSR 1 and NIS sharpen as they enlarge.", three, filter_get,
           [](int i) { edit([i](play::Settings& s) { s.extras.upscale.mode = kFilterModes[i]; }); },
           [](int i, char* o, size_t n) { std::snprintf(o, n, "%s", i >= 0 && i < 3 ? kFilterNames[i] : "Other"); }),
    // Graphics
    FN_ROW(kPageGraphics, "Graphics Preset", "Sets Render Scale, Shadow Resolution and Quality together. Medium is the console's picture. Applies at once.",
           preset_count, preset_get, preset_set, preset_name),
    FN_ROW(kPageGraphics, "Output Size", "What the game draws at, not the window. Ultrawide monitor: pick 21:9, 32:9 or Monitor. Applies on restart.",
           output_count, output_get, output_set, output_name),
    INI_ROW(kPageGraphics, "Ultrawide", "Auto widens the 3D view for a 21:9 or 32:9 Output Size; the UI stays centred. Off stretches. Applies on restart.", "fefw",
            "ultrawide", "auto", kWideNames, kWideVals, nullptr),
    INI_ROW(kPageGraphics, "Render Scale", "The 3D render size per side as a share of the output. Applies at once, with a short pause.", "fefw", "render_ratio", "0",
            kRatioNames, kRatioVals, scale_after),
    INI_ROW(kPageGraphics, "Dynamic Resolution", "Lowers the 3D render size in 10% steps under the target frame rate. Needs an Output Size.", "nvn", "dynres", "0",
            kOffOnNames, kOffOnVals, dynres_switch_after),
    INI_ROW(kPageGraphics, "Dynamic Target", "The frame rate dynamic resolution works towards. Set it at or under the frame limit.", "nvn", "dynres_target", "0",
            kTargetNames, kTargetVals, dynres_after),
    INI_ROW(kPageGraphics, "Dynamic Lowest Scale", "Dynamic resolution never goes under this share of the render size.", "nvn", "dynres_min", "0.7",
            kMinNames, kMinVals, dynres_after),
    INI_ROW(kPageGraphics, "Dynamic Least Gain", "What a step down has to add to the frame rate to be kept. Off keeps every step.", "nvn", "dynres_gain", "3",
            kGainNames, kGainVals, dynres_after),
    INI_ROW(kPageGraphics, "Shadow Resolution", "The shadow map's base size. Larger is sharper and costs GPU time. Applies with two short pauses.", "fefw", "shadow_size", "0",
            kShadowNames, kShadowVals, rebuild_after),
    INI_ROW(kPageGraphics, "Quality", "Docked: the docked shadows and effects always. Handheld: the lighter handheld ones, for frame rate. Applies with two short pauses.", "fefw", "quality", "console",
            kQualityNames, kQualityVals, rebuild_after),
    INI_ROW(kPageGraphics, "Reflections", "Reflections on floors and water. Reduced takes 8 rays instead of 32 and helps in the hubs. Applies on restart.", "fefw",
            "reflections", "full", kReflNames, kReflVals, nullptr),
    INI_ROW(kPageGraphics, "Ambient Occlusion", "The soft shading in corners. Off saves GPU time everywhere, with flatter lighting. Applies on restart.", "fefw",
            "ambient_occlusion", "1", kOnOffAoNames, kOnOffAoVals, nullptr),
    INI_ROW(kPageGraphics, "Crowd Detail", "Reduced: crowds switch to simpler models twice as soon, for busy towns and battles. Applies on restart.", "fefw",
            "crowd_detail", "full", kDetailNames, kDetailVals, nullptr),
    INI_ROW(kPageGraphics, "Scenery Detail", "Reduced: scenery switches to simpler versions at 60% of the distance. Applies on restart.", "fefw",
            "scenery_detail", "full", kDetailNames, kDetailVals, nullptr),
    INI_ROW(kPageGraphics, "NPC Limit", "Townspeople a town shows. Fewer saves CPU and GPU time; more stops pop-in but costs both. Console is the game's own. Applies on restart.",
            "fefw", "npc_distance", "1", kNpcNames, kNpcVals, nullptr),
    INI_ROW(kPageGraphics, "Upscaler", "Applies at once. DLSS and XeSS only when the game started with them; otherwise TAAU until the next start.", "dlss",
            "upscaler", "taau", kUpscalerNames, kUpscalerVals, upscaler_after),
    INI_ROW(kPageGraphics, "DLSS Model", "K, J, L and M are the transformer models. E and F are older and lighter. Used with DLSS.", "dlss", "preset", "default",
            kPresetNames, kPresetVals, preset_after),
    INI_ROW(kPageGraphics, "Scene Shading Rate", "Shades 3D surfaces once per 2 or 4 pixels. Edges and the UI stay sharp.", "nvn", "shading_rate", "off",
            kRateNames, kRateVals, rate_after),
    INI_ROW(kPageGraphics, "FSR 3 Sharpening", "Used with the FSR 3 upscaler. Applies at once.", "dlss", "sharpness", "0", kSharpNames, kSharpVals, tuning_after),
    INI_ROW(kPageGraphics, "TAAU History", "Used with TAAU. Higher is smoother and slower to follow change. Applies at once.", "dlss", "history_weight", "12",
            kHistoryNames, kHistoryVals, tuning_after),
    FN_ROW(kPageGraphics, "Frame Generation", "One generated frame per rendered frame. Keep vsync and low latency off. Applies on restart.", three, fg_get,
           [](int i) { ini_write("dlss", "frame_gen", kFgVals[i]); }, fg_name),
    // Frame rate
    FN_ROW(kPageFrameRate, "Gameplay Frame Rate", "30 is the game's own. 120 needs a frame limit of 120 or more. Applies at once.", three, rate_get, rate_set,
           rate_name),
    INI_ROW(kPageFrameRate, "Smooth Time Step", "Keeps the game clock exact when frames are uneven. Auto is on above 30 fps. Applies at once.", "fefw", "smooth_step", "auto",
            kSmoothNames, kSmoothVals, smooth_after),
    INI_ROW(kPageFrameRate, "Frame-Rate Fixes", "Keeps physics, capes, cutscene shakes, map markers and dialogue typing at their real speed at 60 and 120 fps. Applies at once.",
            "fefw", "frame_fixes", "1", kOffOnNames, kOffOnVals, frame_fixes_after),
    FN_ROW(kPageFrameRate, "Vsync", "Off works with G-Sync and FreeSync and the frame limit. On lets the display pace.", two,
           [] { return settings().display.vsync ? 1 : 0; }, [](int i) { edit([i](play::Settings& s) { s.display.vsync = i == 1; }); }, onoff),
    FN_ROW(kPageFrameRate, "Frame Limit", "Refresh rate follows the display. The gameplay frame rate still holds under it.", cap_count, cap_get,
           [](int i) { edit([i](play::Settings& s) { s.display.fps_cap = kFpsChoices[i]; }); }, cap_name),
    FN_ROW(kPageFrameRate, "Low Latency", "NVIDIA Reflex or AMD Anti-Lag 2. Keep it off with frame generation. Applies on restart.", four,
           [] { return std::clamp(static_cast<int>(settings().extras.latency.mode), 0, 3); },
           [](int i) { edit([i](play::Settings& s) { s.extras.latency.mode = static_cast<extras::LatencyMode>(i); }); },
           [](int i, char* o, size_t n) { std::snprintf(o, n, "%s", kLatencyNames[std::clamp(i, 0, 3)]); }),
    // Audio
    FN_ROW(kPageAudio, "Master Volume", "Everything the game plays. Above 100% is a boost with a limiter.", volume_count, volume_get, volume_set, volume_name),
    FN_ROW(kPageAudio, "Mute in Background", "Silent while the window is not in focus.", two, [] { return settings().audio.mute_unfocused ? 1 : 0; },
           [](int i) { edit([i](play::Settings& s) { s.audio.mute_unfocused = i == 1; }); }, onoff),
    FN_ROW(kPageAudio, "Output Device", "Where the sound plays. A change opens the new device within a second.", device_count, device_get,
           [](int i) { edit([i](play::Settings& s) { s.audio.device = i == 0 ? std::string("default") : devices()[static_cast<size_t>(i - 1)]; }); },
           device_name),
    FN_ROW(kPageAudio, "Speakers", "Stereo, or 5.1 surround for a receiver or surround headset. Applies on restart.", two,
           [] { return settings().audio.channels == 6 ? 1 : 0; }, [](int i) { edit([i](play::Settings& s) { s.audio.channels = i == 1 ? 6 : 2; }); },
           [](int i, char* o, size_t n) { std::snprintf(o, n, "%s", i == 1 ? "5.1 Surround" : "Stereo"); }),
};
constexpr int kRowCount = static_cast<int>(std::size(kRows));

int row_count(const Row& r) { return r.sec ? r.n : r.count(); }
int row_get(const Row& r) {
    if (!r.sec) return r.get();
    const std::string now = norm(ini(r.sec, r.key, r.def));
    for (int i = 0; i < r.n; ++i)
        if (norm(r.vals[i]) == now) return i;
    return -1;
}
void row_set(const Row& r, int i) {
    if (!r.sec) r.set(i);
    else ini_write(r.sec, r.key, r.vals[i]);
    if (r.after) r.after();
}
void row_name(const Row& r, int i, char* out, size_t n) {
    if (!r.sec) r.name(i, out, n);
    else if (i >= 0 && i < r.n) std::snprintf(out, n, "%s", r.names[i]);
    else std::snprintf(out, n, "%.24s", ini(r.sec, r.key, r.def).c_str());
}
// One step along the row's values; false at an end without wrap (and for a row with one value).
bool row_step(const Row& r, int dir, bool wrap) {
    const int n = row_count(r), cur = row_get(r);
    if (n < 1) return false;
    int next = cur < 0 ? (dir > 0 ? 0 : n - 1) : cur + dir;
    if (next < 0 || next >= n) {
        if (!wrap) return false;
        next = next < 0 ? n - 1 : 0;
    }
    if (next == cur) return false;
    row_set(r, next);
    return true;
}

// ---- the game side -----------------------------------------------------------------------------------------------------------------------
// The fake table entries the items point at: the game only reads the first byte (the tab's page) through our vtables; the rest is shaped as
// its own entries are (type 1 = choice row, empty message ids, an empty list of value names).
struct Entry {
    uint8_t page, type;
    uint8_t pad[6];
    uint64_t label_mid, help_mid;
    uint64_t names[3];
    uint32_t setting;
    uint32_t pad2;
};
static_assert(sizeof(Entry) == 56);
Entry g_entries[kRowCount] = {};
char g_value[kRowCount][64] = {};
Entry g_list_entry[kListPages] = {}; // one per tab: a row's entry only tells its page
char g_list_label[64] = {}, g_list_value[64] = {}, g_list_help[160] = {};

// The pick list's row behind an item, -1 when it is not one.
int list_row(uint64_t item) {
    uint64_t vt = 0;
    uint32_t i = 0;
    if (!rd(item, &vt) || vt != ptr(&g_ivt2[2]) || !rd(item + kItemSetting, &i) || static_cast<int>(i) >= g_list.count) return -1;
    return static_cast<int>(i);
}
bool is_list_menu(uint64_t menu) {
    uint64_t vt = 0;
    return rd(menu, &vt) && vt == ptr(&g_mvt2[2]);
}

const Row* row_of(uint64_t item) {
    uint64_t vt = 0;
    uint32_t i = 0;
    if (!rd(item, &vt) || vt != ptr(&g_ivt[2]) || !rd(item + kItemSetting, &i) || i >= static_cast<uint32_t>(kRowCount)) return nullptr;
    return &kRows[i];
}
// The weak layout part at `at` (pointer, control block) when it is still alive: 0 otherwise.
uint64_t part_alive(uint64_t at) {
    uint64_t p = 0, ctl = 0;
    int64_t owners = -1;
    if (!rd(at, &p) || !rd(at + 8, &ctl) || !p || !ctl || !rd(ctl + 8, &owners) || owners < 0) return 0;
    return p;
}

void item_text(::Ctx* c) {
    const Row* r = row_of(c->x[0]);
    c->x[0] = ptr(r ? r->label : kEmpty);
}
// slot 70: the value's name into the row's "T_Txt"
void item_value(::Ctx* c) {
    const uint64_t item = c->x[0];
    const Row* r = row_of(item);
    const uint64_t base = part_alive(item + kItemBase);
    if (!r || !base) return;
    char* text = g_value[r - kRows];
    row_name(*r, row_get(*r), text, sizeof g_value[0]);
    call(c, g_base + kSetText, {base, g_base + kStrTxt, ptr(text)});
}
// The options menu the cursor was last seen in (menu_help), for the rows that change other rows' values.
uint64_t g_opt_menu = 0;
// Graphics Preset writes three other rows' keys, and those three decide what the preset row shows: after a step on any of them every row of
// the menu shows its value again. Only when `item` is one of the menu's items, so the menu is the one open now.
bool links_rows(const Row* r) {
    if (!r) return false;
    if (r->set == preset_set) return true;
    return r->sec && !std::strcmp(r->sec, "fefw") &&
           (!std::strcmp(r->key, "render_ratio") || !std::strcmp(r->key, "shadow_size") || !std::strcmp(r->key, "quality"));
}
void redraw_all(::Ctx* c, uint64_t item) {
    uint64_t vt = 0, b = 0, e = 0;
    const uint64_t menu = g_opt_menu;
    if (!menu || !rd(menu, &vt) || vt != ptr(&g_mvt[2]) || !rd(menu + kMenuItems, &b) || !rd(menu + kMenuItems + 8, &e) || e <= b || (e - b) % 8 ||
        e - b > 8 * 4096 || !mem::is_mapped(b, e - b))
        return;
    bool mine = false;
    for (uint64_t p = b; p < e && !mine; p += 8) mine = *reinterpret_cast<const uint64_t*>(p) == item;
    if (!mine) return;
    for (uint64_t p = b; p < e; p += 8) {
        uint64_t row = 0;
        if (rd(p, &row) && row != item && row_of(row) && part_alive(row + kItemBase)) call(c, g_ivt[2 + kISlotRedraw], {row});
    }
}
// The rows' update (slot 18, every frame for a row on screen; the game's own, then ours). After a step on Graphics Preset, Render Scale, Shadow
// Resolution or Quality every row draws its value again a few times over 0.6 s: those steps rebuild the render targets (twice: the second
// rebuild a few frames later), and on a PC the preset row kept showing its old name though the step had happened and been drawn.
uint64_t g_update_orig = 0;              // the game's slot 18 (also the pick list's, list_update)
std::chrono::steady_clock::time_point g_rows_until, g_rows_gen_at;
uint32_t g_rows_gen = 0, g_row_seen[kRowCount] = {};
void rows_redraw_soon() { g_rows_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(600); }
void item_update(::Ctx* c) {
    const uint64_t item = c->x[0];
    pass(c, g_update_orig);
    const Row* r = row_of(item);
    const auto now = std::chrono::steady_clock::now();
    if (!r || now >= g_rows_until) return;
    if (now - g_rows_gen_at > std::chrono::milliseconds(100)) {
        g_rows_gen_at = now;
        ++g_rows_gen;
    }
    uint32_t& seen = g_row_seen[r - kRows];
    if (seen == g_rows_gen) return;
    seen = g_rows_gen;
    if (part_alive(item + kItemBase)) call(c, g_ivt[2 + kISlotRedraw], {item});
}

// slots 39 / 40: (item, wrap) -> changed, as the game's: the value, a redraw, the arrow's animation
void item_step(::Ctx* c, int dir) {
    const uint64_t item = c->x[0];
    const bool wrap = c->x[1] & 1;
    const Row* r = row_of(item);
    if (!r || !(call(c, g_ivt[2 + kISlotReady], {item}) & 1) || !row_step(*r, dir, wrap)) {
        c->x[0] = 0;
        return;
    }
    call(c, g_ivt[2 + kISlotRedraw], {item});
    if (links_rows(r)) {
        redraw_all(c, item);
        rows_redraw_soon();   // and again over the next frames: a step that rebuilds the render targets lost the value drawn now
    }
    if (const uint64_t base = part_alive(item + kItemBase)) call(c, g_base + kArrow, {base, dir < 0 ? 1ull : 0ull});
    c->x[0] = 1;
}
void item_next(::Ctx* c) { item_step(c, 1); }
void item_prev(::Ctx* c) { item_step(c, -1); }

// The pick list's rows: a label, a value that only describes, and A picks the row and closes the list.
void list_text(::Ctx* c) {
    const int i = list_row(c->x[0]);
    g_list_label[0] = 0;
    if (i >= 0 && g_list.label) g_list.label(i, g_list_label, sizeof g_list_label);
    c->x[0] = ptr(g_list_label);
}
void list_value(::Ctx* c) {
    const uint64_t item = c->x[0];
    const int i = list_row(item);
    const uint64_t base = part_alive(item + kItemBase);
    if (i < 0 || !base) return;
    g_list_value[0] = 0;
    if (g_list.value) g_list.value(i, g_list_value, sizeof g_list_value);
    call(c, g_base + kSetText, {base, g_base + kStrTxt, ptr(g_list_value)});
}
// Left / right on a row (slots 40 / 39, as the Options rows' "previous / next value"): the owner changes the row's value and says so; the
// value is drawn again and the row's arrow moves, as on an Options row. Without an owner's function: no value, no change.
void list_side(::Ctx* c, int dir) {
    const uint64_t item = c->x[0];
    const int i = list_row(item);
    if (i < 0 || !g_list.side || !(call(c, g_ivt2[2 + kISlotReady], {item}) & 1) || !g_list.side(c, i, dir)) {
        c->x[0] = 0;
        return;
    }
    call(c, g_ivt2[2 + kISlotRedraw], {item});
    if (const uint64_t base = part_alive(item + kItemBase)) call(c, g_base + kArrow, {base, dir < 0 ? 1ull : 0ull});
    c->x[0] = 1;
}
void list_next(::Ctx* c) { list_side(c, 1); }
void list_prev(::Ctx* c) { list_side(c, -1); }
// A prompt the row's pick opened (answer 12) is a process made after the list, under the same parent: while one is there, the list still
// gets the buttons the prompt gets (B on the prompt also closed the list, seen on a PC). Processes: +0x20 = parent, +0x28 = newest child,
// +0x38 = the sibling made before this one. So from a pick that answered 12 the list ignores A and B until the prompt was seen and is gone
// again (or none came within two seconds); the rows' update keeps watch every frame.
// The prompt is the newest process over the list when one is first seen, and "gone" means that process left its parent's children. Waiting
// for nothing at all to be over the list is not enough: the game's Save entry makes a helper process next to its prompt (0x16da940), and
// when the prompt is cancelled that helper can stay, which left the list deaf to A and B until it was opened again (seen on a PC).
// After the prompt's process is gone the guard can still be needed: the game's report window (game::menu::ReportDialog, the window of the
// Save and Continue prompts) can stay up a little longer, and after a save a last step is closed with A. That A also reached the row, which
// asked again 0.86 s after the save (seen on a PC). So while a report window is still registered the guard stays on (kPromptWindow).
// The game keeps one global pointer to "the" report window, [main+0x22a10c0]: a new window is stored there only when it is empty
// (0x16da9f4 and 0x16daf54: adrp x8, the global's page; ldr x9, [x8,#0xc0]; cbnz x9; str ..., [x8,#0xc0]) and its destructor clears it. A prompt made while the old
// window is still there is not registered, so its Yes / No / B close the old one and nothing ever closes the new one: it stayed on screen
// and the field camera no longer turned (seen on a PC). 0x1618dc0(window) closes a window once it has finished opening (what the
// Continue prompt's Yes and No call); it does nothing before that.
// A step the game shows after the prompt's Yes (the save's "saved" step, closed with A) can be a process of its own, made after the prompt:
// with the report window already gone the guard lifted and the A that closed that step reached the row, which asked again 0.65 s after the
// save (seen on a PC with the window check working). So the processes over the list when the prompt is first seen are noted, and once the
// prompt is gone the guard stays on while a process made after that is still over the list (kPromptAfter), for at most kAfterWait. The
// Save entry's helper process is made before its prompt, so a helper left after a cancel is in the note and does not hold the list.
enum { kPromptNone, kPromptWaiting, kPromptSeen, kPromptWindow, kPromptAfter };
constexpr auto kAfterWait = std::chrono::seconds(4);
uint64_t g_prompt_snap[16] = {};   // the processes over the list when the prompt was first seen
int g_prompt_snap_n = 0;
int g_guard_logs = 0;              // the guard's per-pick lines: the first 32 of a run
int g_prompt = kPromptNone, g_prompt_ignored = 0;
constexpr uint64_t kReportWindow = 0x22a10c0, kReportClose = 0x1618dc0;
constexpr uint64_t kReportStores[] = {0x16da9f4, 0x16daf54};
constexpr auto kWindowWait = std::chrono::seconds(5);      // then the window is closed by hand ...
constexpr auto kWindowGiveUp = std::chrono::seconds(8);    // ... and past this the guard lifts whatever is left
constexpr auto kWindowRefuse = std::chrono::seconds(10);   // A on a row is refused while a window is up, for at most this long
bool g_report_ok = false;          // the global is where it was expected (optmenu_install)
uint64_t g_report_at_open = 0;     // the window there when the list was opened: not one of the list's prompts
uint64_t g_report_seen = 0;        // list_acall: the window A was last refused for, and since when
std::chrono::steady_clock::time_point g_report_seen_at;
// The report window that is up and is not the one there when the list was opened; 0 if none (or if the global could not be checked).
uint64_t report_window() {
    uint64_t w = 0;
    if (!g_report_ok || !rd(g_base + kReportWindow, &w) || w == g_report_at_open) return 0;
    return w;
}
uint64_t g_prompt_proc = 0;      // the prompt's process (kPromptSeen)
uint64_t g_list_menu = 0;        // the pick list's menu (build_ours)
// A menu whose item answered 12 notes it (menu +0x160, the handler 0x17141e0) and reads no directions while that is set (its tick,
// 0x1713fb8). The game clears it in the menu's resume (process slot 9, 0x1715cc0), which runs when a process made under the menu ends.
// The prompts over the list are made under the + menu's root instead, so the list was never resumed: after a prompt the cursor no longer
// moved (seen on a PC). The list is resumed here once its prompt is gone.
constexpr uint64_t kMenuState = 0x160, kMenuHandle = 0x17141e0;
// The game's own resume is called (it also puts the picked row back to its plain look: with the state alone cleared the row stayed
// highlighted, seen on a PC); the state is cleared here too in case the resume declined.
constexpr int kMSlotResume = 9;
void list_resume(::Ctx* c, uint64_t menu) {
    uint32_t state = 0;
    if (!rd(menu + kMenuState, &state) || state != 12) return;
    if (fn_lookup(g_mvt2[2 + kMSlotResume])) call(c, g_mvt2[2 + kMSlotResume], {menu});
    if (rd(menu + kMenuState, &state) && state == 12) wr<uint32_t>(menu + kMenuState, 0);
    static bool told = false;
    if (!told) {
        told = true;
        NXLOG_I(kTag, "pick list: the prompt over the list is gone: the list reads the directions again");
    }
}
std::chrono::steady_clock::time_point g_prompt_at;
// The processes over the list: the menu's own newest child, else its parent's children made after it, newest first. Returns how many.
int procs_over(uint64_t menu, uint64_t* out, int cap) {
    uint64_t child = 0, parent = 0, p = 0;
    int n = 0;
    if (rd(menu + 0x28, &child) && child) {
        out[n++] = child;
        return n;
    }
    if (!rd(menu + 0x20, &parent) || !parent || !rd(parent + 0x28, &p)) return 0;
    while (p && p != menu && n < cap) {
        out[n++] = p;
        if (!rd(p + 0x38, &p)) break;
    }
    return n;
}
// The prompt is gone: the list takes its buttons again, unless a report window is still up (kPromptWindow, timed from now).
bool prompt_ended(::Ctx* c, uint64_t menu) {
    if (report_window()) {
        g_prompt = kPromptWindow;
        g_prompt_at = std::chrono::steady_clock::now();
        static bool told = false;
        if (!told) {
            told = true;
            NXLOG_I(kTag, "pick list: the prompt is gone but its report window is still up: the list waits for it");
        }
        return false;
    }
    g_prompt = kPromptNone;
    list_resume(c, menu);
    return true;
}
bool under_prompt(::Ctx* c, uint64_t item) {
    if (g_prompt == kPromptNone) return false;
    uint64_t menu = 0, over[16] = {};
    if (!rd(item + 0x8, &menu) || !menu) return false;
    const int n = procs_over(menu, over, 16);
    if (g_prompt == kPromptWaiting) {
        if (n > 0) {
            g_prompt = kPromptSeen;
            g_prompt_proc = over[0];
            g_prompt_snap_n = n;
            std::copy(over, over + n, g_prompt_snap);
        } else if (std::chrono::steady_clock::now() - g_prompt_at > std::chrono::seconds(2)) {
            prompt_ended(c, menu);
        }
    } else if (g_prompt == kPromptWindow) {
        const uint64_t w = report_window();
        const auto waited = std::chrono::steady_clock::now() - g_prompt_at;
        if (!w) {
            g_prompt = kPromptNone;
            list_resume(c, menu);
        } else if (waited > kWindowGiveUp) {
            g_prompt = kPromptNone;
            list_resume(c, menu);
            NXLOG_W(kTag, "pick list: a report window (0x%llx) stayed up %lld s after its prompt and did not close: the list takes A and B again",
                    static_cast<unsigned long long>(w), static_cast<long long>(kWindowGiveUp.count()));
        } else if (waited > kWindowWait) {
            // Its prompt is answered and gone, so nothing else will close it. The close does nothing until the window has finished
            // opening, and nothing while it is already closing, so it is asked every frame until the window is gone.
            static bool told = false;
            if (!told) {
                told = true;
                NXLOG_I(kTag, "pick list: a report window is still up %lld s after its prompt: closing it",
                        static_cast<long long>(kWindowWait.count()));
            }
            call(c, g_base + kReportClose, {w});
        }
    } else if (g_prompt == kPromptAfter) {
        int left = 0;
        for (int k = 0; k < n; ++k) left += std::find(g_prompt_snap, g_prompt_snap + g_prompt_snap_n, over[k]) == g_prompt_snap + g_prompt_snap_n;
        const auto waited = std::chrono::steady_clock::now() - g_prompt_at;
        if (!left || waited > kAfterWait) {
            if (g_guard_logs < 32) {
                ++g_guard_logs;
                NXLOG_I(kTag, "pick list: the step(s) after the prompt %s after %lld ms", left ? "are still up, the guard lifts" : "are gone",
                        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(waited).count()));
            }
            prompt_ended(c, menu);
        }
    } else if (std::find(over, over + n, g_prompt_proc) == over + n) {
        int fresh = 0;
        for (int k = 0; k < n; ++k) fresh += std::find(g_prompt_snap, g_prompt_snap + g_prompt_snap_n, over[k]) == g_prompt_snap + g_prompt_snap_n;
        if (g_guard_logs < 32) {
            ++g_guard_logs;
            NXLOG_I(kTag, "pick list: the prompt is gone; over the list: %d process(es), %d made after the prompt; report window %s", n, fresh,
                    report_window() ? "up" : "none");
        }
        if (fresh) {
            g_prompt = kPromptAfter;
            g_prompt_at = std::chrono::steady_clock::now();
        } else if (prompt_ended(c, menu) && n > 0) {
            static bool told = false;
            if (!told) {
                told = true;
                NXLOG_I(kTag, "pick list: the prompt is gone and %d other process(es) stay over the list: the list takes A and B again", n);
            }
        }
    }
    if (g_prompt == kPromptNone) g_prompt_ignored = 0, g_prompt_proc = 0, g_prompt_snap_n = 0;
    return g_prompt != kPromptNone;
}
void list_acall(::Ctx* c) {
    const uint64_t item = c->x[0];
    const int i = list_row(item);
    if (i < 0 || !(call(c, g_ivt2[2 + kISlotReady], {item}) & 1)) {
        c->x[0] = 1;
        return;
    }
    if (under_prompt(c, item)) {      // the A that answered a prompt over the list
        c->x[0] = 0;
        return;
    }
    // A report window still up (the guard gave up on it, or it came from elsewhere): a prompt made now would not be registered and would
    // stay on screen, so the row is refused (the game's "no" sound) until it is gone, for at most kWindowRefuse per window.
    if (const uint64_t w = report_window()) {
        const auto now = std::chrono::steady_clock::now();
        if (w != g_report_seen) g_report_seen = w, g_report_seen_at = now;
        if (now - g_report_seen_at < kWindowRefuse) {
            static bool told = false;
            if (!told) {
                told = true;
                NXLOG_I(kTag, "pick list: A on a row while a report window is still up: refused until it is gone");
            }
            c->x[0] = 1;
            return;
        }
    }
    // the answer is the row's: 3 closes the list, 13 keeps it open and redraws the row's value, anything else goes to the menu as it is
    const uint32_t answer = g_list.pick ? g_list.pick(c, i) : kClose;
    if (answer == 12) {
        g_prompt = kPromptWaiting;
        g_prompt_ignored = 0;
        g_prompt_at = std::chrono::steady_clock::now();
    }
    if (answer == 13) call(c, g_ivt2[2 + kISlotRedraw], {item});
    c->x[0] = answer;
}

// B on a row: the list's owner hears of it, then the game's own slot (it answers "close"). Under a prompt it is the prompt's B: the owner
// hears of it too (what the row armed is forgotten) and the list stays.
void list_bcall(::Ctx* c) {
    const uint64_t item = c->x[0];
    if (list_row(item) >= 0 && g_list.back) g_list.back();
    if (list_row(item) >= 0 && under_prompt(c, item)) {
        static bool told = false;
        if (!told) {
            told = true;
            NXLOG_I(kTag, "pick list: B reached the list under a prompt: ignored, the list stays");
        }
        if (++g_prompt_ignored < 4) {      // a way out should something else stay over the list
            c->x[0] = 0;
            return;
        }
        g_prompt = kPromptNone;
        g_prompt_ignored = 0;
    }
    pass(c, g_ivt[2 + kISlotBCall]);
}
// The row's update (slot 18): while the owner says its values may have changed, they are drawn again a few times a second.
uint32_t g_list_gen = 0, g_list_seen[kListMax] = {};
std::chrono::steady_clock::time_point g_list_gen_at;
void list_update(::Ctx* c) {
    const uint64_t item = c->x[0];
    pass(c, g_update_orig);
    const int i = list_row(item);
    if (i >= 0) under_prompt(c, item);
    if (i < 0 || i >= kListMax || !g_list.dirty) return;
    static bool told = false;
    if (!told) {
        told = true;
        NXLOG_I(kTag, "pick list rows are updated by the game each frame: their values can follow a save");
    }
    const auto now = std::chrono::steady_clock::now();
    if (g_list.dirty() && now - g_list_gen_at > std::chrono::milliseconds(300)) {
        g_list_gen_at = now;
        ++g_list_gen;
    }
    if (g_list_seen[i] == g_list_gen) return;
    g_list_seen[i] = g_list_gen;
    if (part_alive(item + kItemBase)) call(c, g_ivt2[2 + kISlotRedraw], {item});
}

// The tab bar's label function: (this, int* page) -> std::string in x8 (libc++, short form: every name is under 23 bytes).
void tab_label(::Ctx* c) {
    int32_t page = 0;
    const uint64_t out = c->x[8];
    rd(c->x[1], &page);
    const char* name = c->x[0] == ptr(g_list.fn) ? (page >= 0 && page < kListPages && g_list.titles[page] ? g_list.titles[page] : kEmpty) : page >= 0 && page < kPages ? kTabNames[page] : kEmpty;
    const size_t len = std::min<size_t>(std::strlen(name), 22);
    uint8_t s[24] = {};
    s[0] = static_cast<uint8_t>(len * 2);
    std::memcpy(s + 1, name, len);
    if (mem::is_mapped(out, sizeof s)) std::memcpy(reinterpret_cast<void*>(out), s, sizeof s);
}

// [fefw] help_unblock (default on; issue #187). The X details box (FloatingHelp, [main+0x2231f30]) opens only while the counter at
// +0xfc is 0 (0x18e44c0). The game's menu OnInit (slot 84, 0x16a4620, copied by menu_init) adds 1 and its close (slot 94, 0x16a4790)
// takes it back. A Load from the pick list ends the game with the list still open, and its close never runs (seen on a PC: the
// list took the block 0 -> 1 and neither its close nor its end ran at the load), so the block stayed 1
// and X did nothing until the game was closed. We remember the count each of our menus holds and give it back when a Load from the
// list goes ahead (optmenu_release_help). If the game does close such a menu after that, its own take-back is undone so it is not
// taken twice.
struct HelpHold { uint64_t menu; int32_t n; };
std::vector<HelpHold> g_help_holds;
std::vector<uint64_t> g_help_released;   // menus whose count we gave back ourselves
uint64_t g_menu_close_orig = 0;          // the game's slot 94
constexpr int kMSlotClose = 94;
bool help_fix_on() {
    static const bool on = config_get_bool("fefw", "help_unblock", true);
    return on && g_menu_close_orig;
}
void help_hold(uint64_t menu) {
    std::erase(g_help_released, menu);   // a new menu at the address of an old one
    for (auto& h : g_help_holds)
        if (h.menu == menu) { ++h.n; return; }
    g_help_holds.push_back({menu, 1});
}
int32_t help_block() {
    uint64_t fh = 0;
    int32_t count = 0;
    return rd(g_base + kUiCount, &fh) && fh && rd(fh + 0xfc, &count) ? count : -99;
}
// slot 94: the game's close gives the count back itself
void menu_close(::Ctx* c) {
    const uint64_t menu = c->x[0];
    std::erase_if(g_help_holds, [menu](const HelpHold& h) { return h.menu == menu; });
    const bool released = std::erase(g_help_released, menu) > 0;
    const int32_t before = help_block();
    pass(c, g_menu_close_orig);
    const int32_t after = help_block();
    uint64_t fh = 0;
    if (released && after < before && rd(g_base + kUiCount, &fh) && fh) wr<int32_t>(fh + 0xfc, before);
}

// slot 84, the game's OnInit with our label function (its own reads a table of three message ids)
void menu_init(::Ctx* c) {
    const uint64_t menu = c->x[0];
    uint64_t weak[2] = {0, 0};
    // the label function: an object whose first word is its vtable; the list's lives in g_list, so tab_label can tell the two apart
    uint64_t fn_local[5] = {ptr(&g_fvt[2]), 0, 0, 0, 0};
    uint64_t* fn = fn_local;
    if (is_list_menu(menu)) {
        g_list.fn[0] = ptr(&g_fvt[2]);
        fn = g_list.fn;
    }
    fn[4] = ptr(fn);
    call(c, g_base + kGetPart, {menu + kMenuLayout, g_base + kStrPageTab}, ptr(weak));
    const uint64_t layer = call(c, g_mvt[2 + kMSlotLayer], {menu}) & 0xffffffffull;
    call(c, g_base + kSetupText, {menu + kMenuTab, ptr(weak), ptr(fn), layer, 4, 5});
    if (weak[1]) call(c, g_base + kReleaseWeak, {weak[1]});
    uint64_t info = 0, counter = 0;
    if (rd(g_base + kMapInfo, &info) && info) call(c, g_base + kMapNotify, {info, 15});
    int32_t count = 0;
    if (rd(g_base + kUiCount, &counter) && counter && rd(counter + 0xfc, &count)) {
        wr<int32_t>(counter + 0xfc, std::max(count, -1) + 1);
        if (help_fix_on()) help_hold(menu);
    }
}
// slot 88: the current row's help line
void menu_help(::Ctx* c) {
    const uint64_t menu = c->x[0];
    uint64_t cur = 0;
    rd(menu + kMenuCurrent, &cur);
    const Row* r = cur ? row_of(cur) : nullptr;
    const int li = cur ? list_row(cur) : -1;
    if (!r && li < 0) return;
    if (r) g_opt_menu = menu;
    const char* text = r && r->help ? r->help : kEmpty;
    if (li >= 0) {
        g_list_help[0] = 0;
        if (g_list.help) g_list.help(li, g_list_help, sizeof g_list_help);
        text = g_list_help;
    }
    call(c, g_base + kSetText, {menu + kMenuLayout, g_base + kStrInfoTxt, ptr(text)});
    if (const uint64_t align = call(c, g_base + kFindAlign, {menu + kMenuLayout, g_base + kStrInfo})) call(c, g_base + kAlignUpdate, {align});
}
// Which rows the screen shows: the three that belong to one upscaler only when that one (or Auto) is chosen. Read when the screen opens.
bool row_shown(const Row& r) {
    if (!r.sec || std::strcmp(r.sec, "dlss") != 0) return true;
    std::string u = lower(ini("dlss", "upscaler", "taau"));
    if (u == "fsr") u = "fsr3";
    if (u == "auto") return true;
    if (!std::strcmp(r.key, "preset")) return u == "dlss";
    if (!std::strcmp(r.key, "sharpness")) return u == "fsr3";
    if (!std::strcmp(r.key, "history_weight")) return u == "taau";
    return true;
}

// Every row back to its default. The rows over ObeliskChamber.ini keys know theirs; the others are the play settings' own defaults.
void reset_all() {
    int changed = 0;
    for (const Row& r : kRows) {
        if (!r.sec) continue;
        const std::string want = norm(r.def);
        for (int i = 0; i < r.n; ++i)
            if (norm(r.vals[i]) == want && row_get(r) != i) {
                row_set(r, i);      // writes the key and applies it to the running game where the row can
                ++changed;
            }
    }
    if (output_get() != 0) ini_write("fefw", "output", kOutputChoices[0].value), ++changed;
    if (fg_get() != 0) ini_write("dlss", "frame_gen", kFgVals[0]), ++changed;
    if (rate_get() != 0) rate_set(0), ++changed;
    const play::Settings d;
    audio::set_master_volume(static_cast<float>(d.audio.volume));
    edit([&d](play::Settings& s) {
        s.display.window_mode = d.display.window_mode;
        s.display.monitor = d.display.monitor;
        s.display.width = d.display.width, s.display.height = d.display.height;
        s.display.keep_aspect = d.display.keep_aspect;
        s.display.vsync = d.display.vsync;
        s.display.fps_cap = d.display.fps_cap;
        s.extras.upscale.mode = d.extras.upscale.mode;
        s.extras.latency.mode = d.extras.latency.mode;
        s.audio.volume = d.audio.volume;
        s.audio.mute_unfocused = d.audio.mute_unfocused;
        s.audio.device = d.audio.device;
        s.audio.channels = d.audio.channels;
    });
    NXLOG_I(kTag, "Recomp Options reset to the defaults (%d ObeliskChamber.ini rows changed, the display, frame rate and audio settings put back)", changed);
}

// "Yes" on the reset prompt. The prompt's choice (class vtable 0x20b0530) calls it from its A handler (0x167a880) as (this, &item) with
// the function at item + 0x80, and answers what it returns: the game's own returns 2 (decided: the prompt closes). On a PC the first
// version reset at once, before the prompt was answered, and returned nothing in particular. So this only resets when it is called the
// way that A handler calls it, and not within a moment of the prompt opening; anything else is logged and changes nothing.
constexpr uint64_t kVtPromptChoice = 0x20b0530;
std::chrono::steady_clock::time_point g_reset_asked_at;
void reset_yes(::Ctx* c) {
    const uint64_t self = c->x[0];
    uint64_t menu = 0, b = 0, e = 0, item = 0, item_vt = 0, item_fn = 0;
    rd(self + 8, &menu);
    rd(c->x[1], &item);
    if (item) rd(item, &item_vt), rd(item + 0x80, &item_fn);
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_reset_asked_at).count();
    const bool from_choice = item_vt == g_base + kVtPromptChoice && item_fn == self;
    if (!from_choice || ms < 300) {
        NXLOG_W(kTag, "reset: \"yes\" was called %lld ms after the prompt opened, %s (x1 0x%llx, item 0x%llx, its vtable 0x%llx, its function 0x%llx, this 0x%llx, lr 0x%llx): nothing reset",
                ms, from_choice ? "by the prompt's Yes" : "not by the prompt's Yes", static_cast<unsigned long long>(c->x[1]), static_cast<unsigned long long>(item),
                static_cast<unsigned long long>(item_vt), static_cast<unsigned long long>(item_fn), static_cast<unsigned long long>(self),
                static_cast<unsigned long long>(c->x[30]));
        c->x[0] = 0;       // nothing happened: the prompt stays
        return;
    }
    NXLOG_I(kTag, "reset: confirmed on the prompt after %lld ms", ms);
    reset_all();
    if (!menu || !rd(menu + kMenuItems, &b) || !rd(menu + kMenuItems + 8, &e) || e <= b || (e - b) % 8 || e - b > 8 * 4096 || !mem::is_mapped(b, e - b)) {
        c->x[0] = 2;
        return;
    }
    for (uint64_t p = b; p < e; p += 8) {
        uint64_t row = 0;
        if (rd(p, &row) && row_of(row) && part_alive(row + kItemBase)) call(c, g_ivt[2 + kISlotRedraw], {row});
    }
    c->x[0] = 2;
}
// The two clones of "yes": the game's, which write its vtable into the copy, then ours over it.
void reset_yes_clone_new(::Ctx* c) {
    pass(c, g_yes_new);
    if (c->x[0] && mem::is_mapped(c->x[0], 16)) wr<uint64_t>(c->x[0], ptr(&g_yvt[2]));
}
void reset_yes_clone_at(::Ctx* c) {
    const uint64_t to = c->x[1];
    pass(c, g_yes_at);
    if (to && mem::is_mapped(to, 16)) wr<uint64_t>(to, ptr(&g_yvt[2]));
}

// ---- Quit Game (the System menu's entry, ObeliskChamber_sysmenu.cpp) -----------------------------------------------------------------
// The game's own yes / no dialog, opened the way its Return to Title entry opens one (SystemMenuItem_ExitTitle::ACall): the prompt helper
// with a message id, a "yes" and a "no" function, then the dialog's own call that puts the cursor on No, and 12 for the menu. The message is
// ours (kQuitMid, ObeliskChamber_msg.cpp); "yes" is the reset prompt's "yes" with our invoke and clones, "no" the reset prompt's "no" (it
// does nothing: the dialog closes). The cursor call is found in the game's Return to Title ACall: the first direct call after the prompt
// helper's, before its "mov w0, #12".
constexpr uint64_t kVtExitTitle = 0x2129828;
constexpr int kExitACallSlot = 25;
uint64_t g_qvt[2 + 9] = {};              // "yes" of the quit dialog
uint64_t g_dialog_no_first = 0;          // the dialog's call that moves its cursor to No (0: not found, Yes stays first)
bool g_quit_ok = false;
std::chrono::steady_clock::time_point g_quit_asked_at;
void quit_yes(::Ctx* c) {
    const uint64_t self = c->x[0];
    uint64_t item = 0, item_vt = 0, item_fn = 0;
    rd(c->x[1], &item);
    if (item) rd(item, &item_vt), rd(item + 0x80, &item_fn);
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_quit_asked_at).count();
    if (item_vt != g_base + kVtPromptChoice || item_fn != self || ms < 300) {   // as reset_yes: only the dialog's own Yes ends the game
        NXLOG_W(kTag, "quit: \"yes\" was called %lld ms after the dialog opened, not by its Yes: the game goes on", ms);
        c->x[0] = 0;
        return;
    }
    NXLOG_I(kTag, "quit: Yes on the game's dialog after %lld ms", ms);
    play::exit_process(0, "pass", "quit from the System menu");   // the same exit as the F1 menu's Quit game
}
void quit_yes_clone_new(::Ctx* c) {
    pass(c, g_yes_new);
    if (c->x[0] && mem::is_mapped(c->x[0], 16)) wr<uint64_t>(c->x[0], ptr(&g_qvt[2]));
}
void quit_yes_clone_at(::Ctx* c) {
    const uint64_t to = c->x[1];
    pass(c, g_yes_at);
    if (to && mem::is_mapped(to, 16)) wr<uint64_t>(to, ptr(&g_qvt[2]));
}
// The Return to Title ACall's cursor call: after its BL to the prompt helper, the next BL, with a "mov w0, #12" a few instructions on.
uint64_t find_no_first() {
    uint64_t fn = 0;
    if (!rd(g_base + kVtExitTitle + kExitACallSlot * 8, &fn) || !fn_lookup(fn) || !mem::is_mapped(fn, 400 * 4)) return 0;
    auto bl_target = [](uint64_t at, uint32_t w) -> uint64_t {
        if ((w & 0xfc000000u) != 0x94000000u) return 0;
        int64_t imm = static_cast<int64_t>(w & 0x03ffffffu);
        if (imm & (int64_t{1} << 25)) imm -= int64_t{1} << 26;
        return static_cast<uint64_t>(static_cast<int64_t>(at) + imm * 4);
    };
    bool after_prompt = false;
    std::string seen;   // the direct calls met, for the log when the call is not found
    for (int i = 0; i < 400; ++i) {
        uint32_t w = 0;
        rd(fn + i * 4, &w);
        if (w == 0xd65f03c0u && !after_prompt) break;   // ret (after the prompt helper's call an inlined destructor's ret comes first)
        if (after_prompt && w == 0x52800180u) break;      // mov w0, #12 with no call before it
        const uint64_t t = bl_target(fn + i * 4, w);
        if (w == 0x52800180u) seen += " mov-w0-12@" + std::to_string(i);
        if (!t) continue;
        if (seen.size() < 1200) {
            char b[40];
            std::snprintf(b, sizeof b, " bl@%d:0x%llx", i, static_cast<unsigned long long>(t - g_base));
            seen += b;
        }
        if (!after_prompt) {
            after_prompt = t == g_base + kPrompt;
            if (after_prompt) {   // the instructions after the prompt helper's call, for the log when the cursor call is not a BL
                seen += " | after it:";
                for (int k = 1; k <= 40; ++k) {
                    uint32_t v = 0;
                    rd(fn + (i + k) * 4, &v);
                    char b[12];
                    std::snprintf(b, sizeof b, " %08x", v);
                    seen += b;
                }
                seen += " |";
            }
            continue;
        }
        for (int k = 1; k <= 8; ++k) {
            uint32_t v = 0;
            rd(fn + (i + k) * 4, &v);
            if (v == 0x52800180u) return fn_lookup(t) ? t : 0;   // mov w0, #12
        }
        break;
    }
    NXLOG_I(kTag, "quit: Return to Title's ACall 0x%llx (the prompt helper 0x%llx): %s", static_cast<unsigned long long>(fn - g_base),
            static_cast<unsigned long long>(kPrompt), seen.empty() ? "no direct calls" : seen.c_str());
    return 0;
}

// slot 114, minus: the game's "reset the options?" prompt, whose Yes resets ours. Nothing on a pick list.
void menu_minus(::Ctx* c) {
    const uint64_t menu = c->x[0];
    if (is_list_menu(menu) || !g_reset_ok) {
        c->x[0] = 0;
        return;
    }
    static uint64_t ask, yes[6], no[6], none[6];      // static: the prompt copies the functions, the message id is a pointer it may keep
    ask = g_base + kStrResetAsk;
    yes[0] = ptr(&g_yvt[2]), yes[1] = menu, yes[4] = ptr(yes);
    no[0] = g_base + kVtResetNo, no[4] = ptr(no);
    none[4] = 0;
    g_reset_asked_at = std::chrono::steady_clock::now();
    NXLOG_I(kTag, "reset: the prompt is asked for");
    call(c, g_base + kPrompt, {menu, ptr(&ask), ptr(yes), ptr(no), ptr(none)});
    c->x[0] = 12;      // a dialog is up, as the game's own slot answers
}
// slot 142: the game's key help; a pick list (and a screen whose reset prompt is not available) loses its "Reset" entry (a vector of
// 40-byte entries, the message id at +8)
void menu_keyhelp(::Ctx* c) {
    const uint64_t vec = c->x[1];
    const bool keep_reset = !is_list_menu(c->x[0]) && g_reset_ok;
    pass(c, g_base + kKeyHelp);
    const uint64_t ret = c->x[0];
    uint64_t b = 0, e = 0;
    if (!keep_reset && rd(vec, &b) && rd(vec + 8, &e) && e > b && (e - b) % 40 == 0 && mem::is_mapped(b, e - b)) {
        for (uint64_t p = b; p + 40 <= e;) {
            uint64_t mid = 0;
            std::memcpy(&mid, reinterpret_cast<const void*>(p + 8), 8);
            if (mid != g_base + kStrResetMid) {
                p += 40;
                continue;
            }
            std::memmove(reinterpret_cast<void*>(p), reinterpret_cast<const void*>(p + 40), e - p - 40);
            e -= 40;
        }
        wr<uint64_t>(vec + 8, e);
    }
    c->x[0] = ret;
}

// The menu the game's create call just made becomes ours: our vtable, four tabs, our rows.
void build_ours(::Ctx* c, uint64_t menu, int screen) {
    uint64_t heap_var = 0, heap = 0;
    if (!rd(g_base + kHeapGot, &heap_var) || !rd(heap_var, &heap) || !heap) return;
    const bool list = screen == kScreenList;
    const int rows = list ? std::clamp(g_list.count, 0, kListMax) : kRowCount;
    if (list) {
        g_prompt = kPromptNone, g_list_menu = menu;
        g_report_at_open = 0, g_report_seen = 0;
        if (g_report_ok) rd(g_base + kReportWindow, &g_report_at_open);
    }
    if (list) std::fill(std::begin(g_list_seen), std::end(g_list_seen), g_list_gen);
    else
        for (int i = 0; i < kRowCount; ++i) g_entries[i].page = row_shown(kRows[i]) ? static_cast<uint8_t>(kRows[i].page) : 0xff;   // 0xff: on no tab
    wr<uint64_t>(menu, ptr(list ? &g_mvt2[2] : &g_mvt[2]));
    wr<int32_t>(menu + kMenuTab, list ? std::clamp(g_list.pages, 1, kListPages) - 1 : kPages - 1);   // the last tab's index
    wr<int32_t>(menu + kMenuCount, 0);                     // no entries of the game's table
    if (!list) {
        int32_t zero = 0;                                  // one more cursor slot for the fourth tab
        call(c, g_base + kVecAppend, {menu + kMenuCursorA, 1, ptr(&zero)});
        call(c, g_base + kVecAppend, {menu + kMenuCursorB, 1, ptr(&zero)});
    }
    for (int i = 0; i < rows; ++i) {
        const uint64_t item = call(c, g_base + kAllocate, {heap, kItemBytes, 16, 0});
        if (!item || !mem::is_mapped(item, kItemBytes)) return;
        std::memset(reinterpret_cast<void*>(item), 0, kItemBytes);
        wr<uint64_t>(item, ptr(list ? &g_ivt2[2] : &g_ivt[2]));
        wr<uint16_t>(item + 0x50, 0x0101);
        wr<uint8_t>(item + 0x52, 1);
        const int page = list && g_list.page ? std::clamp(g_list.page(i), 0, kListPages - 1) : 0;
        wr<uint64_t>(item + kItemEntry, ptr(list ? &g_list_entry[page] : &g_entries[i]));
        wr<uint8_t>(item + kItemDecidable, 1);
        wr<uint32_t>(item + kItemSetting, static_cast<uint32_t>(i));
        call(c, g_base + kAddItem, {menu, item});
    }
    if (list) NXLOG_I(kTag, "pick list \"%s\" is open: %d rows on %d tab(s)", g_list.titles[0] ? g_list.titles[0] : "", rows, std::clamp(g_list.pages, 1, kListPages));
    else if (g_opened.fetch_add(1) == 0) NXLOG_I(kTag, "the native Recomp Options screen is open: %d rows on %d tabs", kRowCount, kPages);
}
// slot 128 of the game's Options menu: its rows, or ours
void menu_build(::Ctx* c) {
    if (g_pending == kScreenNone) {
        pass(c, g_base + kBuild);
        return;
    }
    const int screen = g_pending;
    g_pending = kScreenNone;
    build_ours(c, c->x[0], screen);
}

bool same(uint64_t at, const char* text) {
    const size_t n = std::strlen(text) + 1;
    return mem::is_mapped(g_base + at, n) && !std::memcmp(reinterpret_cast<const void*>(g_base + at), text, n);
}
bool slot_is(uint64_t vt, int slot, uint64_t fn) {
    uint64_t v = 0;
    return rd(g_base + vt + static_cast<uint64_t>(slot) * 8, &v) && v == g_base + fn;
}
}  // namespace

namespace {
int mod_open_list(::Ctx* c, uint64_t item, const ::NxModList* list);   // below: the pick list for code mods
}
void optmenu_install(uint64_t base) {
    g_base = base;
    g_on = config_get_bool("fefw", "native_options", true);
}

// Main thread, once per frame: checks this is the build the addresses are for, then makes the vtables and hooks the builder.
void optmenu_frame() {
    if (g_ready || g_failed || !g_on || !g_base) return;
    uint64_t probe = 0;
    if (!rd(g_base + kVtMenu + kMSlotBuild * 8, &probe) || !probe) return;      // the game's data is not there yet
    const bool vtables = slot_is(kVtMenu, kMSlotBuild, kBuild) && slot_is(kVtMenu, kMSlotOnInit, kOnInit) && slot_is(kVtMenu, kMSlotHelp, kHelp) &&
                         slot_is(kVtMenu, kMSlotMinus, kMinus) && slot_is(kVtMenu, kMSlotKeyHelp, kKeyHelp) && slot_is(kVtList, kISlotNext, kItemNext) &&
                         slot_is(kVtList, kISlotPrev, kItemPrev) && slot_is(kVtList, kISlotValue, kItemValue) && slot_is(kVtTabFn, kFSlotInvoke, kTabInvoke);
    const bool strings = same(kStrTxt, "T_Txt") && same(kStrInfoTxt, "L_InfoTxt/T_Txt") && same(kStrInfo, "A_Info") && same(kStrPageTab, "L_PageTab") &&
                         same(kStrResetMid, "MID_KeyHelp_Config_Reset");
    bool functions = true;
    for (uint64_t f : {kCreate, kBuild, kKeyHelp, kAllocate, kAddItem, kVecAppend, kGetPart, kSetupText, kReleaseWeak, kSetText, kFindAlign, kAlignUpdate,
                       kArrow, kMapNotify})
        functions = functions && fn_lookup(g_base + f);
    if (!vtables || !strings || !functions || !mem::is_mapped(g_base + kVtMenu - 16, sizeof g_mvt) || !mem::is_mapped(g_base + kVtList - 16, sizeof g_ivt) ||
        !mem::is_mapped(g_base + kVtTabFn - 16, sizeof g_fvt)) {
        g_failed = true;
        NXLOG_W(kTag, "this build's Options screen is not the one this was written for (vtables %d, strings %d, functions %d): Recomp Options opens the F1 menu",
                vtables, strings, functions);
        return;
    }
    std::memcpy(g_mvt, reinterpret_cast<const void*>(g_base + kVtMenu - 16), sizeof g_mvt);
    std::memcpy(g_ivt, reinterpret_cast<const void*>(g_base + kVtList - 16), sizeof g_ivt);
    std::memcpy(g_fvt, reinterpret_cast<const void*>(g_base + kVtTabFn - 16), sizeof g_fvt);
    for (int i = 0; i < kMenuSlots; ++i)
        if (!fn_lookup(g_mvt[2 + i])) g_failed = true;
    for (int i = 0; i < kItemSlots; ++i)
        if (!fn_lookup(g_ivt[2 + i])) g_failed = true;
    if (g_failed) {
        NXLOG_W(kTag, "a slot of the Options screen's vtables is not a function: Recomp Options opens the F1 menu");
        return;
    }
    if (!fn_lookup(g_base + kBaseNext) || !fn_lookup(g_base + kBasePrev)) {
        g_failed = true;
        NXLOG_W(kTag, "the menu item base class is not where it was expected: Recomp Options opens the F1 menu");
        return;
    }
    std::memcpy(g_mvt2, g_mvt, sizeof g_mvt2);
    std::memcpy(g_ivt2, g_ivt, sizeof g_ivt2);
    for (int i = 0; i < kListPages; ++i) {
        g_list_entry[i].page = static_cast<uint8_t>(i);
        g_list_entry[i].type = 1;
        g_list_entry[i].label_mid = g_list_entry[i].help_mid = ptr(kEmpty);
    }
    for (int i = 0; i < kRowCount; ++i) {
        g_entries[i].page = static_cast<uint8_t>(kRows[i].page);
        g_entries[i].type = 1;
        g_entries[i].label_mid = g_entries[i].help_mid = ptr(kEmpty);
    }
    g_mvt[2 + kMSlotOnInit] = hle_register("fefw.optmenu.init", menu_init);
    g_menu_close_orig = g_mvt[2 + kMSlotClose];
    if (fn_lookup(g_menu_close_orig)) g_mvt[2 + kMSlotClose] = hle_register("fefw.optmenu.close", menu_close);
    else g_menu_close_orig = 0;
    g_mvt[2 + kMSlotHelp] = hle_register("fefw.optmenu.help", menu_help);
    g_mvt[2 + kMSlotMinus] = hle_register("fefw.optmenu.minus", menu_minus);
    g_mvt[2 + kMSlotKeyHelp] = hle_register("fefw.optmenu.keyhelp", menu_keyhelp);
    g_ivt[2 + kISlotText] = hle_register("fefw.optmenu.item_text", item_text);
    g_ivt[2 + kISlotNext] = hle_register("fefw.optmenu.item_next", item_next);
    g_ivt[2 + kISlotPrev] = hle_register("fefw.optmenu.item_prev", item_prev);
    g_ivt[2 + kISlotValue] = hle_register("fefw.optmenu.item_value", item_value);
    g_update_orig = g_ivt[2 + kISlotUpdate];
    g_ivt[2 + kISlotUpdate] = hle_register("fefw.optmenu.item_update", item_update);
    g_fvt[2 + kFSlotInvoke] = hle_register("fefw.optmenu.tab_label", tab_label);
    for (int slot : {kMSlotOnInit, kMSlotHelp, kMSlotMinus, kMSlotKeyHelp, kMSlotClose}) g_mvt2[2 + slot] = g_mvt[2 + slot];
    g_ivt2[2 + kISlotText] = hle_register("fefw.optmenu.list_text", list_text);
    g_ivt2[2 + kISlotACall] = hle_register("fefw.optmenu.list_acall", list_acall);
    g_ivt2[2 + kISlotValue] = hle_register("fefw.optmenu.list_value", list_value);
    g_ivt2[2 + kISlotBCall] = hle_register("fefw.optmenu.list_bcall", list_bcall);
    g_ivt2[2 + kISlotUpdate] = hle_register("fefw.optmenu.list_update", list_update);
    // the reset prompt: optional (without it minus does nothing and the key help has no Reset entry)
    g_reset_ok = fn_lookup(g_base + kPrompt) && same(kStrResetAsk, "MID_Config_Reset_Confirm") && slot_is(kVtMenu, kMSlotMinus, kMinus) &&
                 mem::is_mapped(g_base + kVtResetYes - 16, sizeof g_yvt) && mem::is_mapped(g_base + kVtResetNo, 9 * 8);
    if (g_reset_ok) {
        std::memcpy(g_yvt, reinterpret_cast<const void*>(g_base + kVtResetYes - 16), sizeof g_yvt);
        uint64_t no_invoke = 0;
        for (int i = 0; i < 9; ++i) g_reset_ok = g_reset_ok && fn_lookup(g_yvt[2 + i]);
        g_reset_ok = g_reset_ok && rd(g_base + kVtResetNo + kFSlotInvoke * 8, &no_invoke) && fn_lookup(no_invoke);
    }
    if (g_reset_ok) {
        g_yes_new = g_yvt[2 + kFSlotCloneNew];
        g_yes_at = g_yvt[2 + kFSlotCloneAt];
        g_yvt[2 + kFSlotInvoke] = hle_register("fefw.optmenu.reset_yes", reset_yes);
        g_yvt[2 + kFSlotCloneNew] = hle_register("fefw.optmenu.reset_yes_clone_new", reset_yes_clone_new);
        g_yvt[2 + kFSlotCloneAt] = hle_register("fefw.optmenu.reset_yes_clone_at", reset_yes_clone_at);
        // Quit Game's dialog: the same "yes" with our own invoke
        std::memcpy(g_qvt, g_yvt, sizeof g_qvt);
        g_qvt[2 + kFSlotInvoke] = hle_register("fefw.optmenu.quit_yes", quit_yes);
        g_qvt[2 + kFSlotCloneNew] = hle_register("fefw.optmenu.quit_yes_clone_new", quit_yes_clone_new);
        g_qvt[2 + kFSlotCloneAt] = hle_register("fefw.optmenu.quit_yes_clone_at", quit_yes_clone_at);
        g_dialog_no_first = find_no_first();
        g_quit_ok = true;
        if (!g_dialog_no_first) NXLOG_W(kTag, "quit: the dialog's cursor call was not found in Return to Title: the Quit Game dialog starts on Yes");
    } else NXLOG_W(kTag, "the Options screen's reset prompt is not where it was expected: minus does nothing on Recomp Options");
    // The report window's global: checked through the two places that store it (see kPromptWindow); without it the list guard works as
    // before (no wait for the window, no refusal, no close).
    g_report_ok = fn_lookup(g_base + kReportClose) && mem::is_mapped(g_base + kReportWindow, 8);
    // Each site starts with the adrp of the global's page (the check read the ldr there before, one instruction early, so it always failed
    // and the list never waited for the window: the save prompt came back after a save, seen on a PC).
    for (uint64_t at : kReportStores) {
        uint32_t w[8] = {};
        bool store = false;
        if (!rd(g_base + at, &w)) g_report_ok = false;
        const bool adrp = (w[0] & 0x9f00001fu) == 0x90000008u;      // adrp x8, page
        int64_t imm = static_cast<int64_t>((((w[0] >> 5) & 0x7ffffu) << 2) | ((w[0] >> 29) & 3u));
        if (imm & (int64_t{1} << 20)) imm -= int64_t{1} << 21;
        const bool page = adrp && static_cast<uint64_t>(static_cast<int64_t>(at & ~0xfffull) + imm * 0x1000) == (kReportWindow & ~0xfffull);
        for (int i = 3; i < 8; ++i) store = store || (w[i] & 0xffffffe0u) == 0xf9006100u;   // str xN, [x8,#0xc0]
        g_report_ok = g_report_ok && page && w[1] == 0xf9406109u && (w[2] & 0xff00001fu) == 0xb5000009u && store;   // ldr x9, [x8,#0xc0]; cbnz x9
    }
    if (!g_report_ok) NXLOG_W(kTag, "the game's report window is not where it was expected: the save list does not wait for it");
    g_ivt2[2 + kISlotNext] = hle_register("fefw.optmenu.list_next", list_next);
    g_ivt2[2 + kISlotPrev] = hle_register("fefw.optmenu.list_prev", list_prev);
    services::modapi::set_list_opener(mod_open_list);
    const uint64_t slot = g_base + kVtMenu + kMSlotBuild * 8;
    mem::protect(slot & ~0xfffull, 0x1000, mem::Prot::RW);
    wr<uint64_t>(slot, hle_register("fefw.optmenu.build", menu_build));
    g_ready = true;
    NXLOG_I(kTag, "Options screen hooked: Recomp Options opens a native screen ([fefw] native_options)");
}

bool optmenu_ask_quit(::Ctx* c, uint64_t item) {
    if (!g_ready || !g_quit_ok || !msg_ready()) return false;
    uint64_t menu = 0;
    if (!rd(item + 8, &menu) || !menu) return false;
    static uint64_t ask, yes[6], no[6], none[6];      // static: the dialog copies the functions, the message id is a pointer it may keep
    ask = ptr(kQuitMid);
    std::memset(yes, 0, sizeof yes), std::memset(no, 0, sizeof no), std::memset(none, 0, sizeof none);
    yes[0] = ptr(&g_qvt[2]), yes[1] = menu, yes[4] = ptr(yes);
    no[0] = g_base + kVtResetNo, no[4] = ptr(no);
    g_quit_asked_at = std::chrono::steady_clock::now();
    const uint64_t dialog = call(c, g_base + kPrompt, {menu, ptr(&ask), ptr(yes), ptr(no), ptr(none)});
    if (!dialog) {
        NXLOG_W(kTag, "quit: the game made no dialog: the overlay's prompt is used");
        return false;
    }
    if (g_dialog_no_first) call(c, g_dialog_no_first, {dialog});
    static bool told = false;
    if (!told) {
        told = true;
        NXLOG_I(kTag, "quit: the game's own dialog is shown (cursor on %s)", g_dialog_no_first ? "No" : "Yes");
    }
    return true;
}

// From the Recomp Options entry's ACall (item = the entry): opens the screen as the game's own Options entry does. False: not available.
bool optmenu_open(::Ctx* c, uint64_t item) {
    if (!g_ready) return false;
    uint64_t menu = 0, root = 0;
    if (!rd(item + 0x8, &menu) || !menu) return false;
    if (rd(menu + 0x98, &root) && root) menu = root;
    g_pending = kScreenOptions;
    call(c, g_base + kCreate, {menu, 0, 0});
    const bool built = g_pending == kScreenNone;
    g_pending = kScreenNone;
    return built;
}

// A pick list of up to three tabs on the same screen class, opened from a menu item's ACall (item = that item). A on a row calls
// list.pick(c, row), whose answer says what happens next. False: not available.
bool optmenu_open_list(::Ctx* c, uint64_t item, const OptList& list) {
    if (!g_ready || list.count < 1) return false;
    uint64_t menu = 0, root = 0;
    if (!rd(item + 0x8, &menu) || !menu) return false;
    if (rd(menu + 0x98, &root) && root) menu = root;
    g_list = list;
    g_pending = kScreenList;
    call(c, g_base + kCreate, {menu, 0, 0});
    const bool built = g_pending == kScreenNone;
    g_pending = kScreenNone;
    return built;
}
// The same list for a code mod (nxrt/mod_api.h version 4, open_list): the mod's list is kept and its functions are reached through ours.
namespace {
::NxModList g_mod_list{};
int mod_page(int row) { return g_mod_list.page ? g_mod_list.page(row, g_mod_list.user) : 0; }
void mod_label(int row, char* out, size_t n) { if (g_mod_list.label) g_mod_list.label(row, out, n, g_mod_list.user); }
void mod_value(int row, char* out, size_t n) { if (g_mod_list.value) g_mod_list.value(row, out, n, g_mod_list.user); }
void mod_help(int row, char* out, size_t n) { if (g_mod_list.help) g_mod_list.help(row, out, n, g_mod_list.user); }
uint32_t mod_pick(::Ctx* c, int row) { return g_mod_list.pick ? g_mod_list.pick(c, row, g_mod_list.user) : kClose; }
void mod_back() { if (g_mod_list.back) g_mod_list.back(g_mod_list.user); }
int mod_side(::Ctx* c, int row, int dir) { return g_mod_list.side ? g_mod_list.side(c, row, dir, g_mod_list.user) : 0; }
int mod_open_list(::Ctx* c, uint64_t item, const ::NxModList* list) {
    if (!list || list->size < sizeof(::NxModList)) return 0;
    g_mod_list = *list;
    OptList l;
    l.pages = std::clamp(list->pages, 1, kListPages);
    for (int i = 0; i < kListPages; ++i) l.titles[i] = list->titles[i];
    l.count = list->count;
    l.page = list->page ? mod_page : nullptr;
    l.label = mod_label;
    l.value = mod_value;
    l.help = mod_help;
    l.pick = mod_pick;
    l.back = mod_back;
    l.side = mod_side;
    return optmenu_open_list(c, item, l) ? 1 : 0;
}
}  // namespace
// The pick list closes as B on a row closes it (the menu's handler with the answer 3), when it is open. For a pick whose prompt decided
// something the list must not stay over.
// A Load from the pick list is going ahead: the game ends with our menus open, so give back the details box block they hold.
void optmenu_release_help() {
    if (!help_fix_on() || g_help_holds.empty()) return;
    int32_t n = 0;
    for (const auto& h : g_help_holds) {
        n += h.n;
        g_help_released.push_back(h.menu);
    }
    g_help_holds.clear();
    uint64_t fh = 0;
    const int32_t count = help_block();
    if (count == -99 || !rd(g_base + kUiCount, &fh) || !fh) return;
    const int32_t now = std::max(count - n, 0);
    wr<int32_t>(fh + 0xfc, now);
    NXLOG_I(kTag, "Load: details box block %d -> %d, given back for our open menus ([fefw] help_unblock)", count, now);
}
void optmenu_close_list(::Ctx* c) {
    uint64_t vt = 0;
    const uint64_t menu = g_list_menu;
    if (!g_ready || !menu || !rd(menu, &vt) || vt != ptr(&g_mvt2[2]) || !fn_lookup(g_base + kMenuHandle)) return;
    g_prompt = kPromptNone;
    g_list_menu = 0;
    call(c, g_base + kMenuHandle, {menu, kClose});
}
}  // namespace nxrt::games::ObeliskChamber

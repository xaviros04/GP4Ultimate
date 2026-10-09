#include "app.h"

#include "core/paths.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/ini.h"
#include "i18n.h"
#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"

using namespace gp4u;

namespace gp4cfg {

namespace {

const ImVec4 kGreen(0.45f, 0.85f, 0.45f, 1.0f);
const ImVec4 kAmber(1.0f, 0.75f, 0.30f, 1.0f);
const ImVec4 kRed(1.0f, 0.42f, 0.38f, 1.0f);
const ImVec4 kDim(0.62f, 0.64f, 0.68f, 1.0f);

bool truthy(const std::string& v) {
    std::string l = Ini::lower(v);
    return l == "1" || l == "true" || l == "yes" || l == "on" || l == "si" || l == "sí";
}

int digits_for(double step) {
    if (!(step > 0)) return 3;
    int d = (int)std::ceil(-std::log10(step) - 1e-9);
    return std::clamp(d, 0, 8);
}

std::string fixed(double v, int digits) {
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", digits, v);
    std::string s = b;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

std::string slug(const std::string& name) {
    std::string word, best;
    auto flush = [&]() {
        if (best.empty() && word.size() >= 2) best = word;
        word.clear();
    };
    for (char ch : name) {
        unsigned char c = (unsigned char)ch;
        if (std::isalnum(c) && c < 128) word += (char)std::tolower(c);
        else flush();
    }
    flush();
    return best.empty() ? "device" : best;
}

void help_marker(const std::string& text) {
    if (text.empty()) return;
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// Horizontal bar 0..1 with an optional centre mark (steering).
void bar(double v, float width, bool centre, const char* overlay = nullptr) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetTextLineHeight() * 0.8f;
    ImU32 bg = ImGui::GetColorU32(ImGuiCol_FrameBg), fg = ImGui::GetColorU32(ImGuiCol_PlotHistogram);
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), bg, 3.0f);
    float x = (float)std::clamp(v, 0.0, 1.0) * width;
    if (centre) {
        float c = width * 0.5f;
        dl->AddRectFilled(ImVec2(p.x + std::min(c, x), p.y), ImVec2(p.x + std::max(c, x), p.y + h), fg, 2.0f);
        dl->AddLine(ImVec2(p.x + c, p.y - 1), ImVec2(p.x + c, p.y + h + 1), ImGui::GetColorU32(ImGuiCol_Text), 1.0f);
    } else {
        dl->AddRectFilled(p, ImVec2(p.x + x, p.y + h), fg, 3.0f);
    }
    if (overlay) {
        ImVec2 ts = ImGui::CalcTextSize(overlay);
        dl->AddText(ImVec2(p.x + (width - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), overlay);
    }
    ImGui::Dummy(ImVec2(width, h));
}

// "alias.bN" / "alias.buttonN" / "alias.povN"
bool parse_ref(const std::string& v, const char* shortp, const char* longp, std::string& dev, int& idx) {
    std::string t = Ini::lower(Ini::trim(v));
    size_t dot = t.find('.');
    if (dot == std::string::npos) return false;
    dev = t.substr(0, dot);
    std::string o = t.substr(dot + 1), num;
    if (o.rfind(longp, 0) == 0) num = o.substr(std::strlen(longp));
    else if (o.rfind(shortp, 0) == 0) num = o.substr(std::strlen(shortp));
    else return false;
    long long n;
    if (!parse_int(num, n) || n < 0 || n > 127) return false;
    idx = (int)n;
    return true;
}

std::string compose_source(const AxisSource& s, int target) {
    std::string t = s.device + "." + axis_name(s.axis);
    if (s.invert) t += " invert";
    if (s.bipolar && target != AX_X) t += " center";
    if (s.deadzone > 0) t += " deadzone=" + fixed(s.deadzone, 3);
    if (s.gamma != 1.0) t += " gamma=" + fixed(s.gamma, 2);
    if (s.cal_min > 0) t += " min=" + fixed(s.cal_min, 3);
    if (s.cal_max < 1) t += " max=" + fixed(s.cal_max, 3);
    return t;
}

// A file, not UI: always English, whatever the configurator's language.
const char* kNewFileHeader =
    "; gp4ultimate.ini - settings of the Grand Prix 4 mod.\r\n"
    "; Edited with gp4ultimate-config.exe (or by hand). Options that do not appear use the mod's\r\n"
    "; default, so improvements in new versions apply by themselves.\r\n"
    "; Every option, commented: gp4ultimate_reference.ini in the mod package.\r\n";

// The DLL's parser speaks English in gp4ultimate.log, as shown in English; in Spanish, the
// common messages in plain words.
std::string explain(const std::string& m) {
    if (current_language() != "es") return m;
    auto between = [&](const std::string& a, const std::string& b) {
        size_t p = m.find(a);
        if (p == std::string::npos) return std::string();
        p += a.size();
        size_t q = m.find(b, p);
        return m.substr(p, q == std::string::npos ? std::string::npos : q - p);
    };
    if (m.find("is not used by any mapping") != std::string::npos)
        return "El mando \"" + between("[devices] ", " is") + "\" no se usa en ninguna asignación (se puede quitar en Mostrar avanzados).";
    if (m.find("unknown device alias") != std::string::npos)
        return m.substr(0, m.find(':')) + ": usa el mando \"" + between("alias '", "'") + "\", que no existe en [devices].";
    if (m.find("is not a [devices] alias") != std::string::npos)
        return "La base del force feedback \"" + between("ffb = ", " is") + "\" no existe en [devices].";
    if (m.find("bound twice") != std::string::npos) return m.substr(0, m.find(" bound")) + " está asignado dos veces (vale el último).";
    return m;
}

const char* axis_role_comment(int target) {
    switch (target) {
        case AX_X: return "steering";
        case AX_Y: return "throttle";
        case AX_RZ: return "brake";
        case AX_Z: return "clutch";
    }
    return "";
}

}  // namespace

void App::clear_assignments() {
    for (const char* s : {"devices", "axes", "buttons", "povs"})
        for (auto& kv : doc_.entries(s)) doc_.remove(s, kv.first);
    doc_.remove("virtual", "ffb");
    set_status(T("Assignments cleared. Assign your controllers with \"Detect\" and save."));
}

void App::set_axis(int target, const std::string& value) {
    std::string key = axis_name(target);
    bool is_new = !doc_.has("axes", key);
    doc_.set("axes", key, value);
    if (is_new && *axis_role_comment(target)) doc_.set_comment("axes", key, axis_role_comment(target));
}

void App::set_status(const std::string& s, bool error) {
    status_ = s;
    status_error_ = error;
    status_time_ = now_;
}

void App::init(HWND hwnd, const Options& o, const std::string& embedded_schema) {
    opt_ = o;
    hwnd_ = hwnd;
    advanced_ = o.advanced;
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    dir = dir.substr(0, dir.find_last_of("\\/"));
    exe_dir_ = dir;
    embedded_schema_ = embedded_schema;
    ini_path_ = o.ini_path.empty() ? dir + "\\gp4ultimate.ini" : o.ini_path;

    // The document first: it says which language everything else speaks.
    bool legacy = false, loaded = false;
    std::string source = o.ini_path.empty() ? config_file(dir, &legacy) : ini_path_;
    if (doc_.load_file(source)) {
        loaded = true;
        saved_ = legacy ? std::string() : doc_.text();   // an old gp4mod.ini: saving writes gp4ultimate.ini
    } else {
        // First run: a short file with only what the user sets. Options left out follow the
        // mod's defaults, so improvements in later versions reach this user by themselves.
        doc_.load_string(kNewFileHeader);
        saved_.clear();
    }
    apply_language(resolve_language(doc_.get("general", "language")));
    if (!loaded) set_status(T("No gp4ultimate.ini yet: new configuration (created when you save)."));
    else if (legacy) set_status(T("Read gp4mod.ini (older version): saving creates gp4ultimate.ini."));
    in_.init(hwnd, o.dinput_dll);
    if (!in_.error().empty()) set_status(in_.error(), true);
}

void App::load_schema() {
    // The file shipped next to the exe (a newer mod brings a newer one), else ours.
    schema_ = Schema();
    schema_warnings_.clear();
    Schema ext;
    IniDoc probe;
    std::vector<std::string> w;
    if (probe.load_file(exe_dir_ + "\\gp4ultimate_schema.ini") && ext.load(probe.text(), &w, lang_)) {
        schema_ = ext;
        schema_warnings_ = w;
        schema_origin_ = "gp4ultimate_schema.ini";
        Schema emb;
        if (emb.load(embedded_schema_, nullptr, lang_) && emb.version > ext.version) {   // never go back to an older one
            schema_ = emb;
            schema_warnings_.clear();
            schema_origin_ = "built-in (the file is older)";
        }
    } else {
        schema_.load(embedded_schema_, &schema_warnings_, lang_);
        schema_origin_ = "built-in";
    }
}

void App::apply_language(const std::string& lang) {
    lang_ = lang;
    set_current_language(lang);
    load_schema();   // labels, help and page names come from the schema in this language
}

void App::set_language(const std::string& code) {
    bool is_new = !doc_.has("general", "language");
    doc_.set("general", "language", code);
    if (is_new) doc_.set_comment("general", "language", "auto | en | es (auto = Windows language)");
}

std::string App::page_label(const std::string& page) const {
    std::string l = schema_.tr(page);   // the schema's own translation first
    return l != page ? l : T(page);
}

bool App::save() {
    bool backup = false;
    if (!saved_.empty()) {   // an existing gp4ultimate.ini (not a first save or an old gp4mod.ini)
        std::string bak = ini_path_ + ".bak";
        backup = CopyFileA(ini_path_.c_str(), bak.c_str(), FALSE) != 0;
    }
    if (!doc_.save_file(ini_path_)) {
        set_status(strf(T("Could not save %s (read-only?)"), to_utf8(ini_path_.c_str()).c_str()), true);
        return false;
    }
    saved_ = doc_.text();
    set_status(backup ? T("Saved. The previous version is in gp4ultimate.ini.bak. Restart GP4 to apply it.")
                      : T("Saved. Restart GP4 to apply it."));
    return true;
}

// ---- devices / aliases ----------------------------------------------------------------

std::vector<App::AliasInfo> App::resolve_aliases() const {
    std::vector<AliasInfo> out;
    auto& list = const_cast<Input&>(in_).controllers();
    std::vector<bool> taken(list.size(), false);
    for (auto& kv : doc_.entries("devices")) {
        AliasInfo a;
        a.alias = kv.first;
        std::string err;
        a.sel_ok = parse_selector(kv.second, a.sel, err);
        if (a.sel_ok) {
            if (a.sel.kind == DeviceSelector::Name) a.sel.text = Ini::lower(a.sel.text);
            for (size_t i = 0; i < list.size(); ++i) {
                const Controller& c = list[i];
                if (!taken[i] && selector_matches(a.sel, c.name, c.instance_name, c.vid, c.pid, c.guid, c.index)) {
                    a.ctrl = (int)i;
                    taken[i] = true;
                    break;
                }
            }
        }
        out.push_back(a);
    }
    return out;
}

int App::ctrl_for_alias(const std::string& alias) const {
    std::string l = Ini::lower(alias);
    for (auto& a : resolve_aliases())
        if (a.alias == l) return a.ctrl;
    return -1;
}

std::string App::alias_for(int ctrl, bool create) {
    for (auto& a : resolve_aliases())
        if (a.ctrl == ctrl) return a.alias;
    if (!create || ctrl < 0 || ctrl >= (int)in_.controllers().size()) return "";
    const Controller& c = in_.controllers()[ctrl];
    std::string base = slug(c.name), alias = base;
    for (int n = 2; doc_.has("devices", alias); ++n) alias = base + std::to_string(n);
    int same = 0;
    for (auto& o : in_.controllers()) same += o.vid == c.vid && o.pid == c.pid;
    char sel[64];
    if (same == 1 && (c.vid || c.pid)) std::snprintf(sel, sizeof sel, "vidpid:%04X:%04X", c.vid, c.pid);
    else std::snprintf(sel, sizeof sel, "guid:%s", c.guid.c_str());
    doc_.set("devices", alias, sel);
    doc_.set_comment("devices", alias, c.name);
    return alias;
}

std::string App::ctrl_label(int ctrl) const {
    auto& list = const_cast<Input&>(in_).controllers();
    if (ctrl < 0 || ctrl >= (int)list.size()) return T("(unassigned)");
    return list[ctrl].name;
}

// ---- detection --------------------------------------------------------------------------

void App::start_detect(int kind, int target) {
    det_kind_ = kind;
    det_target_ = target;
    det_start_ = now_;
    auto& list = in_.controllers();
    det_axes_.assign(list.size(), {});
    det_buttons_.assign(list.size(), {});
    det_povs_.assign(list.size(), {});
    for (size_t i = 0; i < list.size(); ++i) {
        for (int a = 0; a < 8; ++a) det_axes_[i].push_back(list[i].axis01(a));
        for (int b = 0; b < 128; ++b) det_buttons_[i].push_back(list[i].button(b));
        for (int p = 0; p < 4; ++p) det_povs_[i].push_back(list[i].pov(p));
    }
}

void App::update_detect() {
    if (det_kind_ == DetNone) return;
    auto& list = in_.controllers();
    if (det_axes_.size() != list.size()) { det_kind_ = DetNone; return; }
    if (now_ - det_start_ > 15.0) {
        det_kind_ = DetNone;
        set_status(T("Nothing detected in 15 s."), true);
        return;
    }
    if (det_kind_ == DetAxis) {
        double threshold = det_target_ == AX_X ? 0.18 : 0.35;
        int bc = -1, ba = -1;
        double bd = 0;
        for (size_t i = 0; i < list.size(); ++i)
            for (int a = 0; a < 8; ++a) {
                if (!list[i].axis[a]) continue;
                double d = list[i].axis01(a) - det_axes_[i][a];
                if (std::fabs(d) > std::fabs(bd)) { bd = d; bc = (int)i; ba = a; }
            }
        if (bc < 0 || std::fabs(bd) < threshold) return;
        std::string key = axis_name(det_target_);
        AxisSource s;
        std::string err;
        bool had = parse_axis_source(doc_.get("axes", key), s, err) && doc_.get("axes", key).find("combine") == std::string::npos;
        if (!had) s = AxisSource{};
        s.device = alias_for(bc, true);
        s.axis = ba;
        s.invert = bd < 0;   // pressing / turning right must raise the value
        set_axis(det_target_, compose_source(s, det_target_));
        set_status(strf(T("Assigned: %s axis %s%s"), list[bc].name.c_str(), axis_name(ba), s.invert ? T(" (inverted)") : ""));
        det_kind_ = DetNone;
    } else if (det_kind_ == DetButton) {
        for (size_t i = 0; i < list.size(); ++i)
            for (int b = 0; b < std::min(128, list[i].buttons); ++b)
                if (list[i].button(b) && !det_buttons_[i][b]) {
                    std::string key = std::to_string(det_target_);
                    doc_.set("buttons", key, alias_for((int)i, true) + ".b" + std::to_string(b));
                    set_status(strf(T("Assigned: %s button %d"), list[i].name.c_str(), b + 1));
                    det_kind_ = DetNone;
                    return;
                }
    } else if (det_kind_ == DetPov) {
        for (size_t i = 0; i < list.size(); ++i)
            for (int p = 0; p < std::min(4, list[i].povs); ++p)
                if (list[i].pov(p) != det_povs_[i][p] && LOWORD(list[i].pov(p)) != 0xFFFF) {
                    doc_.set("povs", std::to_string(det_target_), alias_for((int)i, true) + ".pov" + std::to_string(p));
                    set_status(strf(T("Assigned the POV hat of %s"), list[i].name.c_str()));
                    det_kind_ = DetNone;
                    return;
                }
    }
}

// ---- UI -----------------------------------------------------------------------------------

void App::frame(double now) {
    now_ = now;
    // The language follows the document: the selector, "Discard changes" or a hand edit.
    std::string want = resolve_language(doc_.get("general", "language"));
    if (want != lang_) apply_language(want);
    in_.poll();
    in_.update_ffb_test(now);
    update_detect();
    FfbTest& t = in_.ffb_test();
    if (t.state == FfbTest::Done || t.state == FfbTest::Failed) {
        if (t.state == FfbTest::Done && t.result != 0) {
            if (t.result < 0) doc_.set("ffb", "invert", "1");
            else if (doc_.has("ffb", "invert")) doc_.set("ffb", "invert", "0");
        }
        set_status(t.message, t.state == FfbTest::Failed || t.result == 0);
        t.state = FfbTest::Idle;
    }

    // Validate with the DLL's own parser.
    Ini ini;
    ini.load_string(doc_.text());
    MappingConfig m;
    map_error_.clear();
    map_warnings_.clear();
    if (!parse_mapping(ini, m, map_error_, &map_warnings_) && map_error_.empty()) map_error_ = T("invalid configuration");

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S) && dirty()) save();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ui_top_bar();
    float status_h = ImGui::GetFrameHeightWithSpacing() * 1.6f;
    float left_w = std::max(330.0f, ImGui::GetContentRegionAvail().x * 0.31f);
    ImGui::BeginChild("devices", ImVec2(left_w, -status_h), ImGuiChildFlags_Borders);
    ui_devices_panel();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("pages", ImVec2(0, -status_h), ImGuiChildFlags_Borders);
    if (ImGui::BeginTabBar("tabs")) {
        // Page ids are the schema's English names; tabs show them translated, with the id as
        // ImGui id so the selected tab survives a language change.
        std::vector<std::string> pages = schema_.pages;
        if (std::find(pages.begin(), pages.end(), "Controls") == pages.end()) pages.insert(pages.begin(), "Controls");
        pages.push_back("Other");
        for (auto& pg : pages) {
            std::string label = page_label(pg);
            ImGuiTabItemFlags f = 0;
            if (first_frame_ && !opt_.start_page.empty() &&
                (Ini::lower(pg) == Ini::lower(opt_.start_page) || Ini::lower(label) == Ini::lower(opt_.start_page)))
                f = ImGuiTabItemFlags_SetSelected;
            if (ImGui::BeginTabItem((label + "###" + pg).c_str(), nullptr, f)) {
                ImGui::BeginChild("page");
                if (pg == "Controls") ui_controls_page();
                else if (pg == "Other") ui_other_page();
                else ui_schema_page(pg);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ui_status_bar();
    ImGui::End();
    first_frame_ = false;
}

void App::ui_top_bar() {
    bool d = dirty();
    ImGui::BeginDisabled(!d);
    if (ImGui::Button((std::string(d ? T("  Save  ") : T("  Saved  ")) + "###save").c_str())) save();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!d || saved_.empty());
    if (ImGui::Button(T("Discard changes"))) {
        doc_.load_string(saved_);
        set_status(T("Changes discarded."));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(T("Find controllers"))) {
        in_.enumerate();
        set_status(strf(T("%d controllers connected."), (int)in_.controllers().size()));
    }
    ImGui::SameLine();
    ImGui::Checkbox((std::string(T("Show advanced")) + "###advanced").c_str(), &advanced_);
    ImGui::SameLine();
    // Language names in their own language, so either can be found whatever is showing.
    const char* langs[][2] = {{"en", "English"}, {"es", "Español"}};
    const char* cur = lang_ == "es" ? langs[1][1] : langs[0][1];
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("Español").x + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2);
    if (ImGui::BeginCombo("##language", cur)) {
        for (auto& l : langs)
            if (ImGui::Selectable(l[1], lang_ == l[0])) set_language(l[0]);
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("%s", T("Language"));
    ImGui::SameLine();
    std::string path = to_utf8(ini_path_.c_str());
    std::string name = path.substr(path.find_last_of("\\/") + 1);
    std::string label = name + (d ? T("  (unsaved)") : "");
    float w = ImGui::CalcTextSize(label.c_str()).x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + 10, ImGui::GetWindowWidth() - w - 24));
    ImGui::TextColored(d ? kAmber : kDim, "%s", label.c_str());
    ImGui::SetItemTooltip("%s", path.c_str());
}

void App::ui_devices_panel() {
    ImGui::SeparatorText(T("Connected controllers"));
    auto& list = in_.controllers();
    if (list.empty()) {
        ImGui::TextColored(kAmber, "%s", T("No controllers connected."));
        ImGui::TextWrapped("%s", T("Connect the wheel base, the pedals and the rest, then press \"Find controllers\"."));
        return;
    }
    auto aliases = resolve_aliases();
    float w = ImGui::GetContentRegionAvail().x;
    for (size_t i = 0; i < list.size(); ++i) {
        const Controller& c = list[i];
        ImGui::PushID((int)i);
        std::string alias;
        for (auto& a : aliases)
            if (a.ctrl == (int)i) alias = a.alias;
        std::string head = c.name + "##dev";
        bool used = false;
        if (!alias.empty()) {
            std::string t = Ini::lower(doc_.text());   // referenced anywhere in the mapping?
            used = t.find(alias + ".") != std::string::npos || Ini::lower(doc_.get("virtual", "ffb")) == alias;
        }
        if (ImGui::CollapsingHeader(head.c_str(), used || list.size() <= 2 ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            ImGui::TextColored(kDim, "%04X:%04X", c.vid, c.pid);
            if (c.ff) { ImGui::SameLine(); ImGui::TextColored(kGreen, "force feedback"); }
            if (!alias.empty()) { ImGui::SameLine(); ImGui::TextColored(kDim, "· alias \"%s\"", alias.c_str()); }
            for (int a = 0; a < 8; ++a) {
                if (!c.axis[a]) continue;
                ImGui::TextUnformatted(axis_name(a));
                ImGui::SameLine(ImGui::GetFontSize() * 2.2f);
                bar(c.axis01(a), w - ImGui::GetFontSize() * 2.6f, false);
            }
            int nb = std::min(128, c.buttons);
            if (nb > 0) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                float s = ImGui::GetFontSize() * 0.95f, gap = 3;
                int per_row = std::max(1, (int)((w + gap) / (s + gap)));
                ImVec2 p = ImGui::GetCursorScreenPos();
                for (int b = 0; b < nb; ++b) {
                    ImVec2 q(p.x + (b % per_row) * (s + gap), p.y + (b / per_row) * (s + gap));
                    bool on = c.button(b);
                    dl->AddRectFilled(q, ImVec2(q.x + s, q.y + s),
                                      on ? ImGui::GetColorU32(ImGuiCol_PlotHistogram) : ImGui::GetColorU32(ImGuiCol_FrameBg), 3);
                    if (on || nb <= 32) {
                        char n[8];
                        std::snprintf(n, sizeof n, "%d", b + 1);
                        ImVec2 ts = ImGui::CalcTextSize(n);
                        float scale = std::min(1.0f, (s - 2) / std::max(ts.x, 1.0f));
                        dl->AddText(nullptr, ImGui::GetFontSize() * scale * 0.8f,
                                    ImVec2(q.x + (s - ts.x * scale * 0.8f) * 0.5f, q.y + (s - ts.y * scale * 0.8f) * 0.5f),
                                    ImGui::GetColorU32(on ? ImGuiCol_Text : ImGuiCol_TextDisabled), n);
                    }
                }
                int rows = (nb + per_row - 1) / per_row;
                ImGui::Dummy(ImVec2(w, rows * (s + gap)));
            }
            for (int p = 0; p < std::min(4, c.povs); ++p) {
                long v = c.pov(p);
                if (LOWORD(v) == 0xFFFF) ImGui::TextColored(kDim, T("POV %d: centred"), p + 1);
                else ImGui::Text(T("POV %d: %ld°"), p + 1, v / 100);
            }
        }
        ImGui::PopID();
    }
}

void App::ui_controls_page() {
    ImGui::TextWrapped("%s", T("Assign each function with \"Detect\" and move the control. GP4 sees a single controller, "
                               "\"GP4 Composite Wheel\": in its controller options assign steering = X axis, "
                               "throttle = Y axis, brake = Rz axis and clutch = Z axis."));
    if (!doc_.entries("devices").empty()) {
        if (ImGui::Button(T("Start over"))) ImGui::OpenPopup("clear");
        ImGui::SetItemTooltip("%s", T("Clears every assignment (for example, if this gp4ultimate.ini comes from another PC)."));
        if (ImGui::BeginPopup("clear")) {
            ImGui::TextUnformatted(T("Clear every controller assignment?"));
            if (ImGui::Button(T("Yes, clear"))) { clear_assignments(); ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if (ImGui::Button(T("Cancel"))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    ImGui::Spacing();
    ImGui::SeparatorText(T("Axes"));
    if (ImGui::BeginTable("axes", 6, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn(T("Function"));
        ImGui::TableSetupColumn(T("Controller and axis"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(T("Invert"));
        ImGui::TableSetupColumn(T("Tuning"));
        ImGui::TableSetupColumn(T("Output"));
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        ui_axis_row(AX_X, T("Steering"), T("Turn the wheel to the RIGHT (about 90°)"));
        ui_axis_row(AX_Y, T("Throttle"), T("Press the THROTTLE all the way"));
        ui_axis_row(AX_RZ, T("Brake"), T("Press the BRAKE all the way"));
        ui_axis_row(AX_Z, T("Clutch"), T("Press the CLUTCH all the way"));
        if (advanced_) {
            ui_axis_row(AX_RX, T("Extra axis RX"), T("Move the axis"));
            ui_axis_row(AX_RY, T("Extra axis RY"), T("Move the axis"));
            ui_axis_row(AX_S0, T("Slider 1"), T("Move the axis"));
            ui_axis_row(AX_S1, T("Slider 2"), T("Move the axis"));
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText(T("Buttons"));
    ImGui::TextColored(kDim, "%s", T("GP4 only reads 8 buttons. Give them a function in GP4's controller options."));
    if (ImGui::BeginTable("buttons", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn(T("GP4 button"));
        ImGui::TableSetupColumn(T("Assigned to"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(T("Note"), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 9);
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (int b = 0; b < 8; ++b) {
            char role[48];
            if (b == 0) std::snprintf(role, sizeof role, "%s", T("1 (Fire 1, accept)"));
            else std::snprintf(role, sizeof role, "%d", b + 1);
            ui_button_row(b, role);
        }
        ImGui::EndTable();
    }

    ui_ffb_device();

    if (advanced_) {
        ImGui::SeparatorText(T("POV hats"));
        if (ImGui::BeginTable("povs", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn(T("GP4 POV"));
            ImGui::TableSetupColumn(T("Assigned to@@pov"), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();
            for (int p = 0; p < 4; ++p) ui_pov_row(p);
            ImGui::EndTable();
        }
        ImGui::SeparatorText(T("Controller aliases ([devices])"));
        ImGui::TextColored(kDim, "%s", T("Created when you assign. vidpid:VVVV:PPPP identifies the model; guid:{...} one particular unit."));
        for (auto& a : resolve_aliases()) {
            ImGui::PushID(a.alias.c_str());
            std::string v = doc_.get("devices", a.alias);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
            if (ImGui::InputText(a.alias.c_str(), &v)) doc_.set("devices", a.alias, v);
            ImGui::SameLine();
            if (!a.sel_ok) ImGui::TextColored(kRed, "%s", T("invalid selector"));
            else if (a.ctrl < 0) ImGui::TextColored(kAmber, "%s", T("not connected"));
            else ImGui::TextColored(kGreen, "%s", ctrl_label(a.ctrl).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(T("Remove"))) doc_.remove("devices", a.alias);
            ImGui::PopID();
        }
        std::string autob = doc_.get("buttons", "auto");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
        if (ImGui::InputText((std::string(T("Automatic buttons (alias, ...)")) + "###autob").c_str(), &autob)) {
            if (autob.empty()) doc_.remove("buttons", "auto");
            else doc_.set("buttons", "auto", autob);
        }
        help_marker(T("Appends, in order, every button of those controllers after the assigned ones. GP4 only reads the first 8."));
    }
    ui_schema_page("Controls");
}

void App::ui_axis_row(int target, const char* role, const char* how) {
    std::string key = axis_name(target);
    std::string v = doc_.get("axes", key);
    ImGui::PushID(target);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(role);

    AxisSource s;
    std::string err;
    bool complex = !v.empty() && (Ini::lower(v).rfind("combine(", 0) == 0 || !parse_axis_source(v, s, err));
    int ctrl = (!v.empty() && !complex) ? ctrl_for_alias(s.device) : -1;
    auto& list = in_.controllers();

    ImGui::TableNextColumn();
    if (det_kind_ == DetAxis && det_target_ == target) {
        ImGui::TextColored(kAmber, "%s...", how);
    } else if (complex) {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##raw", &v)) doc_.set("axes", key, v);
        ImGui::SetItemTooltip("%s", T("Advanced mapping: edit it as text."));
    } else {
        std::string cur = v.empty() ? T("(unassigned)")
                                    : (ctrl >= 0 ? list[ctrl].name : strf(T("%s (not connected)"), s.device.c_str())) +
                                          strf(T("  ·  axis %s"), axis_name(s.axis));
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##dev", cur.c_str())) {
            if (ImGui::Selectable(T("(unassigned)"), v.empty())) doc_.remove("axes", key);
            for (size_t i = 0; i < list.size(); ++i)
                for (int a = 0; a < 8; ++a) {
                    if (!list[i].axis[a]) continue;
                    std::string label = list[i].name + strf(T("  ·  axis %s"), axis_name(a)) + "##" + std::to_string(i) + "_" + std::to_string(a);
                    if (ImGui::Selectable(label.c_str(), ctrl == (int)i && s.axis == a)) {
                        AxisSource n = v.empty() ? AxisSource{} : s;
                        n.device = alias_for((int)i, true);
                        n.axis = a;
                        set_axis(target, compose_source(n, target));
                    }
                }
            ImGui::EndCombo();
        }
    }

    ImGui::TableNextColumn();
    if (!v.empty() && !complex) {
        bool inv = s.invert;
        if (ImGui::Checkbox("##inv", &inv)) {
            s.invert = inv;
            set_axis(target, compose_source(s, target));
        }
    }

    ImGui::TableNextColumn();
    if (!v.empty() && !complex) {
        if (ImGui::SmallButton(T("Tune"))) ImGui::OpenPopup("tune");
        if (ImGui::BeginPopup("tune")) {
            bool changed = false;
            float dz = (float)s.deadzone, ga = (float)s.gamma, mn = (float)s.cal_min, mx = (float)s.cal_max;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
            changed |= ImGui::SliderFloat(T("Deadzone"), &dz, 0.0f, 0.3f, "%.3f");
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
            changed |= ImGui::SliderFloat(T("Curve (gamma)"), &ga, 0.3f, 3.0f, "%.2f");
            help_marker(T("1 = linear. Above 1 = gentler at the start (useful for the brake)."));
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
            changed |= ImGui::SliderFloat(T("Travel start"), &mn, 0.0f, 0.5f, "%.3f");
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
            changed |= ImGui::SliderFloat(T("Travel end"), &mx, 0.5f, 1.0f, "%.3f");
            help_marker(T("To reach 100% without pressing all the way, lower the travel end."));
            if (changed && mx > mn) {
                s.deadzone = std::clamp((double)dz, 0.0, 0.95);
                s.gamma = std::clamp((double)ga, 0.1, 10.0);
                s.cal_min = mn;
                s.cal_max = mx;
                set_axis(target, compose_source(s, target));
            }
            ImGui::EndPopup();
        }
    }

    ImGui::TableNextColumn();
    float bw = ImGui::GetFontSize() * 6.5f;
    if (!v.empty() && !complex && ctrl >= 0) {
        AxisBinding b;
        b.target = target;
        b.a = s;
        if (target == AX_X) b.a.bipolar = true;
        double out = apply_binding(b, list[ctrl].axis01(s.axis), 0.0);
        char pct[16];
        std::snprintf(pct, sizeof pct, "%d%%", (int)std::lround(target == AX_X ? (out * 2 - 1) * 100 : out * 100));
        bar(out, bw, target == AX_X || b.a.bipolar, pct);
    } else {
        ImGui::Dummy(ImVec2(bw, 1));
    }

    ImGui::TableNextColumn();
    if (det_kind_ == DetAxis && det_target_ == target) {
        if (ImGui::SmallButton(T("Cancel"))) det_kind_ = DetNone;
    } else {
        if (ImGui::SmallButton(T("Detect"))) start_detect(DetAxis, target);
        if (!v.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton(T("Remove"))) doc_.remove("axes", key);
        }
    }
    ImGui::PopID();
}

void App::ui_button_row(int vb, const char* role) {
    std::string key = std::to_string(vb);
    std::string v = doc_.get("buttons", key);
    ImGui::PushID(100 + vb);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(role);
    ImGui::TableNextColumn();
    std::string dev;
    int idx = 0;
    bool ok = !v.empty() && parse_ref(v, "b", "button", dev, idx);
    int ctrl = ok ? ctrl_for_alias(dev) : -1;
    if (det_kind_ == DetButton && det_target_ == vb) {
        ImGui::TextColored(kAmber, "%s", T("Press the button you want..."));
    } else if (v.empty()) {
        ImGui::TextColored(kDim, "%s", T("(unassigned)"));
    } else if (!ok) {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##raw", &v)) doc_.set("buttons", key, v);
    } else {
        bool on = ctrl >= 0 && in_.controllers()[ctrl].button(idx);
        ImGui::TextColored(on ? kGreen : ImGui::GetStyleColorVec4(ImGuiCol_Text), T("%s  ·  button %d%s"),
                           ctrl >= 0 ? in_.controllers()[ctrl].name.c_str() : strf(T("%s (not connected)"), dev.c_str()).c_str(),
                           idx + 1, on ? T("  (pressed)") : "");
    }
    ImGui::TableNextColumn();
    if (!v.empty()) {
        std::string note = doc_.comment("buttons", key);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##note", T("e.g. shift up"), &note)) {
            for (auto& ch : note)
                if (ch == ';' || ch == '#') ch = ',';
            doc_.set_comment("buttons", key, note);
        }
    }
    ImGui::TableNextColumn();
    if (det_kind_ == DetButton && det_target_ == vb) {
        if (ImGui::SmallButton(T("Cancel"))) det_kind_ = DetNone;
    } else {
        if (ImGui::SmallButton(T("Detect"))) start_detect(DetButton, vb);
        if (!v.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton(T("Remove"))) doc_.remove("buttons", key);
        }
    }
    ImGui::PopID();
}

void App::ui_pov_row(int vp) {
    std::string key = std::to_string(vp);
    std::string v = doc_.get("povs", key);
    ImGui::PushID(200 + vp);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::Text(T("POV %d"), vp + 1);
    ImGui::TableNextColumn();
    std::string dev;
    int idx = 0;
    if (det_kind_ == DetPov && det_target_ == vp) ImGui::TextColored(kAmber, "%s", T("Move the POV hat..."));
    else if (v.empty()) ImGui::TextColored(kDim, "%s", T("(unassigned)"));
    else if (parse_ref(v, "pov", "pov", dev, idx)) {
        int ctrl = ctrl_for_alias(dev);
        ImGui::Text(T("%s  ·  POV %d"),
                    ctrl >= 0 ? in_.controllers()[ctrl].name.c_str() : strf(T("%s (not connected)"), dev.c_str()).c_str(), idx + 1);
    } else {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##raw", &v)) doc_.set("povs", key, v);
    }
    ImGui::TableNextColumn();
    if (det_kind_ == DetPov && det_target_ == vp) {
        if (ImGui::SmallButton(T("Cancel"))) det_kind_ = DetNone;
    } else {
        if (ImGui::SmallButton(T("Detect"))) start_detect(DetPov, vp);
        if (!v.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton(T("Remove"))) doc_.remove("povs", key);
        }
    }
    ImGui::PopID();
}

void App::ui_ffb_device() {
    ImGui::SeparatorText(T("Force feedback"));
    auto& list = in_.controllers();
    std::string alias = doc_.get("virtual", "ffb");
    int ctrl = alias.empty() ? -1 : ctrl_for_alias(alias);
    std::string cur = alias.empty() ? T("(none)") : ctrl >= 0 ? list[ctrl].name : strf(T("%s (not connected)"), alias.c_str());
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(T("Force goes to"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
    if (ImGui::BeginCombo("##ffbdev", cur.c_str())) {
        if (ImGui::Selectable(T("(none)"), alias.empty())) doc_.remove("virtual", "ffb");
        for (size_t i = 0; i < list.size(); ++i) {
            if (!list[i].ff) continue;
            if (ImGui::Selectable((list[i].name + "##ff" + std::to_string(i)).c_str(), ctrl == (int)i))
                doc_.set("virtual", "ffb", alias_for((int)i, true));
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    FfbTest& t = in_.ffb_test();
    bool running = t.state == FfbTest::Positive || t.state == FfbTest::Negative;
    ImGui::BeginDisabled(ctrl < 0 || running);
    if (ImGui::Button((std::string(running ? T("Testing... let go of the wheel") : T("Test direction")) + "###ffbtest").c_str()))
        in_.start_ffb_test(ctrl, now_);
    ImGui::EndDisabled();
    help_marker(T("Let go of the wheel: the base pushes one way and then the other (3 s), and 'Invert the force' is set "
                  "if needed. GP4 must be closed."));
    if (!alias.empty() && ctrl < 0) ImGui::TextColored(kAmber, T("The base \"%s\" is not connected."), alias.c_str());
    else if (ctrl >= 0 && ctrl_for_alias(alias) >= 0) {
        bool used_for_steering = false;
        AxisSource s;
        std::string err;
        if (parse_axis_source(doc_.get("axes", "X"), s, err)) used_for_steering = Ini::lower(s.device) == Ini::lower(alias);
        if (!used_for_steering) ImGui::TextColored(kAmber, "%s", T("Note: the steering (X axis) does not come from this controller."));
    }
}

bool App::ui_item(const SchemaItem& it) {
    bool present = doc_.has(it.section, it.key);
    std::string val = present ? doc_.get(it.section, it.key) : it.def;
    std::string id = "##" + it.section + "." + it.key;
    bool changed = false;
    std::string nv = val;

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(it.label.c_str());
    if (!it.help.empty() && ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        ImGui::TextUnformatted(it.help.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-1);
    if (it.type == "bool") {
        bool b = truthy(val);
        if (ImGui::Checkbox(id.c_str(), &b)) { nv = b ? "1" : "0"; changed = true; }
    } else if (it.type == "float" || it.type == "int") {
        double d = 0;
        parse_double(val, d);
        int digits = it.type == "int" ? 0 : digits_for(it.step);
        std::string fmt = "%." + std::to_string(digits) + "f" + (it.unit.empty() ? "" : " " + it.unit);
        if (it.has_min && it.has_max) {
            double mn = it.min, mx = it.max;
            if (ImGui::SliderScalar(id.c_str(), ImGuiDataType_Double, &d, &mn, &mx, fmt.c_str())) changed = true;
            ImGui::SetItemTooltip("%s", T("Ctrl+click to type the value"));
        } else {
            if (ImGui::InputDouble(id.c_str(), &d, 0, 0, fmt.c_str())) changed = true;
        }
        if (changed) {
            if (it.has_min) d = std::max(d, it.min);
            if (it.has_max) d = std::min(d, it.max);
            if (it.step > 0) d = std::round(d / it.step) * it.step;
            nv = it.type == "int" ? std::to_string((long long)std::llround(d)) : fixed(d, std::max(digits, 1));
        }
    } else if (it.type == "choice") {
        std::string cur = val;
        for (auto& c : it.choices)
            if (c.first == Ini::lower(val)) cur = c.second;
        if (ImGui::BeginCombo(id.c_str(), cur.c_str())) {
            for (auto& c : it.choices)
                if (ImGui::Selectable(c.second.c_str(), c.first == Ini::lower(val))) { nv = c.first; changed = true; }
            ImGui::EndCombo();
        }
    } else {   // text, hex and anything newer
        if (ImGui::InputText(id.c_str(), &nv)) changed = true;
        if (it.type == "hex") {
            long long x;
            if (!nv.empty() && !parse_int(nv, x)) ImGui::TextColored(kRed, "%s", T("not a number (e.g. 0x7A)"));
        }
    }
    ImGui::TableNextColumn();
    if (present) {
        if (ImGui::SmallButton((T("Reset") + id).c_str())) {
            doc_.remove(it.section, it.key);
            return true;
        }
        ImGui::SetItemTooltip(T("Back to the mod's default: %s"), it.def.empty() ? T("(empty)") : it.def.c_str());
    } else {
        ImGui::TextColored(kDim, "%s", T("default"));
    }
    if (changed && (nv != val || !present)) {
        for (auto& ch : nv)
            if ((ch == ';' || ch == '#') && it.type != "text") ch = ' ';
        doc_.set(it.section, it.key, nv);
    }
    return changed;
}

void App::ui_schema_page(const std::string& page) {
    std::vector<std::string> groups;
    for (auto& it : schema_.items)
        if (it.page == page && (advanced_ || !it.advanced) && std::find(groups.begin(), groups.end(), it.group) == groups.end())
            groups.push_back(it.group);
    for (auto& g : groups) {
        ImGui::SeparatorText(g.empty() ? page_label(page).c_str() : schema_.tr(g).c_str());
        if (ImGui::BeginTable(("t_" + page + g).c_str(), 3, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 17);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("reset", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6);
            for (auto& it : schema_.items)
                if (it.page == page && it.group == g && (advanced_ || !it.advanced)) ui_item(it);
            ImGui::EndTable();
        }
    }
    if (!advanced_) {
        int hidden = 0;
        for (auto& it : schema_.items) hidden += it.page == page && it.advanced;
        if (hidden) ImGui::TextColored(kDim, T("%d advanced settings hidden (\"Show advanced\")."), hidden);
    }
}

void App::ui_other_page() {
    ImGui::TextWrapped("%s", T("Options in gp4ultimate.ini that this configurator does not know (for example, from a newer version "
                               "of the mod). They are kept as they are and can be edited as text."));
    bool any = false;
    for (auto& sec : doc_.sections()) {
        if (schema_.managed(sec)) continue;
        std::vector<std::pair<std::string, std::string>> unknown;
        for (auto& kv : doc_.entries(sec))
            if (!schema_.find(sec, kv.first) && !(sec == "general" && kv.first == "language"))   // the top bar's selector
                unknown.push_back(kv);
        if (unknown.empty()) continue;
        any = true;
        ImGui::SeparatorText(("[" + sec + "]").c_str());
        for (auto& kv : unknown) {
            std::string v = kv.second;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 20);
            if (ImGui::InputText((kv.first + "##" + sec).c_str(), &v)) doc_.set(sec, kv.first, v);
            std::string c = doc_.comment(sec, kv.first);
            if (!c.empty()) { ImGui::SameLine(); ImGui::TextColored(kDim, "%s", c.c_str()); }
        }
    }
    if (!any) ImGui::TextColored(kDim, "%s", T("None: every option in the file is described."));
    ImGui::Spacing();
    ImGui::TextColored(kDim, T("Option descriptions: %s, version %d, %zu options."), T(schema_origin_).c_str(),
                       schema_.version, schema_.items.size());
    for (auto& w : schema_warnings_) ImGui::TextColored(kAmber, "%s", w.c_str());
}

void App::ui_status_bar() {
    ImGui::Separator();
    if (!map_error_.empty()) {
        ImGui::TextColored(kRed, T("Configuration error: %s"), explain(map_error_).c_str());
    } else if (!status_.empty() && now_ - status_time_ < 12.0) {
        ImGui::TextColored(status_error_ ? kAmber : kGreen, "%s", status_.c_str());
    } else if (!map_warnings_.empty()) {
        ImGui::TextColored(kAmber, T("Warning: %s"), explain(map_warnings_[0]).c_str());
    } else {
        ImGui::TextColored(kDim, "%s", T("Ready. Ctrl+S saves. Hover over a setting to see what it does."));
    }
}

}  // namespace gp4cfg

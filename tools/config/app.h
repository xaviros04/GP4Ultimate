// gp4ultimate-config: the configurator's state and UI (Dear ImGui). Everything it writes goes
// through IniDoc, so gp4ultimate.ini keeps its comments, order and unknown options; the generic
// pages come from gp4ultimate_schema.ini, so new mod options appear without a new configurator.
#pragma once
#include <string>
#include <vector>

#include "core/ini_doc.h"
#include "core/mapping.h"
#include "core/schema.h"
#include "input.h"

namespace gp4cfg {

struct Options {
    std::string ini_path;      // default: gp4ultimate.ini next to the exe
    std::string dinput_dll;    // default: Windows' dinput8.dll
    std::string start_page;    // tab to open first
    bool advanced = false;
    std::string screenshot;    // BMP path: render, save, exit (tests/docs)
    int frames = 20;
    int width = 1180, height = 820;
};

class App {
public:
    void init(HWND hwnd, const Options& o, const std::string& embedded_schema);
    void frame(double now);
    bool dirty() const { return doc_.text() != saved_; }
    bool save();
    void shutdown() { in_.shutdown(); }

    // Hooks for --selftest (scripted end-to-end run against the mock DirectInput).
    gp4u::IniDoc& doc() { return doc_; }
    Input& input() { return in_; }
    bool detecting() const { return det_kind_ != DetNone; }
    void test_detect_axis(int target) { start_detect(DetAxis, target); }
    void test_detect_button(int vb) { start_detect(DetButton, vb); }
    std::string test_alias_for(int ctrl) { return alias_for(ctrl, true); }
    const std::string& status() const { return status_; }
    const std::string& mapping_error() const { return map_error_; }
    const gp4u::Schema& schema() const { return schema_; }
    const std::string& embedded_schema() const { return embedded_schema_; }

    // UI language ("en" / "es"), from [general] language (auto follows Windows).
    const std::string& language() const { return lang_; }
    void set_language(const std::string& code);   // writes general.language; applied next frame

private:
    struct AliasInfo {
        std::string alias;
        gp4u::DeviceSelector sel;
        bool sel_ok = false;
        int ctrl = -1;   // connected controller it resolves to (DLL rule: first free match)
    };
    std::vector<AliasInfo> resolve_aliases() const;
    std::string alias_for(int ctrl, bool create);
    int ctrl_for_alias(const std::string& alias) const;
    std::string ctrl_label(int ctrl) const;

    void apply_language(const std::string& lang);
    void load_schema();
    std::string page_label(const std::string& page) const;

    void ui_top_bar();
    void ui_devices_panel();
    void ui_controls_page();
    void ui_axis_row(int target, const char* role, const char* how);
    void ui_button_row(int vbutton, const char* role);
    void ui_pov_row(int vpov);
    void ui_ffb_device();
    void ui_schema_page(const std::string& page);
    void ui_other_page();
    bool ui_item(const gp4u::SchemaItem& it);
    void ui_status_bar();
    void start_detect(int kind, int target);
    void update_detect();
    void set_status(const std::string& s, bool error = false);
    void clear_assignments();
    void set_axis(int target, const std::string& value);

    Options opt_;
    HWND hwnd_ = nullptr;
    std::string ini_path_, saved_, exe_dir_, embedded_schema_, lang_;
    gp4u::IniDoc doc_;
    gp4u::Schema schema_;
    std::string schema_origin_;
    std::vector<std::string> schema_warnings_;
    Input in_;
    double now_ = 0;
    bool advanced_ = false;
    bool first_frame_ = true;
    std::string status_;
    bool status_error_ = false;
    double status_time_ = -100;
    // validation of the current document (same parser as the DLL)
    std::string map_error_;
    std::vector<std::string> map_warnings_;

    enum DetectKind { DetNone, DetAxis, DetButton, DetPov };
    int det_kind_ = DetNone, det_target_ = 0;
    double det_start_ = 0;
    std::vector<std::vector<double>> det_axes_;
    std::vector<std::vector<bool>> det_buttons_;
    std::vector<std::vector<long>> det_povs_;
};

}  // namespace gp4cfg

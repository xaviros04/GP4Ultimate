// gp4ultimate-config --selftest: drives the real configurator (UI code included) against the mock
// DirectInput of the e2e tests (tests/win/mock_dinput.cpp): first run without gp4ultimate.ini,
// "Detectar" for the steering, a pedal and a button, the force-feedback sign test in both
// directions, and saving. Checks what lands in gp4ultimate.ini with the DLL's own parser.
// Then switches the UI to Spanish and back through the language setting, and checks the
// translation table itself.
#include "selftest.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "core/ini.h"
#include "core/mapping.h"
#include "i18n.h"

using namespace gp4u;

namespace gp4cfg {

namespace {
typedef void (*SetAxisFn)(int, int, LONG);
typedef void (*SetButtonFn)(int, int, int);
typedef LONG (*GetLFn)();
SetAxisFn set_axis;
SetButtonFn set_button;
GetLFn get_force;

// The printf conversions of a format string, in order ("%d%s", "%%" skipped). The space flag
// is not accepted on purpose: "100 % sin" is prose, not a conversion.
std::string conversions(const char* s) {
    std::string out;
    for (const char* p = s; *p; ++p) {
        if (*p != '%') continue;
        if (p[1] == '%') { ++p; continue; }
        const char* q = p + 1;
        while (*q && std::strchr("-+0#", *q)) ++q;
        while (*q && (std::isdigit((unsigned char)*q) || *q == '.' || *q == '*')) ++q;
        while (*q && std::strchr("hlzjtL", *q)) ++q;
        if (*q && std::strchr("diouxXfFeEgGaAcsp", *q)) {
            out.append(p, q + 1);
            out += ' ';
            p = q;
        }
    }
    return out;
}
}  // namespace

void SelfTest::check(bool ok, const std::string& what) {
    if (ok) ++pass_;
    else { ++fail_; log_ += "FAIL: " + what + "\n"; }
}

bool SelfTest::tick(App& app, double now) {
    if (step_ == 0) {
        HMODULE m = GetModuleHandleA("mock_dinput.dll");
        set_axis = m ? (SetAxisFn)GetProcAddress(m, "mock_set_axis") : nullptr;
        set_button = m ? (SetButtonFn)GetProcAddress(m, "mock_set_button") : nullptr;
        get_force = m ? (GetLFn)GetProcAddress(m, "mock_force") : nullptr;
        check(set_axis && set_button && get_force, "mock_dinput.dll loaded (run with --dinput mock_dinput.dll)");
        if (!set_axis || !set_button || !get_force) return finish();
    }
    if (wait_ > 0) { --wait_; return false; }
    auto& doc = app.doc();
    switch (step_++) {
        case 0:
            check(doc.entries("devices").empty() && doc.entries("axes").empty() && doc.sections().empty(),
                  "first run: a short new file, nothing assigned");
            check(app.status() == T("No gp4ultimate.ini yet: new configuration (created when you save)."),
                  "first run announced: " + app.status());
            check(app.language() == current_language(), "UI language set: " + app.language());
            check(app.input().controllers().size() == 3, "3 mock controllers");
            set_axis(0, 0, 32768);   // wheel centred
            set_axis(1, 1, 65535);   // throttle at rest reads high (needs invert)
            wait_ = 3;
            break;
        case 1: app.test_detect_axis(AX_X); wait_ = 2; break;
        case 2: set_axis(0, 0, 52000); wait_ = 3; break;   // turn right
        case 3: {
            std::string x = doc.get("axes", "X");
            wheel_ = x.substr(0, x.find('.'));
            check(!app.detecting(), "steering detected");
            check(x == wheel_ + ".X", "steering binding '" + x + "'");
            check(doc.get("devices", wheel_) == "vidpid:046D:C24F", "wheel alias by vid:pid: " + doc.get("devices", wheel_));
            check(doc.comment("devices", wheel_) == "Mock Wheel", "alias commented with the product name");
            set_axis(0, 0, 32768);
            app.test_detect_axis(AX_Y);
            wait_ = 2;
            break;
        }
        case 4: set_axis(1, 1, 0); wait_ = 3; break;   // press the throttle (value falls)
        case 5: {
            std::string y = doc.get("axes", "Y");
            check(y.find(".Y invert") != std::string::npos && y.rfind(wheel_ + ".", 0) != 0, "throttle binding '" + y + "'");
            set_axis(1, 1, 65535);
            app.test_detect_button(0);
            wait_ = 2;
            break;
        }
        case 6: set_button(0, 5, 1); wait_ = 3; break;
        case 7:
            check(doc.get("buttons", "0") == wheel_ + ".b5", "button 0 = wheel b5: " + doc.get("buttons", "0"));
            set_button(0, 5, 0);
            doc.set("virtual", "ffb", wheel_);
            physics_ = +1;   // a standard wheel: positive force pushes towards -X
            app.input().start_ffb_test(0, now);
            break;
        case 8:
        case 10:
            // Move the mock wheel with the force until the test finishes.
            set_axis(0, 0, (LONG)(32768 - physics_ * get_force() * 3));
            if (app.input().ffb_test().state != FfbTest::Idle) --step_;
            break;
        case 9:
            check(doc.get("ffb", "invert", "0") != "1", "standard wheel: invert stays off");
            check(app.status() == T("Correct: a positive force turns the wheel left (the standard). Invert = no."),
                  "sign test message: " + app.status());
            physics_ = -1;   // a wheel that does the opposite
            set_axis(0, 0, 32768);
            app.input().start_ffb_test(0, now);
            break;
        case 11: {
            check(doc.get("ffb", "invert") == "1", "opposite wheel: invert = 1");
            set_axis(0, 0, 32768);
            check(app.mapping_error().empty(), "mapping valid: " + app.mapping_error());
            check(app.save(), "save");
            std::ifstream f(ini_path_, std::ios::binary);
            std::stringstream ss;
            ss << f.rdbuf();
            std::string text = ss.str();
            Ini ini;
            ini.load_string(text);
            check(text.find("Edited with gp4ultimate-config.exe") != std::string::npos, "header comment kept");
            check(ini.get("axes", "Y").size() && text.find("invert ; throttle") != std::string::npos, "role comment on new axis lines");
            check(ini.get("ffb", "master_gain").empty(), "options never touched stay out (follow the defaults)");
            MappingConfig mc;
            std::string err;
            check(parse_mapping(ini, mc, err), "saved file parses with the DLL's parser: " + err);
            check(mc.ffb_device == wheel_ && mc.axes.size() == 2 && mc.buttons.size() == 1, "saved mapping complete");
            check(ini.get_bool("ffb", "invert", false), "saved invert");
            check(text.find("[general]") == std::string::npos, "language not written until chosen");
            app.set_language("es");   // as the top bar's selector does; the next frames render in Spanish
            wait_ = 2;
            break;
        }
        case 12: {
            check(doc.get("general", "language") == "es", "selector writes general.language = es");
            check(app.language() == "es" && current_language() == "es", "UI in Spanish: " + app.language());
            check(std::string(T("  Save  ")) == "  Guardar  " && std::string(T("Other")) == "Otros", "T() in Spanish");
            check(std::string(T("Assigned to@@pov")) == "Asignada a", "T() with context in Spanish");
            check(app.schema().lang == "es", "schema reloaded in Spanish");
            // A label of the converted schema, only once the schema carries Spanish labels.
            const std::string& emb = app.embedded_schema();
            Schema en;
            en.load(emb, nullptr, "en");
            int translated = 0;
            for (auto& it : app.schema().items) {
                const SchemaItem* e = en.find(it.section, it.key);
                translated += e && e->label != it.label;
            }
            check(emb.find("label_es") == std::string::npos || translated > 0,
                  "schema labels in Spanish: " + std::to_string(translated));
            if (emb.find("[@lang.es]") != std::string::npos)
                check(app.schema().tr("Effects") != "Effects", "page name translated: " + app.schema().tr("Effects"));
            // Every translation keeps the printf conversions of its English source.
            for (auto& tr : kTranslations)
                check(conversions(tr.en) == conversions(tr.es), std::string("same conversions in: ") + tr.en);
            app.set_language("en");
            wait_ = 2;
            break;
        }
        case 13:
            check(app.language() == "en" && std::string(T("  Save  ")) == "  Save  " && std::string(T("Assigned to@@pov")) == "Assigned to",
                  "back to English");
            check(app.schema().lang == "en", "schema reloaded in English");
            return finish();
    }
    if (now > 60) { check(false, "timeout at step " + std::to_string(step_)); return finish(); }
    return false;
}

bool SelfTest::finish() {
    char b[96];
    std::snprintf(b, sizeof b, "[gp4ultimate-config selftest] %d passed, %d failed\n", pass_, fail_);
    log_ += b;
    std::fputs(log_.c_str(), stdout);
    std::fflush(stdout);
    std::ofstream(result_path_) << log_;
    done_ = true;
    return true;
}

}  // namespace gp4cfg

// UI language of gp4ultimate-config.exe and gp4ultimate-devices.exe: English source strings,
// translated with T("...") through the Spanish table below. Anything without an entry stays
// in English, so a missing translation never hides text.
//
// The language comes from [general] language = auto | en | es in gp4ultimate.ini; auto (also
// when absent) follows the Windows UI language: Spanish for any Spanish locale, else English.
//
// printf-style entries must keep the same conversions, in the same order, in both languages
// (the configurator's --selftest checks the whole table). A key may end in "@@context" when the
// same English needs two Spanish renderings; the English side drops the "@@..." part.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace gp4cfg {

struct Translation {
    const char* en;
    const char* es;
};

// clang-format off
inline const Translation kTranslations[] = {
    // ---- gp4ultimate-config: top bar, status, files
    {"  Save  ", "  Guardar  "},
    {"  Saved  ", "  Guardado  "},
    {"Discard changes", "Descartar cambios"},
    {"Changes discarded.", "Cambios descartados."},
    {"Find controllers", "Buscar mandos"},
    {"%d controllers connected.", "%d mandos conectados."},
    {"Show advanced", "Mostrar avanzados"},
    {"  (unsaved)", "  (sin guardar)"},
    {"Language", "Idioma"},
    {"Could not save %s (read-only?)", "No se pudo guardar %s (¿solo lectura?)"},
    {"Saved. The previous version is in gp4ultimate.ini.bak. Restart GP4 to apply it.",
     "Guardado. Copia de la versión anterior en gp4ultimate.ini.bak. Reinicia GP4 para aplicarlo."},
    {"Saved. Restart GP4 to apply it.", "Guardado. Reinicia GP4 para aplicarlo."},
    {"Force feedback", "Force feedback"},
    {"Read gp4mod.ini (older version): saving creates gp4ultimate.ini.",
     "Leído gp4mod.ini (versión anterior): al guardar se crea gp4ultimate.ini."},
    {"No gp4ultimate.ini yet: new configuration (created when you save).",
     "No había gp4ultimate.ini: configuración nueva (se crea al guardar)."},
    {"built-in", "integrado"},
    {"built-in (the file is older)", "integrado (el archivo es más antiguo)"},
    {"invalid configuration", "configuración no válida"},
    {"Configuration error: %s", "Error en la configuración: %s"},
    {"Warning: %s", "Aviso: %s"},
    {"Ready. Ctrl+S saves. Hover over a setting to see what it does.",
     "Listo. Ctrl+S guarda. Pasa el ratón por un ajuste para ver su explicación."},
    {"There are unsaved changes in gp4ultimate.ini.\n\nSave them before quitting?",
     "Hay cambios sin guardar en gp4ultimate.ini.\n\n¿Guardarlos antes de salir?"},
    {"GP4 Ultimate configurator", "Configurador de GP4 Ultimate"},
    {"Could not start Direct3D 9.", "No se pudo iniciar Direct3D 9."},

    // ---- pages
    {"Controls", "Controles"},
    {"Other", "Otros"},

    // ---- devices panel
    {"Connected controllers", "Mandos conectados"},
    {"No controllers connected.", "No hay mandos conectados."},
    {"Connect the wheel base, the pedals and the rest, then press \"Find controllers\".",
     "Conecta la base, los pedales y el resto, y pulsa \"Buscar mandos\"."},
    {"POV %d: centred", "Cruceta %d: centrada"},
    {"POV %d: %ld°", "Cruceta %d: %ld°"},

    // ---- Controls page
    {"Assign each function with \"Detect\" and move the control. GP4 sees a single controller, "
     "\"GP4 Composite Wheel\": in its controller options assign steering = X axis, "
     "throttle = Y axis, brake = Rz axis and clutch = Z axis.",
     "Asigna cada función con \"Detectar\" y mueve el control. GP4 verá un único mando, "
     "\"GP4 Composite Wheel\": en sus opciones de controles asigna dirección = eje X, "
     "acelerador = eje Y, freno = eje Rz y embrague = eje Z."},
    {"Start over", "Empezar de cero"},
    {"Clears every assignment (for example, if this gp4ultimate.ini comes from another PC).",
     "Borra todas las asignaciones (por ejemplo, si este gp4ultimate.ini viene de otro PC)."},
    {"Clear every controller assignment?", "¿Borrar todas las asignaciones de mandos?"},
    {"Yes, clear", "Sí, borrar"},
    {"Cancel", "Cancelar"},
    {"Assignments cleared. Assign your controllers with \"Detect\" and save.",
     "Asignaciones borradas. Asigna tus mandos con \"Detectar\" y guarda."},
    {"Axes", "Ejes"},
    {"Function", "Función"},
    {"Controller and axis", "Mando y eje"},
    {"Invert", "Invertir"},
    {"Tuning", "Ajuste"},
    {"Output", "Resultado"},
    {"Steering", "Dirección"},
    {"Turn the wheel to the RIGHT (about 90°)", "Gira el volante a la DERECHA (unos 90°)"},
    {"Throttle", "Acelerador"},
    {"Press the THROTTLE all the way", "Pisa el ACELERADOR a fondo"},
    {"Brake", "Freno"},
    {"Press the BRAKE all the way", "Pisa el FRENO a fondo"},
    {"Clutch", "Embrague"},
    {"Press the CLUTCH all the way", "Pisa el EMBRAGUE a fondo"},
    {"Extra axis RX", "Eje extra RX"},
    {"Extra axis RY", "Eje extra RY"},
    {"Slider 1", "Deslizador 1"},
    {"Slider 2", "Deslizador 2"},
    {"Move the axis", "Mueve el eje"},
    {"Buttons", "Botones"},
    {"GP4 only reads 8 buttons. Give them a function in GP4's controller options.",
     "GP4 solo lee 8 botones. Asígnales función en las opciones de controles de GP4."},
    {"GP4 button", "Botón de GP4"},
    {"Assigned to", "Asignado a"},
    {"Assigned to@@pov", "Asignada a"},
    {"Note", "Nota"},
    {"1 (Fire 1, accept)", "1 (Fire 1, aceptar)"},
    {"POV hats", "Crucetas (POV)"},
    {"GP4 POV", "Cruceta de GP4"},
    {"Controller aliases ([devices])", "Alias de los mandos ([devices])"},
    {"Created when you assign. vidpid:VVVV:PPPP identifies the model; guid:{...} one particular unit.",
     "Se crean solos al asignar. vidpid:VVVV:PPPP identifica el modelo; guid:{...} un mando concreto."},
    {"invalid selector", "selector no válido"},
    {"not connected", "no conectado"},
    {"Remove", "Quitar"},
    {"Automatic buttons (alias, ...)", "Botones automáticos (alias, ...)"},
    {"Appends, in order, every button of those controllers after the assigned ones. GP4 only reads the first 8.",
     "Añade en orden todos los botones de esos mandos tras los asignados. GP4 solo lee los 8 primeros."},
    {"Detect", "Detectar"},
    {"Advanced mapping: edit it as text.", "Asignación avanzada: se edita como texto."},
    {"(unassigned)", "(sin asignar)"},
    {"%s (not connected)", "%s (no conectado)"},
    {"  ·  axis %s", "  ·  eje %s"},
    {"Tune", "Ajustar"},
    {"Deadzone", "Zona muerta"},
    {"Curve (gamma)", "Curva (gamma)"},
    {"1 = linear. Above 1 = gentler at the start (useful for the brake).",
     "1 = lineal. Más de 1 = más suave al principio (útil en el freno)."},
    {"Travel start", "Inicio del recorrido"},
    {"Travel end", "Fin del recorrido"},
    {"To reach 100% without pressing all the way, lower the travel end.",
     "Para llegar al 100 % sin pisar a fondo, baja el fin del recorrido."},
    {"Press the button you want...", "Pulsa el botón que quieras..."},
    {"%s  ·  button %d%s", "%s  ·  botón %d%s"},
    {"  (pressed)", "  (pulsado)"},
    {"e.g. shift up", "p. ej. subir marcha"},
    {"POV %d", "Cruceta %d"},
    {"Move the POV hat...", "Mueve la cruceta..."},
    {"%s  ·  POV %d", "%s  ·  cruceta %d"},
    {"Nothing detected in 15 s.", "No se detectó nada en 15 s."},
    {"Assigned: %s axis %s%s", "Asignado: %s eje %s%s"},
    {" (inverted)", " (invertido)"},
    {"Assigned: %s button %d", "Asignado: %s botón %d"},
    {"Assigned the POV hat of %s", "Asignada la cruceta de %s"},

    // ---- force feedback device
    {"(none)", "(ninguno)"},
    {"Force goes to", "La fuerza va a"},
    {"Testing... let go of the wheel", "Probando... suelta el volante"},
    {"Test direction", "Probar sentido"},
    {"Let go of the wheel: the base pushes one way and then the other (3 s), and 'Invert the force' is set "
     "if needed. GP4 must be closed.",
     "Suelta el volante: la base empuja a un lado y luego al otro (3 s) y se ajusta 'Invertir la fuerza' "
     "si hace falta. GP4 debe estar cerrado."},
    {"The base \"%s\" is not connected.", "La base \"%s\" no está conectada."},
    {"Note: the steering (X axis) does not come from this controller.",
     "Ojo: la dirección (eje X) no sale de este mando."},
    {"That controller has no force feedback.", "Ese mando no tiene force feedback."},
    {"Could not get exclusive use of the base. Is GP4 running?",
     "No se pudo usar la base en exclusiva. ¿Está GP4 abierto?"},
    {"The base did not accept the force effect.", "La base no aceptó el efecto de fuerza."},
    {"Correct: a positive force turns the wheel left (the standard). Invert = no.",
     "Correcto: la fuerza positiva gira el volante a la izquierda (estándar). Invertir = no."},
    {"Your base works the opposite way to the standard: 'Invert the force' has been ticked.",
     "Tu base va al revés del estándar: se ha marcado 'Invertir la fuerza'."},
    {"The wheel barely moved (were you holding it?). Let go of it and repeat the test.",
     "El volante apenas se movió (¿lo sujetabas?). Suéltalo y repite la prueba."},
    {"Could not open DirectInput (%s)", "No se pudo abrir DirectInput (%s)"},

    // ---- generic option pages
    {"Ctrl+click to type the value", "Ctrl+clic para escribir el valor"},
    {"not a number (e.g. 0x7A)", "no es un número (p. ej. 0x7A)"},
    {"Reset", "Restablecer"},
    {"Back to the mod's default: %s", "Volver al valor por defecto del mod: %s"},
    {"(empty)", "(vacío)"},
    {"default", "por defecto"},
    {"%d advanced settings hidden (\"Show advanced\").", "%d ajustes avanzados ocultos (\"Mostrar avanzados\")."},
    {"Options in gp4ultimate.ini that this configurator does not know (for example, from a newer version "
     "of the mod). They are kept as they are and can be edited as text.",
     "Opciones de gp4ultimate.ini que este configurador no conoce (por ejemplo, de una versión más nueva "
     "del mod). Se conservan tal cual y se pueden editar como texto."},
    {"None: every option in the file is described.", "Ninguna: todas las opciones del archivo están descritas."},
    {"Option descriptions: %s, version %d, %zu options.", "Descripción de opciones: %s, versión %d, %zu opciones."},

    // ---- gp4ultimate-devices (console: plain ASCII on the Spanish side, the console code
    //      page may not show accents)
    {"%u game controller(s)\n\n", "%u mando(s)\n\n"},
    {"    axes:", "    ejes:"},
    {"   buttons: %lu   povs: %lu%s\n\n", "   botones: %lu   crucetas: %lu%s\n\n"},
    {"Move each control and note which device/axis/button changes. Esc to stop.\n\n",
     "Mueve cada control y fijate en que mando/eje/boton cambia. Esc para parar.\n\n"},
    {"device %d has no force feedback\n", "el mando %d no tiene force feedback\n"},
    {"Let go of the wheel. Positive force (+30%) for 1.5 s...\n",
     "Suelta el volante. Fuerza positiva (+30%) durante 1.5 s...\n"},
    {"Negative force (-30%) for 1.5 s...\n", "Fuerza negativa (-30%) durante 1.5 s...\n"},
    {"X axis: start %.1f%%, after +force %.1f%%, after -force %.1f%%\n",
     "Eje X: inicio %.1f%%, tras la fuerza + %.1f%%, tras la fuerza - %.1f%%\n"},
    {"Positive force moves X down (DirectInput standard, as GP4 expects): keep [ffb] invert = 0.\n",
     "La fuerza positiva baja X (estandar de DirectInput, lo que espera GP4): deja [ffb] invert = 0.\n"},
    {"Positive force moves X up (opposite to DirectInput): set [ffb] invert = 1.\n",
     "La fuerza positiva sube X (al reves que DirectInput): pon [ffb] invert = 1.\n"},
    {"The wheel barely moved (held, or too little force): watch it and set invert by eye -\n"
     "positive force should turn it the way that DECREASES X in `gp4ultimate-devices watch`.\n",
     "El volante apenas se movio (sujeto, o poca fuerza): miralo y ajusta invert a ojo -\n"
     "la fuerza positiva debe girarlo hacia donde BAJA X en `gp4ultimate-devices watch`.\n"},
    {"\n(Saved to %s)\n", "\n(Guardado en %s)\n"},
    {"\n=== gp4ultimate-devices ===\n"
     "  1  List devices (saved to gp4ultimate-devices.txt)\n"
     "  2  Live axes and buttons (Esc to go back)\n"
     "  3  Suggest a configuration (saved to gp4ultimate_suggested.ini)\n"
     "  4  Test the force feedback direction\n"
     "  0  Quit\n"
     "Option: ",
     "\n=== gp4ultimate-devices ===\n"
     "  1  Listar dispositivos (se guarda en gp4ultimate-devices.txt)\n"
     "  2  Ver ejes y botones en vivo (Esc para volver)\n"
     "  3  Proponer configuracion (se guarda en gp4ultimate_suggested.ini)\n"
     "  4  Probar el sentido del force feedback\n"
     "  0  Salir\n"
     "Opcion: "},
    {"Number of the force feedback wheel: ", "Numero del volante con force feedback: "},
    {"\nPress a key to quit...", "\nPulsa una tecla para salir..."},
    {"No connected controller found.\n", "No se ha encontrado ningun mando conectado.\n"},
};
// clang-format on

// Current language: "en" or "es".
inline std::string& current_language() {
    static std::string lang = "en";
    return lang;
}

inline void set_current_language(const std::string& lang) { current_language() = lang == "es" ? "es" : "en"; }

// `setting` is the value of [general] language. Returns "en" or "es".
inline std::string resolve_language(std::string setting) {
    for (auto& c : setting) c = (char)std::tolower((unsigned char)c);
    while (!setting.empty() && (setting.back() == ' ' || setting.back() == '\t')) setting.pop_back();
    while (!setting.empty() && (setting[0] == ' ' || setting[0] == '\t')) setting.erase(0, 1);
    if (setting == "en" || setting == "es") return setting;
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_SPANISH ? "es" : "en";   // auto, empty, unknown
}

// [general] language of an ini file, read with Windows' own reader (enough for one value;
// the inline "; comment" is dropped by hand because GetPrivateProfileString keeps it).
inline std::string ini_language(const std::string& path) {
    char full[MAX_PATH], v[64] = {};
    if (!GetFullPathNameA(path.c_str(), MAX_PATH, full, nullptr)) return "auto";
    GetPrivateProfileStringA("general", "language", "auto", v, sizeof v, full);
    std::string s = v;
    size_t c = s.find_first_of(";#");
    return c == std::string::npos ? s : s.substr(0, c);
}

// The translation of `en` in the current language (the pointer stays valid).
inline const char* T(const char* en) {
    struct Tables {
        std::unordered_map<std::string, const char*> es;
        std::unordered_map<std::string, std::string> en_ctx;   // "x@@ctx" -> "x"
    };
    static const Tables t = [] {
        Tables r;
        for (auto& e : kTranslations) {
            r.es[e.en] = e.es;
            if (const char* at = std::strstr(e.en, "@@")) r.en_ctx[e.en] = std::string(e.en, at);
        }
        return r;
    }();
    if (current_language() == "es") {
        auto it = t.es.find(en);
        if (it != t.es.end()) return it->second;
    }
    auto it = t.en_ctx.find(en);
    return it != t.en_ctx.end() ? it->second.c_str() : en;
}
inline std::string T(const std::string& en) { return T(en.c_str()); }

// printf into a std::string (for translated format strings).
inline std::string strf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
}

}  // namespace gp4cfg

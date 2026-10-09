#!/usr/bin/env bash
# Runs every test that can run on this machine.
#   native core tests: always
#   Windows build + end-to-end proxy tests: when an i686 MinGW compiler is on PATH,
#   executed under Wine when not on Windows (WINE=/path/to/wine to override).
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
out="${BUILD_DIR:-$here/build}"

cmake -S "$here" -B "$out/native" -DGP4U_TESTS_ONLY=ON >/dev/null
cmake --build "$out/native" -j >/dev/null
"$out/native/test_core"
"$out/native/test_schema" "$here"

if command -v i686-w64-mingw32-g++ >/dev/null; then
    cmake -S "$here" -B "$out/win32" -DCMAKE_TOOLCHAIN_FILE="$here/cmake/i686-mingw.cmake" -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$out/win32" -j >/dev/null
    run() { if [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]]; then "$@"; else WINEDEBUG=-all WINEDLLOVERRIDES="dinput8,d3d8=n,b" "${WINE:-wine}" "$@"; fi; }
    gp4exe="${GP4_EXE:-}"   # optional: path to your GP4.exe for the differential tests (never committed)
    gp4arg=()
    if [[ -n "$gp4exe" && -f "$gp4exe" ]]; then
        if [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]]; then gp4arg=("$gp4exe")
        else gp4arg=("Z:$(cd "$(dirname "$gp4exe")" && pwd | tr / '\\')\\GP4.exe"); fi
    fi
    (cd "$out/win32/e2e" && run ./test_cockpit.exe "${gp4arg[@]}")
    (cd "$out/win32/e2e" && run ./test_proxy.exe telemetry && run ./test_proxy.exe game \
        && run ./test_proxy.exe gp4 && run ./test_proxy.exe aligning && run ./test_net.exe \
        && rm -f gp4ultimate_d3d.log && run ./test_d3d8.exe fix && rm -f gp4ultimate_d3d.log && run ./test_d3d8.exe nofix \
        && rm -f gp4ultimate_d3d.log && run ./test_d3d8.exe fullscreen \
        && rm -f gp4ultimate_d3d.log && run ./test_d3d8.exe fullscreen_plain)
    # Window mode needs a desktop: on Windows the real one; elsewhere a 5760x1080 Xvfb
    # screen, like an NVIDIA Surround desktop, so the centring is tested for real.
    if [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]]; then
        (cd "$out/win32/e2e" && rm -f gp4ultimate_d3d.log && ./test_d3d8.exe window)
    elif command -v xvfb-run >/dev/null; then
        # A wineserver left by the headless tests has no display: start a fresh one.
        "${WINESERVER:-wineserver}" -k 2>/dev/null || true
        (cd "$out/win32/e2e" && rm -f gp4ultimate_d3d.log && WINEDEBUG=-all WINEDLLOVERRIDES="d3d8=n,b" \
            xvfb-run -a -s "-screen 0 5760x1080x24" "${WINE:-wine}" ./test_d3d8.exe window 2>&1 | grep -v "X connection"; \
            exit "${PIPESTATUS[0]}")
    else
        echo "(no xvfb-run: skipped the window-mode test)"
    fi
    # Configurator: scripted run (detect steering/pedal/button, FFB sign test both ways, save)
    # against the mock DirectInput, plus a screenshot of every page. Needs a desktop too.
    cfg() { (cd "$out/win32/e2e" && cp ../gp4ultimate-config.exe . && rm -f selftest.ini* && "$@"); }
    if [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]]; then
        cfg ./gp4ultimate-config.exe --dinput mock_dinput.dll --ini selftest.ini --selftest
        cfg cat selftest.ini.selftest.txt
    elif command -v xvfb-run >/dev/null; then
        cfg sh -c 'WINEDEBUG=-all xvfb-run -a -s "-screen 0 1600x1000x24" "${WINE:-wine}" ./gp4ultimate-config.exe --dinput mock_dinput.dll --ini selftest.ini --selftest 2>&1 | grep -v "X connection"; exit "$(grep -c FAIL selftest.ini.selftest.txt)"'
    fi
    echo "artifacts: $out/win32/dinput8.dll  $out/win32/d3d8.dll  $out/win32/gp4ultimate-devices.exe"
else
    echo "(no i686-w64-mingw32-g++: skipped the Windows build and end-to-end tests)"
fi

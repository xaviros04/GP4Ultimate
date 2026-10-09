#!/usr/bin/env bash
# Builds the release zip: dist/GP4Ultimate-<version>.zip, the files a player copies next
# to GP4.exe. Usage: tools/package.sh [build dir]   (default: build/win32 from run_tests.sh)
#   An optional gp4ultimate.ini can be added with GP4U_INI=/path/to/gp4ultimate.ini.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
bin="${1:-$root/build/win32}"
ver="$(head -n1 "$root/VERSION")"
out="$root/dist/GP4Ultimate-$ver"
rm -rf "$out" && mkdir -p "$out"
for f in dinput8.dll d3d8.dll gp4ultimate-config.exe gp4ultimate-devices.exe; do
    [[ -f "$bin/$f" ]] || { echo "missing $bin/$f (build first: tests/run_tests.sh)" >&2; exit 1; }
    cp "$bin/$f" "$out/"
done
cp "$root/config/gp4ultimate_reference.ini" "$out/"
cp "$root/README.md" "$out/README.md"
cp "$root/docs/GUIDE.md" "$out/GUIDE.md"
cp "$root/docs/GUIA.md" "$out/GUIA.md"
cp "$root/LICENSE" "$out/LICENSE.txt"
cp "$root/THIRD_PARTY_NOTICES.md" "$out/THIRD_PARTY_NOTICES.md"
if [[ -n "${GP4U_INI:-}" ]]; then cp "$GP4U_INI" "$out/gp4ultimate.ini"; fi
(cd "$out" && rm -f "../GP4Ultimate-$ver${GP4U_SUFFIX:-}.zip" && zip -q -X "../GP4Ultimate-$ver${GP4U_SUFFIX:-}.zip" *)
echo "dist/GP4Ultimate-$ver${GP4U_SUFFIX:-}.zip"

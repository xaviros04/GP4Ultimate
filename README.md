# GP4 Ultimate

A modernisation mod for **Grand Prix 4** (Geoff Crammond / MicroProse, 2002): several controllers as one, telemetry-based force feedback, any resolution including ultrawide and triple screens, window mode and smooth online races.

[Guía en español](docs/GUIA.md) · [User guide](docs/GUIDE.md) · [Online server](docs/ONLINE.md) · [Technical notes (Spanish)](docs/GP4-FINDINGS.md)

- **Screenshots of all my settings to drive this mod:** https://imgur.com/gallery/gp4-ultimate-settings-cgtT0Oa
- **Important:** disable GP4's steering help (**F8** while driving), or the force feedback jumps and feels weird.
- **Discord community:** https://discord.gg/hFzEg5B4K

GP4 Ultimate is made of two proxy DLLs that sit next to `GP4.exe`. It never modifies `GP4.exe` or any game file on disk: remove the DLLs and the game is back to normal (see [Uninstalling](docs/GUIDE.md#uninstalling) if you used a custom resolution). It works with the original 2002 executable (the build GP4PP also targets); other builds are detected and the code patches are skipped.

## Features

**Controllers (`dinput8.dll`)**
- Wheel base, wheel rim, pedals, shifter and button boxes combined into **one virtual controller** ("GP4 Composite Wheel"). GP4 only handles 4 controllers and gives force feedback to the first one, so this is what makes modern rigs work.
- Per-axis calibration, dead zone, gamma (e.g. for load-cell brakes), inversion and combined pedals.
- Controllers GP4 should not touch (vJoy, handbrakes...) are hidden from it.

**Force feedback**
- Own 500 Hz engine fed by the car's state, using GP4's own force signal with its exact scale and sign (read from the game's code), smoothed so GP4's 30-60 Hz steps disappear.
- Damping tuned against the "ping-pong" on straights, or left to your wheel base software.
- Surface and grip: kerbs, gravel and grass rumble, lighter steering off track, front tyre scrub when understeering (idea from GP2FFB).
- Braking: pull when one front wheel brakes harder than the other, vibration on lock-up (per-wheel forces from GP4's physics).
- Gear change kick, impacts, track bumps from the suspension.

**Display (`d3d8.dll`)**
- **Any resolution**, or `auto` for the desktop's. GP4's menu only offers 4:3 and 5:4 sizes; its "1600x1200" entry becomes your resolution.
- **Ultrawide and triple screens**: on track the view keeps the height of a 16:9 screen and shows more to the sides (original GP4 cuts the top and bottom instead), with a camera zoom for triple screens; the menus are drawn in the centre at 16:9 with black bars instead of coming out broken.
- Full-screen mode list fixed for modern drivers when needed (GP4 only reads the first 256 modes, so large resolutions could be unreachable).
- Borderless **window mode**, centred (the centre screen with NVIDIA Surround / AMD Eyefinity).
- Hor+ aspect correction, field-of-view scale, frame capture for diagnostics.
- The cockpit steering wheel can turn like your real wheel (GP4 limits it to 157°).
- Chains to d3d8to9 or dgVoodoo2 if you use them.

**Online**
- Original GP4 makes every PC wait for the others' packet at every simulation step, so with any ping the race runs in slow motion (at 40 ms, at 40 % speed). GP4 Ultimate lets every PC run at full speed, keeps the race clocks in step and smooths the other cars' movement. Tested with 3 players over Radmin VPN.
- **Play over the internet without a VPN or DirectPlay**: a small GP4 Ultimate server (inside the host's GP4, on any PC, or on a Linux VPS) replaces DirectPlay. Only the server opens a port. See [docs/ONLINE.md](docs/ONLINE.md).

**Tools**
- `gp4ultimate-config.exe`: graphical configurator (English / Spanish). Detects your controllers, assigns axes and buttons by moving them, tests the force-feedback direction and explains every option.
- `gp4ultimate-devices.exe`: console tool that lists controllers and shows their axes live.

## Quick start

1. Download the latest release and copy everything into the folder of `GP4.exe`.
2. Run `gp4ultimate-config.exe` with GP4 closed: assign steering and pedals with **Detect**, choose your wheel base under **Force goes to** and press **Test direction**, then **Save**.
3. In GP4's controls menu choose **GP4 Composite Wheel**, calibrate it, and **enable force feedback** (GP4 must compute its force for the mod to use it).
4. Disable steering help with **F8** while driving.
5. For a non-4:3 resolution set it in **Cockpit & display** (or `auto`), then pick "1600x1200" once in GP4's graphics options. Triple screen: the whole size (e.g. `5760x1080`) and **Camera zoom** 2.0 ([guide](docs/GUIDE.md#ultrawide-and-triple-screens)).

The full walk-through, online setup and troubleshooting are in the [user guide](docs/GUIDE.md).

## Files

| File | What it is |
|---|---|
| `dinput8.dll` | Controllers and force feedback. Also the online improvements. |
| `d3d8.dll` | Resolution, window mode, aspect correction. |
| `gp4ultimate-config.exe` | Configurator. |
| `gp4ultimate-devices.exe` | Controller diagnostics (console). |
| `gp4ultimate-server.exe` | Online server, for whoever hosts one outside GP4 ([docs/ONLINE.md](docs/ONLINE.md)). |
| `gp4ultimate.ini` | Your settings (created by the configurator; plain text). An older `gp4mod.ini` is still read. |
| `gp4ultimate_reference.ini` | Every option with comments (reference; not read by the mod). |
| `gp4ultimate.log`, `gp4ultimate_d3d.log` | What the mod found and did. Attach them when reporting a problem. |

## Building

32-bit Windows binaries; C++17.

```bash
# Windows, Visual Studio
cmake -S . -B build -A Win32
cmake --build build --config Release
ctest --test-dir build -C Release

# Linux/macOS with an i686 MinGW cross compiler (+ Wine and Xvfb for the end-to-end tests)
tests/run_tests.sh
```

`tests/run_tests.sh` runs the native unit tests, then builds the DLLs and tools and runs the end-to-end tests under Wine against mock DirectInput and Direct3D 8 (and Wine's DirectPlay). With `GP4_EXE=/path/to/GP4.exe` it also checks the code patch sites against your own copy of the game and compares a reimplemented game function with the original, instruction by instruction. No game files are ever stored in the repository.

How it works, and the addresses it relies on, is documented in [docs/GP4-FINDINGS.md](docs/GP4-FINDINGS.md) (in Spanish).

## Contributing

Issues and pull requests are welcome. Please attach `gp4ultimate.log` (and `gp4ultimate_d3d.log` for display problems) to bug reports. Every option the DLLs read must be described in `config/gp4ultimate_schema.ini` with the same default, in English and Spanish; `test_schema` enforces it.

## Credits and licence

GP4 Ultimate is released under the [MIT licence](LICENSE). It builds on the work of others; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md):

- [decomp-grandprix4](https://github.com/leighrobertabbott/decomp-grandprix4) by Leigh Abbott (MIT): the reverse-engineering workspace this mod started in.
- [GP4PP](https://github.com/Oggo87/GP4PP) by Diego "Öggo" Noriega (MIT), with the car data layout by René "SDI" Smit and Paulo "Prblanco" Blanco.
- [GP2FFB](https://github.com/gplaps/GP2FFB) by gplaps (MIT): ideas for surface and braking effects.
- [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut (MIT): the configurator's interface.

Grand Prix 4 is a trademark of its respective owners. This project is not affiliated with or endorsed by them, and it does not include any part of the game: you need your own copy.

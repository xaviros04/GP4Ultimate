# GP4 Ultimate - user guide

[Versión en español](GUIA.md)

1. [Requirements](#requirements)
2. [Installing](#installing)
3. [Controllers](#controllers)
4. [Setting up GP4](#setting-up-gp4)
5. [Force feedback](#force-feedback)
6. [Resolution, window and screens](#resolution-window-and-screens)
7. [Online races](#online-races)
8. [Troubleshooting](#troubleshooting)
9. [Uninstalling](#uninstalling)

## Requirements

- Grand Prix 4 installed, with the standard `GP4.exe` (6,275,072 bytes, sha256 starting `2046f390`). With another executable the controllers and force feedback still work, but the code patches (resolution menu, cockpit wheel, online, braking forces) are switched off and the log says so.
- Windows 10 or 11.
- For online play: the Windows feature **DirectPlay** (Control Panel → Programs → Turn Windows features on or off → Legacy Components → DirectPlay).
- Optional: [d3d8to9](https://github.com/crosire/d3d8to9) or dgVoodoo2 if you already use them for GP4 (see [Chaining](#chaining-other-dlls)).

## Installing

1. Unzip the release into the folder that contains `GP4.exe`.
2. That's it: no installer, nothing is written outside that folder.

If the folder already has a `dinput8.dll` or `d3d8.dll` from another mod, read [Chaining](#chaining-other-dlls) before overwriting it.

## Controllers

Open `gp4ultimate-config.exe` **with GP4 closed** (GP4 takes the controllers for itself). The language follows Windows; change it with the selector in the top bar.

### Controls tab

GP4 sees a single controller, **GP4 Composite Wheel**, built from your devices.

1. Connect everything and press **Find controllers**. The left panel lists them; expand one to see its axes and buttons move.
2. For each function press **Detect** and move the control:
   - **Steering**: turn the wheel to the right (about 90°).
   - **Throttle**, **Brake**, **Clutch**: press each pedal all the way.
3. **Tune** an axis if needed: deadzone, **curve (gamma)** (above 1 is gentler at the start, good for load-cell brakes; set it to 1 if your pedal software already applies a curve), and travel start/end (lower the end to reach 100 % without pressing all the way).
4. **Buttons**: GP4 only reads 8. Detect the ones you want (button 1 is "accept" in GP4's menus), then give them a function in GP4's own controls menu.
5. **Save** (Ctrl+S). The previous file is kept as `gp4ultimate.ini.bak`.

If your `gp4ultimate.ini` comes from another PC, press **Start over** and assign your own controllers.

Controllers you do not assign (vJoy, a handbrake...) are hidden from GP4, which would otherwise open them exclusively for nothing.

## Setting up GP4

In GP4's controller options:

1. Choose **GP4 Composite Wheel** as the controller.
2. Assign steering = X axis, throttle = Y axis, brake = Rz axis, clutch = Z axis.
3. **Calibrate**: turn the wheel fully to both sides and press every pedal. GP4 calibrates the raw axes itself.
4. **Enable force feedback.** GP4 only computes its steering force when force feedback is on, and the mod uses that force. The FFB strength in GP4's menu scales the signal too (around 60 % is a good start).

## Force feedback

1. In the **Controls** tab, at the bottom: **Force goes to** = your wheel base. With the wheel free, press **Test direction**: the base pushes one way and then the other, and "Invert the force" is set if your base works the other way round.
2. In the **Force feedback** tab, **Your wheel's rotation**: the same degrees as in your wheel base software (default 360; e.g. 540 or 900).
3. **Overall strength**: with a direct-drive base start low (0.3-0.5) and go up.

### Damping

GP4 updates its force only 30-60 times per second, and at speed the wheel can oscillate on straights ("ping-pong"). The mod's damping grows with speed to prevent it.

- By default the general damping is left to your wheel base software (Simucube True Drive, Fanatec, Moza...): set some damping there. The mod only adds **High-speed damping** (0.15), the part that holds the centre on straights, which a base cannot do because it does not know the car's speed.
- If the wheel feels "fast and elastic", there is too little damping overall: raise it in the base, or set **Base damping** (around 0.03) and **Pit-lane damping** (around 0.04) here.

### Effects tab

Kerbs, gravel, grass, front tyre scrub (understeer warning), pull under braking, lock-up vibration, gear changes, impacts and track bumps. Each has its own strength; 0 switches it off. They are scaled by the overall strength, so if they feel weak, raise that first.

### Tuning with data

With **Force telemetry** on (Advanced tab) the mod writes `gp4ultimate_ffb.csv` with the car's state and every force component, a few MB per race. Useful for tuning or for reporting a problem.

## Resolution, window and screens

The **Cockpit & display** tab.

### Any resolution

GP4's graphics menu only offers 4:3 and 5:4 sizes up to 1600x1200.

1. Set **Game resolution** to your size, e.g. `1920x1080`, `2560x1080`, `3440x1440`, `5760x1080`, or `auto` (the desktop's).
2. Start GP4 and choose **1600x1200** once in its graphics options: that entry now means your resolution.

In full screen your monitor must support the size. Modern drivers list hundreds of modes and GP4 only reads the first 256, so big resolutions could be missing; when that happens the mod gives GP4 a shorter list with yours first. `gp4ultimate_d3d.log` says what it did, and if your monitor does not have the size it lists the closest ones. Window mode works with any size.

### Window mode

**Play in a window** opens GP4 as a borderless window of its resolution, centred on the monitor. With NVIDIA Surround / AMD Eyefinity it lands on the centre screen. Handy with a 1920x1080 window on a triple-screen desktop.

### Wide images

If the picture looks stretched sideways, turn on **Hor+ correction** (the log tells you when it applies). **Field of view scale** (Show advanced) above 1 widens the view.

### Cockpit steering wheel

GP4 turns the cockpit wheel at most 157°. **Cockpit wheel rotation** (default 360) makes it turn like yours: set your wheel's degrees, or 0 for the original. Calibrate the wheel in GP4 turning it fully both ways.

### Chaining other DLLs

If you use d3d8to9 or dgVoodoo2, rename its `d3d8.dll` (for example to `d3d8to9.dll`), put GP4 Ultimate's `d3d8.dll` in its place and set **Chained d3d8.dll** to the new name. The same works for another `dinput8.dll` (Advanced → Chained dinput8.dll, with Show advanced).

## Online races

GP4 plays online with DirectPlay. Over the internet, a virtual LAN such as **Radmin VPN** or ZeroTier works well.

### What GP4 Ultimate changes

Original GP4 makes every PC wait, at every simulation step, for the packets of the others. With any ping the race runs in slow motion: at 40 ms of ping, at 40 % speed. With GP4 Ultimate:

- every PC runs at its own full frame rate and uses the latest data from the others (**Race without waiting**);
- the race clocks are kept in step (a PC that gets ahead waits a few milliseconds, one that falls behind runs extra steps);
- other players' cars move smoothly (**Smooth the other players' cars**, an advanced option that is on by default, keeps one packet in reserve: about one frame, 17 ms at 60 fps, of extra delay).

Everyone should use the **same version** of GP4 Ultimate. Friends who only want to play online can use the defaults: the configurator is only needed for controllers.

### Step by step

1. Everyone joins the same Radmin VPN network.
2. On every PC: DirectPlay enabled, and the Windows firewall allowing `GP4.exe` and `C:\Windows\SysWOW64\dplaysvr.exe` (Control Panel → Windows Defender Firewall → Allow an app). Set the Radmin network as **Private** (Settings → Network → the Radmin VPN adapter → Network profile).
3. One player creates the game (TCP/IP), the others find it in the list and join.

### Good to know

- The race is run by one PC (clock and AI cars). GP4 chooses it, not necessarily the one that created the game.
- Each PC shows its own on-screen lap times, so they can differ by a few hundredths between PCs. The final results agree.
- Other cars are shown about half a ping late. Up to ~100 ms of ping it is very playable.
- `gp4ultimate.log` writes a `netsync` line every second (steps, packets, clock difference, waits). Attach the logs of every player when reporting a problem.

### If someone cannot join

| Symptom | Cause | Fix |
|---|---|---|
| The game does not appear in the list | Discovery packets blocked, or not on the same network | Same Radmin network; firewall rules above |
| It appears, but joining hangs ~35 s and fails (`DPERR_TIMEOUT` in the log) | Firewall or antivirus blocking DirectPlay's connection | Allow `GP4.exe` and `dplaysvr.exe`, network as Private; if needed open TCP 47624 and TCP/UDP 2300-2400 inbound. Third-party antivirus firewalls need the same rules |
| GP4 says networking is not available | DirectPlay not installed | Enable the DirectPlay Windows feature |

## Troubleshooting

| Problem | What to do |
|---|---|
| GP4 does not see the wheel | Choose **GP4 Composite Wheel** in GP4's controls menu. Check `gp4ultimate.log`: it lists every controller found and which alias matched |
| No force feedback | Force feedback enabled in GP4's menu; the right base under **Force goes to**; **Overall strength** above 0 |
| The wheel pulls into the corners | Run **Test direction** again (or tick **Invert the force**) |
| Wheel oscillates on straights | Raise **High-speed damping** (0.15 → 0.2) |
| Resolution does not work | Choose "1600x1200" in GP4's graphics menu. Check `gp4ultimate_d3d.log`: if your monitor has no such mode it lists the closest ones; use one of them or **Play in a window** |
| Image stretched | **Hor+ correction** on |
| Something else | Attach `gp4ultimate.log` and `gp4ultimate_d3d.log` to an issue |

## Uninstalling

First, if you set a custom resolution, choose a normal size (e.g. 1600x1200 or 1024x768) in GP4's graphics options: GP4 saves the chosen size in `f1graphics.cfg`, and without the mod it would not find a widescreen size and would fall back to 640x480. (Or delete the two `Game 3D ... Resolution` lines in `f1graphics.cfg`.)

Then delete `dinput8.dll`, `d3d8.dll`, the `gp4ultimate*` files and the logs. If you had chained another DLL, give it back its original name.

# Dual Input mod for Prototype 2

Use **gyro aiming and a gamepad at the same time**, with native mouse-look feel and
gamepad button prompts.

Prototype 2 already supports an Xbox (XInput) pad, and Steam Input's virtual Xbox pad
works. The problem is gyro: Steam Input sends it as mouse, and in gamepad mode the game
either ignores the mouse, or (if you let it use it) locks keyboard and mouse and the pad
prompts disappear. Feeding gyro through the right stick instead means fighting the stick's
dead zone, response curve and acceleration. This mod fixes that:

- **Gyro is fed to the game's own mouse look**, straight to the camera, while the game
  keeps the **gamepad as the active device**, so gamepad button prompts/glyphs stay.
  The engine's mouse acceleration curve is removed (in memory only), so the gyro response is linear.
- **Smooth.** Frame-locked, distance-preserving smoothing evens out the uneven gyro-as-mouse
  packets without losing movement; fast flicks stay instant.
- **The right stick/trackpad wins.** Once you push it past a threshold, gyro is
  ignored until you let go, so big turns stay clean and gyro handles fine aim.
- **Press-start screen is protected.** Mouse input is blocked there, so gyro cannot make the
  game lock keyboard and mouse.
- **In menus** the mouse is removed (the cursor does not move) so you navigate with the pad.
- Everything else (left stick, buttons, triggers, vibration) passes through untouched.

Unofficial fan mod. Not affiliated with Radical Entertainment, Activision or Valve.
Use at your own risk.

## Requirements

- Prototype 2, **Steam version** (AppID 115320). Tested on the current Steam build
  (`prototype2engine.dll` SHA-256
  `C3D51D6355387963A63CBAC058381B8FD1CE39F51088FA50276A49285EE94174`).
- A controller with gyro, used through **Steam Input** (its virtual Xbox pad is what the game sees).

## Install

Download the latest ZIP from the [Releases](https://github.com/GyroGamesCom/Prototype2_DualInput/releases) page.

**Automatic:** unzip anywhere and run `install.bat`. It finds the game through
Steam (or asks for the folder), backs up any `xinput1_3.dll` it would replace, and
keeps your existing settings if you update.

**Manual:** copy the 2 files from the `mod` folder (`xinput1_3.dll`, `xinput_mouse.ini`)
next to `prototype2.exe`
(Steam: right-click the game, then Manage > Browse local files).

**Uninstall:** run `uninstall.bat`, or delete `xinput1_3.dll`, `xinput_mouse.ini` and
`xinput_mouse.log` (plus `xinput_trace.csv` / `xinput_native.csv` if you used `LogLevel=3`)
from the game folder.

At the **press-start screen, press a button on the gamepad**. If the game ever ends up in
keyboard mode, reconnect the controller (the game switches to the pad when it connects).

## Steam Input setup

In the game's controller configuration:

| Input              | Set to                                 |
|--------------------|----------------------------------------|
| Gyro               | **As Mouse** (any activation you like) |
| Right trackpad / stick | **Joystick** (right stick output)  |
| Everything else    | Normal gamepad layout                  |

Gyro speed is the product of three things:

- the **Steam Input gyro sensitivity**,
- the in-game **Mouse Sensitivity** setting,
- `NativeSensitivity` in `xinput_mouse.ini`.

Tip: keep the Steam gyro sensitivity moderate/high and lower the in-game Mouse Sensitivity.
That gives you finer resolution for small aim corrections.

If vertical gyro is inverted, change `InvertY` in the ini (or invert in Steam,
but not both, or they cancel out).

The right trackpad/stick keeps the game's normal stick feel. If you want it linear too
(no dead zone, no squared response, no pitch snap, no acceleration), set `StickLinear=1`.

## Settings (`xinput_mouse.ini`)

Every option is explained in the file. The file is re-read about once per second while the game
runs, so most values can be tuned live. The exceptions are `LinearMouse`, `StickLinear` and the
other engine-patch keys: they are applied once at startup and need a game restart.

| Key            | Default | Meaning |
|----------------|---------|---------|
| `MouseMode`    | 1   | 1 = native: gyro goes to the game's mouse look (recommended), 0 = legacy: mouse mixed into the right stick |
| `NativeSensitivity` | 1.0 | Mode 1: extra multiplier on the mouse counts given to the game |
| `NativeSmoothingMs` | 35 | Mode 1: smoothing time constant in ms (0 = off). Total distance is preserved, motion is only delayed by about this much |
| `NativeSmoothingMaxSpeed` | 1500 | Mode 1: counts/s. Full smoothing below this speed, none above 2x this speed. 0 = smooth at all speeds |
| `NativeFrameLocked` | 1 | Mode 1: release smoothed motion once per displayed frame (steadier) instead of by measured poll time |
| `NativeFrameMs` | 16.667 | Mode 1: nominal frame time for `NativeFrameLocked` (4-50; 16.667 = 60 fps) |
| `ModeKey`      | 0   | Virtual-key code that toggles `MouseMode` at runtime (e.g. 119 = F8) |
| `LinearMouse`  | 1   | Remove the engine's mouse acceleration curve (restart needed) |
| `StickLinear`  | 0   | Make the real right stick linear too (restart needed) |
| `Enabled`      | 1   | 1 = mod on, 0 = pass-through (also automatically off while no pad is connected) |
| `StickOverride`| 0.27 | Right-stick deflection above which gyro is ignored |
| `InvertY`      | 0   | 1 = invert vertical gyro (both modes) |
| `Sensitivity`  | 0.3 | `MouseMode=0`: gyro speed (1.0 = about 1000 counts/s gives a fully pushed stick) |
| `SensitivityY` | 1.0 | `MouseMode=0`: extra multiplier for vertical gyro |
| `Curve`        | 1.0 | `MouseMode=0`: 1.0 linear, below 1 boosts small moves, above 1 makes them finer |
| `DeadzoneComp` | 0.10 | `MouseMode=0`: pushes small gyro moves past the game's stick dead zone (0-0.3) |
| `SmoothingMode`| 1   | `MouseMode=0`: 1 = exponential smoothing per game poll, 0 = sliding window |
| `SmoothingMs`  | 20  | `MouseMode=0`: smoothing time constant / window in ms |
| `MenuMouse`    | 0   | 0 = mouse removed in menus (cursor stays put), 1 = mouse reaches the game in menus |
| `UsePauseFlag` | 1   | 1 = the engine's pause flag counts as "menu" too |
| `StripMouseOnStart` | 1 | 1 = remove all mouse data on the press-start screen so gyro cannot lock keyboard mode |
| `MouseButtons` | 0   | 1 = in gameplay: LMB = right trigger, RMB = left trigger, MMB = right bumper |
| `ToggleKey`, `SlowerKey`, `FasterKey` | 0 | Optional hotkeys (virtual-key codes, e.g. 80 = P): mod on/off, `Sensitivity` -10% / +10% (saved; `MouseMode=0` only) |
| `LogLevel`     | 1   | 0 = problems only, 1 = install details and state changes, 2 = + summary every ~5 s, 3 = + per-poll traces to `xinput_trace.csv` / `xinput_native.csv` |

## Troubleshooting

- **Nothing changes:** check that `xinput_mouse.log` appears next to `prototype2.exe` after
  launching. If not, the DLL is in the wrong folder.
- **The game switched to keyboard and mouse:** reconnect the controller. At the press-start
  screen, press a gamepad button, not a key or the mouse.
- **Gyro does nothing / log says a patch was skipped:** your game build differs from the tested
  one. The mod checks the original bytes before each memory patch and skips it (logged). If the
  mouse gate patch cannot apply, gyro falls back to `MouseMode=0` (mixed into the right stick).
- **Gyro too fast or too slow:** see Steam Input setup above (Steam gyro sensitivity, in-game
  Mouse Sensitivity, `NativeSensitivity`).
- **Camera feels laggy:** lower `NativeSmoothingMs` (0 turns smoothing off).
- **Antivirus warning:** proxy DLLs next to a game exe are a common false positive
  (ReShade and many fixes work the same way). The full source code is in `src`.
- **Conflict with another mod** that also uses `xinput1_3.dll`: the installer backs it
  up to `DualInput_backup` and restores it on uninstall. The two cannot run at the
  same time.

## HiRes (optional, separate)

A second, independent mod that unlocks resolutions above 1920x1200 (4K, 1440p, ultrawide)
in the options menu. It does not need, and does not touch, the Dual Input mod; install either
or both. Install with `install_hires.bat` (remove with `uninstall_hires.bat`), or copy
`winmm.dll` and `p2_hires.ini` from the `mod_hires` folder next to `prototype2.exe`.

- **Re-select your resolution in Options once.** The game saves a position in its resolution
  list, not the size, and the mod changes the list.
- **A resolution only appears if your display or driver offers it.** A 4K monitor does. On a
  1080p/1200p monitor enable NVIDIA DLDSR (NVIDIA Control Panel > Manage 3D settings >
  DSR - Factors > DL scaling 2.25x, then restart the game) or AMD VSR (Radeon Software >
  Display > Virtual Super Resolution). The game then renders at e.g. 2880x1620 and the driver
  scales it down to your screen, which gives a much cleaner image.
- **Full refresh rate.** On TVs the game used to pick the first refresh rate the display lists
  for a resolution (often 24 Hz at 4K, so vsync locked the game to 24 fps). The mod now uses
  the highest refresh rate the display offers for the chosen resolution, also when you change
  resolution in the menu. `[Display]` in `p2_hires.ini`: `RefreshRate=60` forces an exact rate,
  `MaxRefresh=60` caps it (e.g. on a 120 Hz TV). These need a game restart.
- **DLSS is not possible** (the game is DirectX 9 and has no motion vectors). DLDSR is the
  recommended alternative.
- The resolution list is in `p2_hires.ini` (exactly 14 entries, `Mode1`..`Mode14`, default
  1280x720 up to 3840x2160). `Auto=1` builds the list from the modes your display reports.
  Problems are explained in `p2_hires.log`.
- Technically: a proxy `winmm.dll` (all 193 exports forwarded to the real one) rewrites the
  game's hard-coded 14-entry resolution table in memory before the game starts, after checking
  the original bytes, and sets the fullscreen refresh rate in Direct3D 9 `CreateDevice`/`Reset`. Nothing is written to the game files. Build: `src\hires\build_hires.bat`
  (the export stubs are generated by `src\hires\gen_winmm_exports.py`).
- Conflict: another mod that also uses `winmm.dll` (the installer backs it up to
  `DualInput_backup\hires` and restores it on uninstall).

## How it works

One small proxy DLL, `xinput1_3.dll`, sits between the game and Windows. The engine
(`prototype2engine.dll`) imports XINPUT1_3 statically, so the DLL placed next to the exe is
loaded instead of the system one. It forwards everything to the real XInput and adds:

- **Gyro handling in `XInputGetState`**, and a filter on the game's DirectInput mouse data.
  Mouse counts are scaled, smoothed (distance-preserving, frame-locked) and then handed to
  the game; while the real right stick is pushed or the game is in a menu or on the
  press-start screen they are removed instead.
- **In-memory engine patches** (never written to disk): the game's gate that makes gamepad
  mode ignore the mouse is removed, so the native mouse-look path works while the pad stays the
  active device, and the engine's mouse acceleration curve is flattened (`LinearMouse`).
  Optionally the real stick is made linear (`StickLinear`).
- Before each patch the mod verifies the original bytes. If they do not match (different game
  build) it skips that patch and logs it.

The reverse-engineering notes (addresses, input path, camera look path) are in
[`re/FINDINGS.md`](re/FINDINGS.md).

Build: run `src\build.bat` (needs Visual Studio Build Tools, 32-bit MSVC). It copies the DLL
to `mod\` and removes the intermediate files.

## Contact

Bug reports, questions and permission requests: open an issue at
https://github.com/GyroGamesCom/Prototype2_DualInput/issues

## License

Free for personal use. Redistribution, re-uploading or any commercial use requires prior written
permission from GyroGamesCom, and GyroGamesCom must always be credited. Showing the mod in
videos and streams is fine with credit. See `LICENSE.txt` for the full terms.

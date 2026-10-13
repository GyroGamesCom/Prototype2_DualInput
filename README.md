# Dual Input for Prototype 2

Gyro aiming and gamepad at the same time. Gyro moves the camera like a real mouse
(linear, no stick curve or acceleration), and the game keeps showing gamepad buttons.

## What it does

- Gyro goes straight into the camera as native mouse look, with light smoothing.
- The gamepad stays the active device (gamepad prompts).
- The right stick/trackpad has priority: while you push it, gyro is ignored.
- Menus and the press-start screen are not affected.

## Install

1. Close the game. Run `install.bat`.
2. Steam Input (controller layout for the game):
   - Gyro: **As Mouse**
   - Right trackpad / right stick: **Joystick**
3. At the press-start screen, press a gamepad button (not the keyboard or mouse).

**Manual install:** copy `xinput1_3.dll` and `xinput_mouse.ini` from `mod` next to
`prototype2.exe`.

## Uninstall

Run `uninstall.bat`, or delete `xinput1_3.dll`, `xinput_mouse.ini` and `xinput_mouse.log`
from the game folder.

## Settings

`xinput_mouse.ini` next to the exe. Most values apply while the game runs.
Gyro speed = Steam gyro sensitivity x in-game Mouse Sensitivity x `NativeSensitivity`.

| Key | Default | |
|---|---|---|
| `NativeSensitivity` | 1.0 | Gyro speed |
| `InvertY` | 0 | 1 = flip vertical gyro |
| `NativeSmoothingMs` | 35 | Gyro smoothing in ms (0 = off) |
| `StickOverride` | 0.27 | How far the right stick must move to override gyro |
| `StickLinear` | 0 | 1 = make the right stick linear too (restart) |
| `Enabled` | 1 | 0 = mod off |

## HiRes (optional, separate mod)

Unlocks resolutions above 1920x1200 (4K, 1440p, ultrawide) and uses the full refresh rate.
Run `install_hires.bat` (remove: `uninstall_hires.bat`), then pick your resolution in
Options again once. 4K appears only if your display offers it (or with NVIDIA DLDSR / AMD VSR).
The list is in `p2_hires.ini`.

## Problems

- **No effect:** after starting the game, `xinput_mouse.log` must appear next to the exe.
- **Game switched to keyboard and mouse:** reconnect the controller.
- **Game updated, mod stopped working:** the mod skips any game code that changed.
- Tested on the Steam version.

Source code is in `src` (build with `src\build.bat`, MSVC x86). Notes: `re\FINDINGS.md`.

Unofficial fan mod, not affiliated with Radical Entertainment, Activision or Valve.
Use at your own risk. License: `LICENSE.txt`.

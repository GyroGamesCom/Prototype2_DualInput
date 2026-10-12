# Prototype 2 (Steam) – input system RE findings

Target: `prototype2engine.dll` (x86, MSVC9, ImageBase 0x10000000, no ASLR). RVA = VA - 0x10000000.
Static analysis (REA/Ghidra + capstone). [O] observed, [I] inference, [?] unknown.

## TL;DR
1. XInput is created unconditionally and polled every frame from `ControllerXInput::Update` (0x10761b90). No SKU / pc / cmdline gate. [O]
2. Pad → player attachment depends on GameDB int **`SettingsDefaultController`** (SDC): 0=Keyboard, 1=Mouse, 2/3=SmartJoy, 4=XBCD, **5=XInput XBox360 Controller**, 6-10=Logitech.
   PCM listener (0x10708510): `PCM+0x34 = (SDC <= 2) ? 0xFE00 (KB only) : -1 (all pads)`. [O]
   - Default value 5 (0x100f3e80). Boot (0x100f2d90) sets 5 if an XBOX pad is present.
   - Connect callback 0x106cd3e0: name must match whitelist 0x10cf21c8 (11 entries) else device ignored; idx 4/5 → SetInt(SDC, idx).
   - Disconnect 0x106cd5a0: no XInput/XBCD left and SDC in {4,5} → SDC = 0.
3. Keyboard always attached, mouse if `InputSystem+0x80`, pads on top → KB+M and pad work simultaneously; no last-used-device switching. [O]/[I]
4. Glyphs depend only on SDC (cache `GameDBHelper+0xC` = SDC < 2 = IsKeyboardAndMouse). [O]
   - HUD icon 0x10371d20, prompt 0x10367e20, `<BUTTON=n>` resolver 0x106cfc40 (SDC 2..5 Xbox table 0x10cf3a60, >=6 Logitech 0x10cf3b20).
5. XInput imported by ordinal #2 GetState (IAT 0x109e548c), #4 GetCapabilities (0x109e5490), #3 SetState (0x109e5494). No XInputEnable. [O]
6. Mouse/keyboard: DirectInput8 via `CoCreateInstance(CLSID_DirectInput8)` in 0x106ccec0 → a game-folder dinput8.dll proxy is NOT loaded. No raw input, no GetCursorPos. [O]
7. Pad inputs zeroed while window unfocused (WndProc 0x10760b00: WM_SETFOCUS/KILLFOCUS/ACTIVATEAPP → sys+0xEC). [O]
8. Non-whitelisted DirectInput pads (DS4/Switch/generic) are ignored. XInput devices are filtered out of DI by WMI IG_ check. [O]

## XInput objects
- `ControllerSystemXInput` singleton ptr **0xE41570** (size 0xF0): pads at +0x10 stride 0x34; +0xE8 old WndProc; +0xEC hasFocus; +0xED focusTracking.
- `ControllerXInput`: +0x0C userIndex, +0x10 id "JoystickN", +0x20 input points (20×0x38), +0x24 outputs, +0x28 type, +0x2C connected, +0x2E XINPUT_VIBRATION.
- Update: GetState → if !connected && r==0 && GetCapabilities==0 → OnConnect (0x107615a0); r!=0 → OnDisconnect (0x107618c0); !focus → ZeroInputs; SetState on rumble change.
- Type table 0x10d08aa8 (one entry SubType 1 "XInput XBox360 Controller"; mismatch falls back to entry 0 → any SubType OK).
- Composite `ControllerSystem` ptr 0xE24998 (DI devices first, then 4 XInput slots). DI system ptr 0xE24980 (+0x10 IDirectInput8A*, +0x14 device list, +0x28 focus).

## Input system
- InputSystem ptr **0xDFA240**: +0x3C PhysicalControllerManager, +0x80 attach-mouse flag.
- PCM: +0x0C num, +0x10 array, +0x30 slot ctrl id, +0x34 default ctrl id. Update 0x107087f0.
- GameDB ptr 0xE185C0; GetInt 0x670180, SetInt 0x670050, name hash 0x63BE80. SDC name string 0x109f9b18.
- GameDBHelper ptr **0xE1961C** (+0xC IsKeyboardAndMouse).
- `InGameDisableMouse` command-line switch (0x10673170) – mouse only.

## Lua bindings (GameFEControllerManager, 0x1035bcd0)
fe_GetSupportedControllerType 0x1035a3c0, fe_GetControllerName 0x1035a520 ("$XBOX_CONTROLLER_NAME"), fe_IsGamepadAvailable 0x1035a680,
fe_GetNumController, fe_GetControllerMapTypeFromSlot, fe_ApplyControllerSettingsFromGDB, fe_IsKeyboardAndMouse, fe_SetMouseVisibility, gameflow_IsPaused.
scripts.rcf is packed – not analyzed. [?]

## Menu / pause detection
| What | Read | Status |
|---|---|---|
| Paused | `*(u8*)(*(u32*)(base+0xDFA2BC)+0xA8)` | [O] chain, verify at runtime |
| FE cursor visible | `*(u8*)(base+0xCE9CC6)` | [I] 1 in menus |
| KB mode | `*(u8*)(*(u32*)(base+0xE1961C)+0xC)` | [O] |
| Default ctrl id | `*(u32*)(*(u32*)(*(u32*)(base+0xDFA240)+0x3C)+0x34)` | [O] |

GameState vtables (RVA): InGameState 0x9FB328, FrontEndState 0x9F99FC, GamePauseState 0x9FB544, PauseState 0xAAE8DC, NISGameState 0xAB09A4, base 0xAA92CC. Active-state global not found. [?]

## Gyro plan implications
- Mix gyro (Steam "gyro as mouse") into RightStickX/Y inside proxied XInputGetState.
- Implemented instead: vtable hook of IDirectInputDevice8A::GetDeviceState/GetDeviceData (shared dinput8 vtable), no CoCreateInstance hook needed. [O]
- profile.bin persistence of SDC: [?]

## Camera look path (StickUtil / ChaseCamera) – verified bytes live 2026-10-12

- `proto::StickUtil::Update` 0x101053f0 computes look input for cameras (ChaseCamera+0x5F0 holds one).
- **Mouse look gate (pad mode ignores mouse):** 0x1010555C `0F 84 A1 00 00 00` (je skip-mouse after `cmp byte [GameDBHelper+0xC],0`). NOP → native mouse look in pad mode, glyphs unaffected. When mouse delta != 0 it REPLACES the stick for that frame (src=3).
- Mouse path: x = raw[MouseX]*2048/dt → MouseCurve 0x10104fd0 (Hermite below T=1520 [.rdata 0x109fc880], P=653.33 [0x109fc87c], gain 0.23 at low speed) ×0.001. Statics 0x10dd087c/0x10dd0880 guard 0x10dd0884 (0 until first mouse use). T=P=1.0 → linear.
- Mouse sens table float[21] @0x10cecd9c indexed by GameDB SettingsMouseSensitivity (idx8=1.0, ×2^0.2 per step).
- Stick path: axial dead zone 0.3 (.rdata 0x109fc884) → yaw=x|x|+extra (0x1010b062 mulss) → pitch=y|y| (0x1010b068 fld st0) → pitch snap if |y|<0.2|x| (0x1010b054 `76 04`) → yaw ramp accel (ChaseCamera config push 1 at 0x1010f1d5).
- Integration: azimuth += stickAzimuthAngVelocity(+0xEC)*yaw*dt, elevation -= (+0xF0)*pitch*dt.

| Purpose | VA | Original | New |
|---|---|---|---|
| Native mouse look in pad mode | 0x1010555C | 0F 84 A1 00 00 00 | 90×6 |
| Linear mouse | 0x109fc880 / 0x109fc87c | 1520.0 / 653.33 | 1.0 / 1.0 |
| No stick dead zone | 0x109fc884 | 0.3 | 0.0 |
| Linear yaw | 0x1010b062 | F3 0F 59 44 24 48 | F3 0F 10 44 24 48 |
| Linear pitch | 0x1010b068 | D9 C0 | D9 E8 |
| No pitch snap | 0x1010b054 | 76 04 | EB 04 |
| No yaw ramp | 0x1010f1d5 | 6A 01 | 6A 00 (cameras created after patch) |

Unpatched stick pre-compensation: inject r = sgn(q)(0.3+0.7·sqrt|q|), keep |r|<0.93 to avoid ramp.

## Press-start controller lock (KB switch) – 2026-10-12
- Press-start screen Lua calls `fe_LockController(slot)` 0x106858B0 with the first device that produced input; slots 0/1 (keyboard/mouse) → type 0 Keyboard → SDC=0 via Lua `db_SetInt` 0x10670660 [I] → IsKBM=1, PCM+0x30/0x34=0xFE00.
- **Controller-locked flag u8 0x10E19741**: 0 on press-start screen (fe_UnlockAllControllers 0x10685900), 1 after lock. Last lock id u32 0x10CEA7B8 (live 0xFE02).
- PCM_SetDefault 0x107084F0 writes PCM+0x34/+0x30.
- GameDB::SetInt 0x10670050 thiscall(this=[0x10E185C0], const u32 key[2], int player, int value, int sub) ret 0x10; prologue 8B 44 24 04 56 | 57 8B F9 ...
- SDC hash 0x0BEC25471FB4F49A (key[0]=0x1FB4F49A, key[1]=0x0BEC2547); hash fn 0x1063BE80: h=h*0x1003F ^ (int64)(signed char)c.
- Fix (implemented in proxy): strip all DI mouse data while flag==0. Native alternative: detour fe_LockController (prologue 56 6A 01 8B F1) + SetInt veto of SDC<2 while pad connected.

## Resolution cap (max 1920x1200) – 2026-10-12
- Hard-coded table of 14 (u32 w,u32 h) pairs at **0x10cbb2c0** (.data, RW, 112 bytes, file 0xcba2c0): 640x480 … 1920x1080, 1920x1200. Following bytes are other data (table can't grow in place).
- Lua: rm_GetNumAvailableResolutions 0x10405100 (`mov eax,0Eh`), rm_GetAvailableResolutionWidth/Height 0x104050e0/0x104050f0, rm_IsResolutionSupported 0x10405110 (adapter mode AND whitelist), rm_IsResolutionRecommended 0x10405130 (aspect == largest adapter mode), rm_SetResolution 0x104074e0.
- Table copied as whitelist by FUN_100f6a10 (config+0x104, cap 26) and FUN_1065ec50 (device params, cap 32) → render obj [0x10e436c4]+0x30/+0x34. Adapter enum FUN_10939c90 keeps modes only if whitelisted.
- Setting `SettingVideoResolution` is an INDEX 0..13 (apply FUN_10407690 clamps). Cmdline `width`/`height` only pick the default index. No way to force other sizes without patching.
- Fix: rewrite the 14 entries before FUN_100f6a10/FUN_100f3e80 run (from a DLL statically imported by the engine). Entry 13 → 3840x2160 = 8-byte patch at 0x10cbb328 (`80 07 00 00 b0 04 00 00` → `00 0f 00 00 70 08 00 00`).
- Mode appears only if the adapter reports it (4K monitor or DSR/DLDSR/VSR).
- Cmdline switches: windowed, width, height, vsync, aa (0..2), shadow (0..2), NoMSAA, MSAA2x, MSAA4x, InGameDisableMouse, DebugCam2Controller, EnableAllControllers.
- Proxy hosts (non-KnownDLL engine imports): d3d9 (avoid, ReShade), WINMM (timeGetTime), dbghelp, IPHLPAPI, XINPUT1_3 (gyro mod).
- Refresh: engine picks the FIRST adapter mode matching w/h (TV lists 24 Hz first at 4K -> vsync 24 fps). HiRes hooks IDirect3D9::CreateDevice / IDirect3DDevice9::Reset (vtable 16) and sets FullScreen_RefreshRateInHz to the highest available (verified live: 60 fps).

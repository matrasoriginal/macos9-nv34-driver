# NV34 for Mac OS 9 — installation and use

Drivers and 3D for the **GeForce FX Go5200 (NV34)** in the **12" PowerBook G4 (PowerBook6,2)** under
**Mac OS 9.2.x**. The result of phases 1–4 (driver, 2D acceleration, RAVE, our own OpenGL).

## Package contents (`NV34 Release.dsk`)

| Folder | File | What it is | Where it goes |
|---|---|---|---|
| For Extensions | **NV34 NDRV** 4.0.5 | video driver: 1024×768 / 800×600 / 640×480, 256 / Thousands / Millions | `System Folder:Extensions` |
| For Extensions | **NV34 QD Accel** 1.9 | INIT: QuickDraw 2D acceleration, GPU manager (push buffer, VRAM) for 3D | `System Folder:Extensions` |
| For Extensions | **NV34 RAVE** 1.1 | QuickDraw 3D RAVE engine (Unreal Tournament) | `System Folder:Extensions` |
| For Game Folder | **OpenGLLibrary** (NV34 MiniGL 1.0) | our own OpenGL 1.1 + AGL on the GPU | **into the game folder** next to the application, not into Extensions |
| For Game Folder | **NV34 MiniGL config** | MiniGL settings (flags) | next to `OpenGLLibrary` |
| Tools | **NV34 CPU** | higher CPU clock (765 → ~905 MHz) | anywhere or `Startup Items` |
| | Read Me | this guide as plain text | |

Transfer: `.dsk` (Disk Copy) or `.bin` (StuffIt Expander). A FAT USB stick, FTP or e-mail drop the
resource fork and the file type → OS 9 ignores the file.

## Installation

1. Extensions Manager: disable the original NVIDIA 3.1 extensions (Driver, Engine, 2D, OpenGL…) and the ATI extensions.
2. Copy `NV34 NDRV`, `NV34 QD Accel` and `NV34 RAVE` into `System Folder:Extensions`.
3. Restart. Monitors: **Thousands** or **Millions** (3D does not work in 256 colors).

Apple OpenGL (OpenGLLibrary, OpenGLEngine…) can stay in Extensions — other programs keep
using it; our library only applies to the game whose folder it sits in.

## Games

| Game | Path | Setup | Performance (640×480) |
|---|---|---|---|
| **Unreal Tournament** | NV34 RAVE | choose RAVE in the startup dialog | 30–50 fps |
| **Quake II** | MiniGL | `OpenGLLibrary` into the game folder, OpenGL renderer | timedemo 53–54 fps |
| **Half-Life** (Xash3D, SDL "macosclassic") | MiniGL | `OpenGLLibrary` into the game folder; `valve:video.cfg` full screen **only at 1024×768** | 25–75 fps |
| **Quake III** (MacQuake3 1.1x) | MiniGL | `OpenGLLibrary` into the game folder; data matching the application version (for 1.1x only `pak0.pk3`); more memory in Get Info | timedemo 61 fps |
| **Return to Castle Wolfenstein** | MiniGL | `OpenGLLibrary` into the game folder; at least 256 MB of memory in Get Info | 15–90 fps (heavy scenes ~20 fps, CPU bound) |
| **Max Payne** | MiniGL | `OpenGLLibrary` into the game folder | runs, cutscenes included |
| **Medal of Honor: Allied Assault** | MiniGL | `OpenGLLibrary` into the game folder; **512 MB** of memory in Get Info (with 256 MB the game itself crashes with error 2) | heavy scenes ~10 fps (CPU bound) |

Without `OpenGLLibrary` in its folder a game uses Apple OpenGL (software, slow).

MiniGL settings live in the file **`NV34 MiniGL config`** next to `OpenGLLibrary` (the name
`NV34 MiniGL config.txt` works too). Every word turns on one flag, e.g. `showfps` (fps counter),
`nofps` (no 60 fps limit), `stats` (timings to the log); lines starting with `#` are ignored. Changes
apply from the next game launch, without a restart. The list of flags is in the file. Boot-args
(`nv34glshowfps` …) keep working too. If double-clicking the file does not open it, drag it onto SimpleText.

By default MiniGL limits the frame rate to 60 fps — above 60 fps games responded to the mouse with a delay.

MiniGL supports two textures at once (GL_ARB_multitexture) and hardware transformation (T&L) in
`glDrawElements` — vertex positions, clipping and back faces are computed by the GPU.

Under Mac OS 9 the PowerBook 6,2 CPU runs at ~765 MHz instead of 1 GHz: the bus stays at 102 MHz
(Mac OS 9 does not switch speeds on this model). Games with heavy logic (Wolfenstein, MOHAA) are CPU bound.
**NV34 CPU** switches the bus to the higher speed (KeyLargo GPIO, like Linux): CPU ~905 MHz,
RAM reads +22 %, Wolfenstein ~22 → ~25 fps. Run it after every boot (or put it into `Startup
Items`) and after waking from sleep; with Option held down it switches back. Log `NV34 CPU.txt` next to
the program. More heat and higher power draw; the fan regulates itself.

## Known limitations

- **Half-Life** freezes when switching to a lower resolution (640×480) — a bug in the port (also with Apple OpenGL).
- **Quake III**: once after a cold boot the GPU hung right at launch ("hw_sync: GPU did not finish
  within 2 s" in the log); it did not happen again. On a freeze, restart and try again.
- MiniGL lacks: stencil buffer (`cg_shadows 2` shadows), `glClipPlane` (mirrors/portals),
  `glDrawPixels`/`glBitmap`, display lists. Fog (GL_FOG) only with HW T&L — geometry drawn
  otherwise (e.g. with polygon offset) has no fog.
- The scaled modes 800×600 and 640×480 are blurry (the panel's FP scaler).

## Recovery

- **Holding Shift at boot does NOT disable the driver.** In Open Firmware (Cmd-Option-O-F):
  `setenv boot-args nv34off` and `boot` → NV34 NDRV does not bind (Apple framebuffer).
  To undo: `set-default boot-args`.
- `nv34noaccel` turns off the NV34 QD Accel 2D acceleration, `nv34no3d` turns off 3D (both RAVE and MiniGL).
- Uninstall: delete the files from Extensions and from the game folders.

## Diagnostics (boot-args)

Separate several options with a space: `setenv boot-args "nv34glstats nv34gldebug"`, then `reset-all`.
The MiniGL flags (`nv34gl…`) are easier to set in the `NV34 MiniGL config` file.

| boot-args | Component | Effect |
|---|---|---|
| `nv34off` | NDRV | the driver does not bind |
| `nv34noaccel` / `nv34no3d` | QD Accel | no 2D acceleration / no 3D |
| `nv34filelog` | RAVE | log to `Preferences:NV34 RAVE live.txt` |
| `nv34trace` | RAVE | Caps Lock = record one frame into the log |
| `nv34z16`, `nv34no2x`, `nv34immediate`, `nv34raveati`, `nv34tex16swap` | RAVE | test switches |
| `nv34glshowfps` | MiniGL | fps counter in the top left corner |
| `nv34glnofps` / `nv34glfps30` / `nv34glfps75` | MiniGL | no fps limit / at most 30 / at most 75 fps |
| `nv34glqueue` | MiniGL | frames may queue up (the mouse may lag) |
| `nv34glstats` | MiniGL | fps and a time breakdown in the log every 250 frames |
| `nv34gldebug` | MiniGL | verbose log (pixel format, every buffer swap) |
| `nv34gltex` | MiniGL | log of textures and batches (first 3000 lines) |
| `nv34glnoexc` | MiniGL | do not catch crashes (EXCEPTION line in the log) |
| `nv34glclrhw` | MiniGL | hardware scissored clear (Max Payne cutscenes may flicker) |
| `nv34glvtx12`, `nv34glcolorle` | MiniGL | the original vertex format / reversed color byte order |
| `nv34glnomt` | MiniGL | no multitexturing |
| `nv34glnotnl` | MiniGL | no HW T&L (transformation on the CPU) |
| `nv34glnofog` | MiniGL | no fog (GL_FOG) |
| `nv34glz24` | MiniGL | always a 24-bit z-buffer (otherwise Z16 at 16 bpp if the game asks for ≤ 16 bits) |

MiniGL always writes a short log to `System Folder:Preferences:NV34 MiniGL log.txt`
(loading, config, context, back buffer, errors, free VRAM at exit).

## Build

```bash
make release
```

Requires Retro68 (`~/Retro68-build/toolchain`). Output: `dist/NV34 Release.dsk` and `dist/*.bin`.
Sources and references: [README.md](../README.md).

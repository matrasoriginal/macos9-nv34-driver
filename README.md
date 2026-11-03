# NV34 for Mac OS 9 — GeForce FX Go5200 in the 12" PowerBook G4 (PowerBook6,2)

A video driver, 2D acceleration, a QuickDraw 3D RAVE engine and a custom hardware OpenGL (MiniGL) for the
**NVIDIA GeForce FX Go5200 (NV34)** under **Mac OS 9.2.x**.

NVIDIA's last Mac OS 9 driver (3.1, 2002) predates the GeForce FX and does not support the NV34, so under
Mac OS 9 the PowerBook6,2 was stuck with Apple's generic framebuffer: one resolution, Thousands of colors,
no acceleration and software-only OpenGL. This project replaces all of that with drivers written from scratch.

| Component | Version | What it does |
|---|---|---|
| NV34 NDRV | 4.0.4 | native video driver: 1024×768 / 800×600 / 640×480 in 256 / Thousands / Millions, CLUT, gamma |
| NV34 QD Accel | 1.9 | INIT: QuickDraw 2D acceleration (screen → screen CopyBits on the GPU), GPU manager (512 KB push buffer, VRAM arena) shared by the 3D components |
| NV34 RAVE | 1.1 | QuickDraw 3D RAVE engine — Unreal Tournament |
| NV34 MiniGL (`OpenGLLibrary`) | 1.0 | OpenGL 1.1 + AGL on the GPU, placed in a game folder — Quake II, Quake III, Half-Life, Return to Castle Wolfenstein, Max Payne, Medal of Honor |
| NV34 CPU | 4 | switches the CPU to its full clock (Mac OS 9 leaves it at ~765 MHz) |

## Requirements

- PowerBook G4 12" 1 GHz (PowerBook6,2) with the GeForce FX Go5200 32 MB, Mac OS 9.2.x.
- For 3D: Thousands or Millions of colors.
- To build: macOS with [Retro68](https://github.com/autc04/Retro68) (PowerPC toolchain in
  `~/Retro68-build/toolchain`, the 68k toolchain in `~/Retro68-build-68k` for the QD Accel INIT stub) and Python 3.

## Installation

The complete guide (also included on the disk image as *Read Me*) is in [docs/INSTALL.md](docs/INSTALL.md).
In short:

1. Transfer `NV34 Release.dsk` (Disk Copy) or the `.bin` files (StuffIt Expander) to the PowerBook. Do not copy
   the plain files over FAT, FTP or e-mail — they lose the resource fork and file type and OS 9 ignores them.
2. In Extensions Manager disable the original NVIDIA 3.1 extensions and the ATI extensions.
3. Copy `NV34 NDRV`, `NV34 QD Accel` and `NV34 RAVE` from *For Extensions* into `System Folder:Extensions`
   and restart.
4. In Monitors choose **Thousands** or **Millions**.
5. For an OpenGL game copy `OpenGLLibrary` (and optionally `NV34 MiniGL config`) from *For Game Folder* into
   the game's folder next to the application. The Code Fragment Manager finds it before Apple's system library,
   so only that game uses MiniGL; everything else keeps using Apple OpenGL.
6. Optionally run **NV34 CPU** after every boot (or put it into `Startup Items`) for ~905 MHz instead of ~765 MHz.

### Games

| Game | Path | Notes |
|---|---|---|
| Unreal Tournament | NV34 RAVE | choose RAVE in the startup dialog |
| Quake II | MiniGL | timedemo ~54 fps at 640×480 |
| Quake III (MacQuake3 1.1x) | MiniGL | timedemo ~61 fps; give the application more memory |
| Half-Life (Xash3D, SDL "macosclassic") | MiniGL | full screen only at the native 1024×768 |
| Return to Castle Wolfenstein | MiniGL | at least 256 MB of memory in Get Info |
| Max Payne | MiniGL | cutscenes included |
| Medal of Honor: Allied Assault | MiniGL | 512 MB of memory in Get Info; heavy scenes are CPU bound |

### MiniGL settings

MiniGL reads the text file **`NV34 MiniGL config`** (or `NV34 MiniGL config.txt`) next to `OpenGLLibrary`.
Every word turns on one flag, `#` starts a comment, and changes apply from the next game launch:

```
showfps      # fps counter in the top left corner
nofps        # no 60 fps limit (fps30 / fps75 for other limits)
stats        # timings every 250 frames to the log
```

The full list is in the sample file. MiniGL writes a log to `System Folder:Preferences:NV34 MiniGL log.txt`.
The same flags also work as Open Firmware boot-args with the `nv34gl` prefix (e.g. `nv34glshowfps`).

### Recovery

Holding Shift at boot does **not** disable the driver (the Expansion Manager loads `ndrv` files from Extensions
anyway). In Open Firmware (Cmd-Option-O-F at boot) type `setenv boot-args nv34off` and `boot`; the driver then does
not bind and Apple's framebuffer is used. Undo with `set-default boot-args`. `nv34noaccel` disables the 2D
acceleration and `nv34no3d` disables 3D.

## Building

```bash
make            # dist/NV34 test.dsk + dist/*.bin (test image)
make release    # dist/NV34 Release.dsk (installation package)
make clean
```

`make -C driver dist` builds just the driver. Driver build switches (after `make -C driver clean`):
`NV_SCALED_MODES=0` (1024×768 only), `NV_SET_ARB=1` (FIFO arbitration as in nouveau), `NV_FB_CACHE=2`
(experimental write-through framebuffer cache; no effect on the PB 6,2).

The output packages need both forks and the type/creator, so `tools/mkndrv.sh` and `tools/macres.py` produce
MacBinary (`.bin`) files and HFS disk images (`.dsk`).

## How it works

- **NV34 NDRV** ([driver/](driver)) is a native PCI display driver (`ndrv`, Video Driver API). It matches
  `NVDA,NVMac` on the `built-in` node, checks `PMC_BOOT_0` for an NV34 and finds MMIO/VRAM in the Name Registry.
  The panel is driven by CRTC head 1 even though the node is `NVDA,Display-A`; the driver selects the head whose
  format and pitch match what Open Firmware set up. It keeps the PLL and flat-panel timing from Open Firmware and
  only changes the pixel format and pitch; the smaller modes use the FP scaler.
- **NV34 QD Accel** ([nv34qd/](nv34qd)) is an INIT (a 68k stub that loads a PPC fragment) that initializes PFIFO/PGRAPH
  ([accel/](accel), ported from xf86-video-nv) and patches `CopyBits` so that screen → screen copies (window dragging,
  scrolling) run on the GPU. Reading VRAM over AGP is only ~12 MB/s on this machine, so this is what made the Finder fast.
  It also owns the GPU for the 3D components and publishes the shared state through the Name Registry.
- **NV34 RAVE** ([nv34rave/](nv34rave)) is a RAVE 1.6 drawing engine on the NV30-class 3D engine (fixed-function vertices,
  register combiners, a small fragment program for perspective-correct textures). For Unreal Tournament it emulates the
  private ATI RAVE extensions that the game requires.
- **NV34 MiniGL** ([minigl/](minigl)) is a replacement `OpenGLLibrary` exporting all 474 symbols of Apple's library.
  It implements the OpenGL 1.1 fixed pipeline plus AGL, multitexturing, `GL_COMBINE` texture environments (generated
  fragment programs), fog and hardware T&L in `glDrawElements`; textures are swizzled into VRAM with LRU eviction and
  a GPU fence; the back buffer is copied to the window with a 2D blit. It also limits the frame rate to 60 fps to keep
  mouse input responsive.
- **NV34 CPU** ([nv34cpu/](nv34cpu)) switches the bus speed through the KeyLargo GPIOs the same way Linux does,
  because Mac OS 9 does not know this model's speed switching.

## Repository layout

```
driver/        NV34 NDRV (DoDriverIO, Video Driver API, CRTC/RAMDAC/FP)
accel/         NV34 2D engine (PFIFO/PGRAPH) and 3D bring-up, shared GPU state
nv34qd/        NV34 QD Accel INIT (68k stub + PPC CopyBits patch)
nv34rave/      NV34 RAVE engine
minigl/        NV34 MiniGL (OpenGL 1.1 + AGL), sample config file
nv34cpu/       NV34 CPU tool
tools/         PEF tools, packaging into 'ndrv'/'shlb' files, MacBinary, HFS images, Read Me
docs/          INSTALL.md
logs/          logs from test runs on the hardware
```

## Sources and references

Everything was written from public documentation and open-source drivers; proprietary Apple, ATI and NVIDIA binaries
were only examined locally to understand interfaces and are not part of this repository.

**NVIDIA hardware (register and method level)**

- Linux **nouveau** kernel driver — `drivers/gpu/drm/nouveau/dispnv04/` (CRTC, RAMDAC, flat panel and scaler,
  `nvreg.h`), `nvkm/engine/gr/nv30.c`, `nv34.c`, `nv20.c` (PGRAPH init, default NV34 context, context table, tile
  regions), `nv04_ramht`, `nv10_fence` (REF_CNT fence), PLL decoding. MIT license.
- **xf86-video-nv** (X.Org, Copyright NVIDIA Corporation) — `nv_hw.c` (`NVLoadStateExt`), `nv_xaa.c`
  (`NVResetGraphics`, DMA push buffer), `nv_setup.c`, `nv_dma.h`. The 2D engine in [accel/](accel) is a port of it. MIT license.
- **xf86-video-nouveau** — `nv30_exa.c` (`NVAccelInitNV30TCL`, initial NV30 3D state, fragment programs,
  `NV30EXAPrepareComposite`). MIT license.
- **Mesa** — `gallium/drivers/nouveau/nv30` and `nvfx_shader.h` (NV30 fragment program encoding), the classic
  nouveau driver (`nv10_state_frag.c`, `nv10_get_fog_coeff`, `nv20_emit_projection`) for fog and transformation.
- **envytools / rules-ng-ng** — `nv30-40_3d.xml.h` (NV30 3D method numbers) and hardware documentation.
- XFree86 mailing list report on the PowerBook6,2 GeForce FX Go5200 PCI ID `10DE:0329`:
  <https://www.spinics.net/lists/xfree86/msg01952.html>.

**PowerBook / PowerPC platform**

- Linux PowerMac support — `drivers/cpufreq/pmac32-cpufreq.c` (`gpios_set_cpu_speed`, `read_gpio`) for the
  KeyLargo GPIO speed switching in NV34 CPU, `pmu_set_speed` and the clock-spreading code in
  `arch/powerpc/platforms/powermac/` for comparison.
- Open Firmware device tree dumps and the Mac OS 9 Name Registry of the machine itself.

**Mac OS 9 interfaces**

- Apple, *Designing PCI Cards and Drivers for Power Macintosh Computers* — native driver model, `DoDriverIO`,
  `TheDriverDescription`, Video Driver API.
- Apple **Universal Interfaces 3.4** (`Video.h`, `Devices.h`, `NameRegistry.h`, `DriverServices.h`,
  `VideoServices.h`, `MachineExceptions.h`) — the subset needed is re-declared in
  [driver/ndrv_api.h](driver/ndrv_api.h) and [minigl/agl.c](minigl/agl.c).
- *Inside Macintosh* — QuickDraw (CopyBits, regions), Memory Manager, Code Fragment Manager, Mixed Mode Manager,
  Start Manager (INIT loading).
- Apple **QuickDraw 3D RAVE 1.6** documentation and SDK headers (`RAVE.h`, `RAVESystem.h`) and the "Empty Engine"
  sample — basis of [nv34rave/rave.h](nv34rave/rave.h).
- The **OpenGL 1.1 specification** and Apple's `agl.h` (AGL 2.x) — basis of [minigl/gldefs.h](minigl/gldefs.h).
- [QemuMacDrivers](https://github.com/ozbenh/QemuMacDrivers) (`qemu_vga.ndrv`) — structure of a modern Mac OS 9 NDRV.
- [elliotnunn/x-ndrv](https://github.com/elliotnunn/x-ndrv) — catalog of NDRVs from Mac OS X (phase 1 research).
- The macos9lives.com community — prior work on new NDRVs for ATI cards.

**Toolchain**

- [Retro68](https://github.com/autc04/Retro68) — GCC-based cross compiler for classic Mac OS (PowerPC PEF and 68k),
  Multiversal Interfaces, `hfsutils` for disk images.

**Examined locally for interface behavior only (not distributed)**

- NVIDIA Mac OS 9 driver 3.1 (2002) — packaging (`cfrg`, `TheDriverDescription`) and the fact that it lacks NV34 support.
- Apple OpenGL 1.2.x for Mac OS 9 (`OpenGLLibrary` export list, `OpenGLRendererATI`) and the QuickDraw 3D RAVE
  system files of Mac OS 9.2.2 — the export list MiniGL must provide and the private ATI RAVE interface.
- Unreal Tournament's `RaveDrv` — what the game expects from a RAVE engine.

**Games used for testing**: Unreal Tournament, Quake II, Quake III (MacQuake3), Half-Life (Xash3D FWGS with the
SDL2 "macosclassic" port), Return to Castle Wolfenstein, Max Payne, Medal of Honor: Allied Assault, Gothic.

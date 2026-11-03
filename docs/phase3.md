# Phase 3 — 2D speed in the system

State after 4.0d4: games run well (they draw into their own buffer and copy it to the screen), but the Finder
is slow — window redraws, dragging, scrolling. QuickDraw is purely software.

## Why the Finder is slow

The framebuffer is mapped **cache-inhibited** for the CPU. Writes over AGP are still bearable, but every
**read** of VRAM is a separate bus transaction. QuickDraw reads the screen when:

- dragging and moving windows, `ScrollRect` (CopyBits screen → screen),
- using drawing modes that blend with the background (XOR selection, `srcOr`, grayed text, icon masking),
- saving the background under menus.

Games do not have this problem — they only overwrite the screen.

## Step 1: write-through cache (4.0d5)

`SetProcessorCacheMode(..., kProcessorCacheModeWriteThrough)` on the framebuffer range:
reads come from L1/L2, writes go to VRAM immediately (the picture is always current). Since nothing but the CPU
writes VRAM (no acceleration), the cache stays coherent. Copy-back mode would delay the picture —
we do not use it.

Risk: a `dcbz` instruction on a write-through page raises an alignment exception on the G4 (the OS emulates it,
slowly but correctly). If some test got slower than without the cache, that would be why.

Measurement: `NV34 Bench` (fill, screen→screen, offscreen→screen, screen→offscreen,
text). Compare three runs at the same depth:

1. without our driver (Apple COFB),
2. `NV34 NDRV nocache`,
3. `NV34 NDRV` (write-through).

Results are appended to `NV34 Bench.txt` along with the driver name and cache mode.

### Result (2026-10-07, 1024×768, 32 bpp, 504×384 area)

| | COFB | NV34 nocache | NV34 write-through |
|---|---|---|---|
| fill (write) | 736 MB/s | 733 MB/s | 735 MB/s |
| offscreen → screen | 259 MB/s | 268 MB/s | 267 MB/s |
| screen → offscreen (read) | **13 MB/s** | 13 MB/s | 12 MB/s |
| screen → screen | **8.5 MB/s** | 8.5 MB/s | 8.5 MB/s |

Write-through **has no effect** — the system maps PCI/AGP space uncached (probably via a
BAT/fixed WIMG) and `SetProcessorCacheMode` does not change that. Reading VRAM is ~50× slower than writing;
dragging a window (screen → screen) depends entirely on it. COFB is just as slow — this is not a driver
bug.

### Access width (NV34 Bench 2)

| direct VRAM read | 4 B `lwz` | 8 B `lfd` | 16 B `lvx` |
|---|---|---|---|
| | 12.0 MB/s | 12.0 MB/s | 12.0 MB/s |

Throughput does not depend on the access width — the ceiling is on the bus/card side, not the instruction count.
(The RAM tests returned 0: a 730 KB `NewPtr` failed in the small application partition; it does not affect the conclusion.)

Paths originally considered:

- **An AltiVec path for screen → screen CopyBits** (a QuickDraw patch): makes sense only if `lvx`
  reads VRAM significantly faster than 13 MB/s.
- **A shadow framebuffer**: QuickDraw draws into RAM, the driver copies changed rows to VRAM
  from a VBL timer. All reads come from RAM; the cost = compare + copy every frame
  (limited by RAM/RAM → VRAM throughput), latency ≤ 1 frame.
- **A hardware blit** (step 2 below): the fastest, but the biggest project.

That rules out the AltiVec path. What remained was the shadow framebuffer (CPU overhead, worse for games drawing
directly to the screen) and the hardware blit (a big research project).

## Decision (2026-10-07)

**Leave it as is.** The display modes work, games run well, and slow window dragging is the same as with Apple COFB —
it is a property of reading VRAM over AGP on this machine, not a driver regression. Write-through is
disabled in 4.0 (`NV_FB_CACHE=0`) and remains a switch. If 2D is ever revisited, the starting point is a
shadow framebuffer as an optional driver variant, then a hardware blit per step 2.

## Step 2: hardware 2D acceleration (from 2026-10-14)

The software OpenGL renderer does run, but it is unusably slow → we go for our own 2D acceleration.

### Step 2A — bringing up the engine (`NV34 Accel`)

Source of truth: **xf86-video-nv** (MIT), which did accelerated fills and copies on PPC Macs with the
GeForce FX (`10DE:0329`). The port is in [accel/nv34_accel.c](../accel/nv34_accel.c):

- `NVLoadStateExt(pNv, NULL)` for `NV_ARCH_30`: PFIFO/PGRAPH reset via `PMC_ENABLE`, tile regions,
  RAMHT (handles `0x80000010..17`) and objects in PRAMIN (`0x710000`): DMA over all of VRAM, surface2d
  `0x62`, ROP `0x43`, pattern `0x44`, clip `0x19`, line `0x5C`, image blit `0x5F`, GDI `0x4A`,
  scaled image `0x77`, the push buffer DMA object. PGRAPH values for NV30, PFIFO channel 0 in DMA mode,
  `DMA_FETCH` with bit 31 (big-endian push buffer).
- `NVResetGraphics`: 32 KB push buffer at `VRAM − 128 KB`, binding objects to subchannels, surface
  format, pitch, **surface offset = `fboffset` (0x4000)** — the only significant difference from X.
- Fill `RECT_SOLID_RECTS`, blit `BLIT_POINT_SRC/DST/SIZE`, sync = `GET == PUT` and `PGRAPH 0x700 == 0`.
- Left out: PTIMER (xf86 writes it at a wrong offset, not needed), the CRTC part.
- The objects' big-endian flag (`| 0x00080000`) as in xf86; if the fill returns swapped bytes,
  the test automatically retries the objects without it.

The `NV34 Accel` application (standalone, does not change the system): init, GPU fill + pixel check, GPU blit +
content comparison, measurement of GPU fill / GPU blit / CPU (QuickDraw) blit. The log `NV34 Accel.txt`
is written line by line — after a hang the last step remains. A GPU hang = restart.

#### Result of NV34 Accel 1 (2026-10-14, 1024×768, 32 bpp)

Init OK, fill and blit verified by reading back (big-endian objects correct on the first try):

| | speed |
|---|---|
| GPU fill | 2.7 GB/s |
| GPU screen → screen | 1.34 GB/s |
| CPU (QuickDraw) screen → screen | 8 MB/s |

Side effect: the whole screen outside the window got scrambled. Most likely because the init
cleared the PFB tile regions as xf86 does, and the OF framebuffer lives in a tile region (the old contents started being read
linearly). NV34 Accel 2 does not change the tile regions, only mirrors them into PGRAPH, and logs their values.

#### Result of NV34 Accel 2

`PFB tile 0: 00004001 00300000 00001000 80000040` — the OF framebuffer is a tile region (base 0x4000,
limit 0x300000, pitch 4096). With the regions unchanged (only mirrored into PGRAPH) fill and blit
pass and the screen stays intact: GPU fill 2.3 GB/s, blit 1.17 GB/s. **Step 2A done.**

### Step 2B — hooking into QuickDraw (after a successful 2A)

Design: a PPC background application of type `appe` in Extensions (Retro68 can build PPC applications; we have no 68k
toolchain), which at startup installs a patch for `CopyBits` / `ScrollRect` / `PaintRect`/`EraseRect`
via `NSetTrapAddress` with routine descriptors pointing to our PPC code and stays resident. The patch
accelerates only the safe case, otherwise it calls the original routine:

- source and destination = the main screen PixMap (`baseAddr` = framebuffer), `srcCopy`, no mask,
  no scaling and no color table change, a rectangular clip (`visRgn ∩ clipRgn` is a rectangle),
- cursor: `ShieldCursor` before the blit / `ShowCursor` after, like QuickDraw,
- `nva_sync` before any CPU access to the screen → the patch synchronizes before returning.

After a resolution/depth change the engine has to be set up again (`nva_reset_graphics`) — the appe will watch
the main device's `gdPMap`.

#### Implementation: `NV34 QD Accel`

**Attempt 1 — `appe` (qd-1, qd-2): does not work.** The patch gets installed (`trapNow == myUPP`) and our own
`CopyBits` through InterfaceLib goes through it (`selfTestCalls 1`), but the Finder and other applications do not
(`calls 1`). The Process Manager treats a patch installed by an application as local to its context.

**Attempt 2 — INIT** ([nv34qd/](../nv34qd)): a file of type `INIT` in Extensions:

- `init68k.c` — a 68k stub (the Start Manager only runs 68k INIT code): loads resource `'nvqd' 128`
  (sysHeap, locked) via `GetMemFragment` in the system zone, finds `nv34qd_install` and calls it
  through a routine descriptor (Mixed Mode 68k → PPC).
- `qdpatch.c` — a PPC fragment without an application runtime: engine, `CopyBits` patch (`A8EC`), statistics.
  A patch installed during the INIT phase is global.

Acceleration conditions (unchanged):

- source and destination are the framebuffer of the main `GDevice` (CGrafPort portBits = PixMapHandle, a PixMap, not
  a 1-bit BitMap), `srcCopy`, the same size, the source entirely within the bitmap bounds,
- the port has a black foreground and white background, the same `ctSeed` of the color tables,
- clip = `dstRect ∩ bounds ∩ (visRgn ∩ clipRgn if the destination is the port) ∩ maskRgn`, split into ≤ 512
  rectangles; ordered by scroll direction (overlap), one rectangle = one HW blit,
- `ShieldCursor` / `ShowCursor` around it, `nva_sync` before returning (the CPU draws right after).

Otherwise the original CopyBits (`CallUniversalProc`). On a timeout the engine is reinitialized; after a second failure
acceleration is turned off. Depth/resolution change → `nva_reset_graphics`. `boot-args` switches:
`nv34off`, `nv34noaccel`. Statistics: `NV34ACCEL,stats` → NV34 Info.

**Finder profile (qd-4, qd-5):** 2 min of work → 25–31 s in CPU CopyBits that the patch did not recognize.
All of it went to one type: **a 1-bit `BitMap` with the framebuffer's `baseAddr`** (`rowBytes 128`, `screenBits` /
the Window Manager port), destination = source, with a mask — moving windows. QuickDraw substitutes the device
PixMap for it (425 ms for 1010×714 = a 32 bpp copy reading VRAM). Everything else (offscreen → screen,
icons via `CopyMask`/`CopyDeepMask`, text) took < 0.5 s in total. Version 1.3 also treats this alias as the screen.

**Result of 1.3 (qd-6, 2026-10-14):** windows are drawn correctly, the Finder is noticeably faster.

| | calls | total |
|---|---|---|
| GPU (accelerated) | 289 | 363 ms |
| CPU offscreen → screen | 1886 | 457 ms |
| CPU from screen (VRAM reads) | 27 | 230 ms |
| CPU other | 92 | 3 ms |
| CopyMask + CopyDeepMask | 1106 | 135 ms |

Time in CopyBits for ~2 min of work dropped from 25–31 s to ~1.2 s. The rest: screen → offscreen copies
(saving backgrounds, ~8.5 ms/call) — the GPU could only do those with an offscreen buffer in VRAM.

**1.4:** in Thousands neighboring pixels got swapped after moving a window. The objects' BE flag (from xf86)
swaps 32-bit words; at 16/8 bpp a word holds several pixels → shifting by an odd amount scrambles them.
A blit in LE mode is an exact byte copy at any depth → QD Accel uses `beObjects = 0`.

**1.5:** in Millions windows turned yellow after moving — the X8R8G8B8 format zeroes the "X" byte, which in an LE view
of a BE framebuffer is blue. QD Accel now uses the raw formats Y8/Y16/Y32 (exact byte copy).
In Thousands the whole screen broke — changing the surface at run time called `nva_reset_graphics`, which
restarted the push buffer from zero while the GPU kept reading from the old `GET` (old commands = random blits).
After startup the surface is now changed only by commands in the existing stream (`nva_set_surface`).

**State of 1.5 (2026-10-14):** window moves and scrolling through the GPU are correct in 256, Thousands and Millions,
including depth switching at run time. Sleep cannot be tested on the PB 6,2 under OS 9 (sleep does not work on
this model under OS 9). Step 2B done.

Not accelerated: `ScrollRect` (unless it calls CopyBits through the trap), fills (CPU writes are fast),
screen ↔ offscreen copies.

### The original consideration

It would require two separate things, both big:

1. **NV34 PGRAPH / FIFO** — initializing the graphics engine, a channel (PIO or DMA push buffer),
   the `surf2d` / `imageblit` / `gdi_rect` objects, context, CPU synchronization (notifier). Reference:
   nouveau `nv04_fbcon` / `nv10_graph` / envytools. Without a binary blob the NV34 also needs a ctxprog.
2. **Hooking into QuickDraw** — OS 9 has no public API for a 2D accelerator. NVIDIA/ATI did it
   through private hooks (an INIT patching the `CopyBits`/`StdBits`/`FillRect` bottlenecks). Every patch
   must recognize whether it is the main screen, a simple rectangle without a mask and a supported transfer
   mode, otherwise call the original routine; and wait for the GPU before every CPU access.

Decision based on the results of step 1: if write-through removed most of the slowdown (window dragging,
scrolling), step 2 would not be worth it.

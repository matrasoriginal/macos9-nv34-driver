# Phase 0 — decoded dump from the PowerBook 6,2

Source: [phase0-of-dump.txt](phase0-of-dump.txt) (`dev agp/@10/@0 .properties`, transcribed from the screen,
hence OCR errors like `NUDA` → `NVDA`, `9`/`0`, `S` → `8`). Below are the values after correction and
cross-checking (EDID, `NVNT` and `display-power-sequence` confirm each other's timing).

## Node `NVDA,Display-A` (child `@0` under `agp/@10`)

| property | value | meaning |
|---|---|---|
| `name` | `NVDA,Display-A` | head A |
| `device_type` | `display` | |
| `reg` | `0` | head index 0 |
| `compatible` | `NVDA,NVMac` | **this is what `TheDriverDescription.driverType` matches** (same as 3.1) |
| `connector-type` | `2` | |
| `width` × `height` | `0x400` × `0x300` = **1024 × 768** | native panel |
| `depth` | `8` | OF runs at 8 bpp |
| `linebytes` | `0x400` = 1024 | pitch at 8 bpp |
| `refresh` | `0x3c` = 60 Hz | |
| `address` | `0x?0004000` (OCR unclear) | logical FB address = BAR1 + `fboffset` |
| `fboffset` | `0x4000` | start of the FB in VRAM → `NV_PCRTC_START` |
| `NVDA,memsize` / `VRAM,memsize` | `0x02000000` | **32 MB VRAM** |
| `built-in` | (present) | internal panel — the driver only binds to such a node |
| `display-link-type` | `0` | single-link LVDS |
| `display-link-component-bits` | `6` | 6-bit panel (18 bpp + dithering) |
| `display-pixel-component-bits` | `6` | |
| `display-data-justification` | `1` | |
| `backlight-control` | `1 0` | |
| `backlight-pwm-freq` | `0x61a8` = 25000 | |
| `inverter-current` | `0` | |
| `i2c-mapping` | `02ffffff` | DDC on I2C port 2 |
| `panelinfo` | `00195400` | |

### EDID (`EDID`, first 64 B legible)

```
00 ff ff ff ff ff ff 00  06 10 29 9c 00 00 00 00   header, vendor 0x0610 = "APP", product 0x9c29
00 0c 01 03 80 19 12 78  0a f5 be 98 57 50 89 27   EDID 1.3, digital input, 25×18 cm
22 50 54 00 08 00 01 01  01 01 01 01 01 01 01 01   established: 1024×768@60
01 01 01 01 64 19 00 40  41 00 26 30 18 88 ...     detailed timing #1
```

Further descriptors (in the dump under `display-type`): `00 fe "LTN121XJ"` (Samsung LTN121XJ),
`00 fc "Color LCD"`.

### Native timing (detailed timing #1, confirmed by `NVNT`)

| | value |
|---|---|
| pixel clock | **65.00 MHz** (`0x1964` × 10 kHz) |
| H: active / sync start / sync end / total | 1024 / 1048 / 1184 / **1344** |
| V: active / sync start / sync end / total | 768 / 771 / 777 / **806** |
| refresh | 65e6 / (1344 × 806) = 60.00 Hz |

`NVNT` = `0803ff05 0610299c 04000300 05400418 0400… 03260303 0309…` → 0x400, 0x300, 0x540 (1344),
0x418 (1048), 0x326 (806), 0x303 (771), 0x309 (777). Matches the EDID.

## What is still missing (to be filled in from the parent node)

The dump was of the **child** node `@10/@0`. The PCI properties are on the **parent** `agp/@10` (`NVDA,Parent`):

```
dev agp/@10 .properties
```

or from OS X on the same machine:

```
ioreg -l -w0 -x -p IODeviceTree -n NVDA,Parent
```

Needed: `vendor-id`, `device-id` (we expect `0329`), `revision-id`, `assigned-addresses`,
`AAPL,address`, `reg`.

The driver reads them from the Name Registry at run time (no hardcoded addresses), so the first build can be
tested without them. As a safeguard the driver reads `PMC_BOOT_0` and refuses to start if the chip is not an NV34
(`(boot0 >> 20) & 0x1ff == 0x34`) — so it does not depend on `device-id`. The parent's values are needed
mainly for diagnostics in case the driver does not bind.

## Consequences for the NDRV

- Match via `compatible = NVDA,NVMac` (a string in `TheDriverDescription`), binding only to the node
  with the `built-in` property → Display-B (external mini-DVI) stays with the original driver.
- The panel is a fixed 1024×768 / 65 MHz. Native mode = **keep both the PLL and the FP timing from OF**, change only
  the pixel format and pitch. Smaller modes = CRTC display end + FP scaler (experimental).
- 32 MB VRAM, FB at offset `0x4000` — even 1024×768×32 bpp (3 MB) fits without moving it.
- 6-bit panel: the 8-bit CLUT/gamma stays, panel dithering is controlled by the FP (we do not change it).

## Additions from NV34 Info (OS 9 Name Registry, 2026-10-05)

`NVDA,Parent` (`pci`): `vendor-id 10de`, **`device-id 0329`**, `revision-id a3`, model
`GeForce FX Go5200 32MB`, `rom-revision 2069.1`.

| BAR | phys = logical (`AAPL,address`) | size |
|---|---|---|
| 0x10 MMIO | `0x91000000` | 16 MB |
| 0x14 FB | `0xa0000000` | 256 MB |
| 0x30 ROM | `0x90000000` | 128 KB |

`NVDA,Display-A` while OS 9 runs: **`depth 0x20`, `linebytes 0x1000`** — OF/boot ROM leaves the panel at
32 bpp, not 8 bpp as the OCR dump claimed. `address a0004000`, `fboffset 0x4000`.
Without our driver `.Display_Video_Apple_COFB` is bound (the generic OF framebuffer, a single mode).
The node has its own ROM driver `driver,AAPL,MacOS,PowerPC` (31040 B) — OS 9 did not use it.

Diag 4.0d2: `PMC_BOOT_0 0x034900a3`, `PMC_BOOT_1` = big endian, head 0,
`GENERAL_CONTROL 0x00100130`, `FP_TG_CONTROL 0x11100011` (center, DISPEN_POS),
**`PCRTC_CONFIG 0x80000002`** (bit 31 = big-endian scanout, this is how OF handles it).
**The internal panel is driven by head 1**, not head 0, even though the node is `NVDA,Display-A` with `reg 0` (diag 4.0d4):

| | CR28 | pitch | START | FP_TG_CONTROL | GENCTRL | PCRTC_CONFIG |
|---|---|---|---|---|---|---|
| head 0 | `01` (8 bpp) | 1024 | `0x4000` | `0x11100011` | `0x00100130` | `0x80000002` |
| head 1 | `83` (32 bpp + FP slave) | 4096 | `0x4000` | `0x11100000` | `0x00100130` | `0x80000002` |

Head 0 stays in the state left by the OF prompt. CR44 (VGA owner) = 0. 8-bit MMIO accesses go to their natural
addresses even in BE mode (echo test: `byteXor 0`). The big-endian scanout is provided by `PCRTC_CONFIG` bit 31
set by OF — the driver does not change it, and both Thousands and Millions display correctly.

The node's ROM driver (`driver,AAPL,MacOS,PowerPC`, 31040 B) is Apple's `.Display_Video_Apple_COFB`
("Non-existent hardware", a generic OF framebuffer) — not an NVIDIA driver. Local copy:
`reference/rom-ndrv-Display-A.pef` (Apple binary, do not distribute).

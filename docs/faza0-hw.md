# Fáza 0 — dekódovaný dump z PowerBook 6,2

Zdroj: [faza0.txt](../faza0.txt) (`dev agp/@10/@0 .properties`, prepisované z obrazovky, preto
OCR chyby typu `NUDA` → `NVDA`, `9`/`0`, `S` → `8`). Nižšie sú hodnoty po oprave a krížovej kontrole
(EDID, `NVNT` a `display-power-sequence` si navzájom potvrdzujú časovanie).

## Uzol `NVDA,Display-A` (child `@0` pod `agp/@10`)

| property | hodnota | význam |
|---|---|---|
| `name` | `NVDA,Display-A` | hlava A |
| `device_type` | `display` | |
| `reg` | `0` | head index 0 |
| `compatible` | `NVDA,NVMac` | **toto matchuje `TheDriverDescription.driverType`** (rovnako ako 3.1) |
| `connector-type` | `2` | |
| `width` × `height` | `0x400` × `0x300` = **1024 × 768** | natívny panel |
| `depth` | `8` | OF beží v 8 bpp |
| `linebytes` | `0x400` = 1024 | pitch pri 8 bpp |
| `refresh` | `0x3c` = 60 Hz | |
| `address` | `0x?0004000` (OCR nejasné) | logická adresa FB = BAR1 + `fboffset` |
| `fboffset` | `0x4000` | začiatok FB vo VRAM → `NV_PCRTC_START` |
| `NVDA,memsize` / `VRAM,memsize` | `0x02000000` | **32 MB VRAM** |
| `built-in` | (prítomné) | interný panel — driver sa viaže iba na takýto uzol |
| `display-link-type` | `0` | single-link LVDS |
| `display-link-component-bits` | `6` | 6-bit panel (18 bpp + dithering) |
| `display-pixel-component-bits` | `6` | |
| `display-data-justification` | `1` | |
| `backlight-control` | `1 0` | |
| `backlight-pwm-freq` | `0x61a8` = 25000 | |
| `inverter-current` | `0` | |
| `i2c-mapping` | `02ffffff` | DDC na I2C porte 2 |
| `panelinfo` | `00195400` | |

### EDID (`EDID`, prvých 64 B čitateľných)

```
00 ff ff ff ff ff ff 00  06 10 29 9c 00 00 00 00   header, výrobca 0x0610 = "APP", produkt 0x9c29
00 0c 01 03 80 19 12 78  0a f5 be 98 57 50 89 27   EDID 1.3, digitálny vstup, 25×18 cm
22 50 54 00 08 00 01 01  01 01 01 01 01 01 01 01   established: 1024×768@60
01 01 01 01 64 19 00 40  41 00 26 30 18 88 ...     detailed timing #1
```

Deskriptory ďalej (v dumpe pod `display-type`): `00 fe "LTN121XJ"` (Samsung LTN121XJ),
`00 fc "Color LCD"`.

### Natívne časovanie (detailed timing #1, potvrdené `NVNT`)

| | hodnota |
|---|---|
| pixel clock | **65.00 MHz** (`0x1964` × 10 kHz) |
| H: active / sync start / sync end / total | 1024 / 1048 / 1184 / **1344** |
| V: active / sync start / sync end / total | 768 / 771 / 777 / **806** |
| refresh | 65e6 / (1344 × 806) = 60.00 Hz |

`NVNT` = `0803ff05 0610299c 04000300 05400418 0400… 03260303 0309…` → 0x400, 0x300, 0x540 (1344),
0x418 (1048), 0x326 (806), 0x303 (771), 0x309 (777). Sedí s EDID.

## Čo ešte chýba (treba doplniť z rodičovského uzla)

Dump bol z **child** uzla `@10/@0`. PCI properties sú na **rodičovi** `agp/@10` (`NVDA,Parent`):

```
dev agp/@10 .properties
```

alebo z OS X na tom istom stroji:

```
ioreg -l -w0 -x -p IODeviceTree -n NVDA,Parent
```

Potrebné: `vendor-id`, `device-id` (čakáme `0329`), `revision-id`, `assigned-addresses`,
`AAPL,address`, `reg`.

Driver ich za behu číta sám z Name Registry (nehardcoduje adresy), takže prvý build sa dá testovať
aj bez nich. Ako poistku driver číta `PMC_BOOT_0` a odmietne štart, ak čip nie je NV34
(`(boot0 >> 20) & 0x1ff == 0x34`) — na `device-id` teda nezávisí. Hodnoty z rodiča sú potrebné
hlavne na diagnostiku, keby sa driver nenaviazal.

## Dôsledky pre NDRV

- Match cez `compatible = NVDA,NVMac` (string v `TheDriverDescription`), naviazanie len na uzol
  s property `built-in` → Display-B (externý mini-DVI) ostane pôvodnému driveru.
- Panel je fixný 1024×768 / 65 MHz. Natívny režim = **ponechať PLL aj FP timing z OF**, meniť iba
  formát pixelov a pitch. Menšie režimy = CRTC display end + FP scaler (experimentálne).
- 32 MB VRAM, FB na offsete `0x4000` — aj 1024×768×32 bpp (3 MB) sa zmestí bez presunu.
- 6-bit panel: 8-bit CLUT/gamma ostáva, panel dithering riadi FP (nemeníme).

## Doplnené z NV34 Info (OS 9 Name Registry, 2026-10-05)

`NVDA,Parent` (`pci`): `vendor-id 10de`, **`device-id 0329`**, `revision-id a3`, model
`GeForce FX Go5200 32MB`, `rom-revision 2069.1`.

| BAR | phys = logická (`AAPL,address`) | veľkosť |
|---|---|---|
| 0x10 MMIO | `0x91000000` | 16 MB |
| 0x14 FB | `0xa0000000` | 256 MB |
| 0x30 ROM | `0x90000000` | 128 KB |

`NVDA,Display-A` za behu OS 9: **`depth 0x20`, `linebytes 0x1000`** — OF/boot ROM nechá panel v
32 bpp, nie 8 bpp ako tvrdil OCR dump. `address a0004000`, `fboffset 0x4000`.
Bez nášho drivera je naviazaný `.Display_Video_Apple_COFB` (generický OF framebuffer, jeden režim).
Uzol má vlastný ROM driver `driver,AAPL,MacOS,PowerPC` (31040 B) — OS 9 ho nepoužil.

Diag 4.0d2: `PMC_BOOT_0 0x034900a3`, `PMC_BOOT_1` = big endian, hlava 0,
`GENERAL_CONTROL 0x00100130`, `FP_TG_CONTROL 0x11100011` (center, DISPEN_POS),
**`PCRTC_CONFIG 0x80000002`** (bit 31 = big-endian scanout, takto to rieši OF).
**Interný panel ťahá hlava 1**, nie hlava 0, hoci uzol je `NVDA,Display-A` s `reg 0` (diag 4.0d4):

| | CR28 | pitch | START | FP_TG_CONTROL | GENCTRL | PCRTC_CONFIG |
|---|---|---|---|---|---|---|
| hlava 0 | `01` (8 bpp) | 1024 | `0x4000` | `0x11100011` | `0x00100130` | `0x80000002` |
| hlava 1 | `83` (32 bpp + FP slave) | 4096 | `0x4000` | `0x11100000` | `0x00100130` | `0x80000002` |

Hlava 0 ostáva v stave z OF promptu. CR44 (VGA owner) = 0. 8-bit MMIO prístupy idú na prirodzené
adresy aj v BE režime (test ozveny: `byteXor 0`). Big-endian scanout zabezpečuje `PCRTC_CONFIG` bit 31
z OF — driver ho nemení, a Thousands aj Millions sa zobrazujú správne.

ROM driver uzla (`driver,AAPL,MacOS,PowerPC`, 31040 B) je Apple `.Display_Video_Apple_COFB`
(„Non-existent hardware“, generický OF framebuffer) — nie NVIDIA driver. Lokálna kópia:
`reference/rom-ndrv-Display-A.pef` (Apple binárka, nedistribuovať).

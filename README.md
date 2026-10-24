# NV34 pre Mac OS 9 — GeForce FX Go5200 v PowerBooku G4 12" (PowerBook 6,2)

Vlastný video driver, 2D akcelerácia, QuickDraw 3D RAVE engine a vlastné OpenGL (MiniGL) pre
**GeForce FX Go5200 (NV34)** pod Mac OS 9.2.x.

**Inštalácia a použitie: [docs/INSTALL.md](docs/INSTALL.md)** (`make release` → `dist/NV34 Release.dsk`).

| Komponent | Verzia | Čo robí |
|---|---|---|
| NV34 NDRV | 4.0.4 | režimy 1024×768 / 800×600 / 640×480, 256 / Thousands / Millions, CLUT, gama |
| NV34 QD Accel | 1.9 | 2D akcelerácia QuickDraw, správa GPU (push buffer 512 KB, VRAM aréna) pre 3D |
| NV34 RAVE | 1.1 | QuickDraw 3D RAVE engine — Unreal Tournament |
| NV34 MiniGL (`OpenGLLibrary`) | 1.0 | OpenGL 1.1 + AGL na GPU, do priečinka hry — Quake II, Half-Life, Quake III |

Plán a história: [PLAN.md](PLAN.md). Hardvér: [docs/faza0-hw.md](docs/faza0-hw.md).
3D (RAVE, MiniGL, merania): [docs/faza4-opengl.md](docs/faza4-opengl.md).

Nasledujúce časti popisujú vývoj drivera (fáza 1–3).

## Build

Potrebný [Retro68](https://github.com/autc04/Retro68) (PPC toolchain, `~/Retro68-build/toolchain`).

```bash
make -C driver dist
```

Výstup:

- `driver/build/NV34NDRV.pef` — PEF data fork (`python3 tools/pef.py info` ukáže exporty a `TheDriverDescription`)
- `dist/NV34 NDRV` — súbor s type `ndrv` / creator `NV34` (pre HFS+ kľúč, AFP)
- `dist/NV34 NDRV.dsk` — 1.4 MB HFS obraz s driverom

Prepínače (po `make -C driver clean`): `NV_SCALED_MODES=0` (iba 1024×768), `NV_SET_ARB=1` (FIFO arbitráž podľa nouveau), `NV_FB_CACHE=2` (experimentálne write-through pre framebuffer; na PB 6,2 bez účinku).

`make` v koreni pribalí aj aplikácie `NV34 Info` a `NV34 Bench` ([docs/faza3.md](docs/faza3.md)).

## Inštalácia a test (PowerBook 6,2, OS 9)

`make` v koreni repa vyrobí `dist/NV34 test.dsk` (driver + NV34 Info) a `dist/*.bin` (MacBinary).

**Prenos:** driver potrebuje data fork, resource fork (`cfrg`, `vers`) **aj** typ `ndrv`. FAT kľúč,
FTP, e-mail či web ich zahodia → OS 9 súbor ignoruje a Extensions Manager ho nezobrazí. Prenášaj
`.dsk` (Disk Copy / ShrinkWrap) alebo `.bin` (rozbaliť StuffIt Expanderom). Kontrola: Get Info má
ukázať verziu `4.0`, ResEdit/File Type typ `ndrv`, creator `NV34`.

0. Spusti **NV34 Info** (pred aj po inštalácii drivera) a pošli `NV34 Info.txt`.
1. Extensions Manager: vypnúť NVIDIA 3.1 (Driver, Engine, 2D, OpenGL, …) aj ATI extensions.
2. `NV34 NDRV` do `System Folder:Extensions`, restart.
3. Monitors CP: Millions → Thousands → 256 pri 1024×768. Až potom 800×600 — škálované režimy nie sú
   označené ako „safe“, takže Display Manager by mal pýtať potvrdenie a pri nepotvrdení sa vrátiť
   (neoverené). Ak obraz zmizne a nevráti sa, reštart so Shift.
4. Hra / app vyžadujúca 256 farieb alebo 640×480.

**Záchrana:** **Shift (Extensions Off) driver NEvypne** — Expansion Manager načíta `ndrv`
z Extensions aj tak. Možnosti:

1. Open Firmware (Cmd-Option-O-F pri štarte): `setenv boot-args nv34off` a `boot` → driver 4.0.2+
   sa nenaviaže, ostane Apple COFB. Vypínač zrušiť: `set-default boot-args`.
2. Naštartovať OS X a súbor `NV34 NDRV` z OS 9 `System Folder:Extensions` zmazať.

### Čo hlásiť pri probléme

- čo bolo vidno (čierne / zlé farby / posunutý obraz / nič sa nezmenilo) a v akom režime,
- či sa v Monitors CP vôbec objavili nové režimy (= driver sa naviazal),
- z OF: `dev agp/@10 .properties` (chýbajú `device-id`, `assigned-addresses`, `AAPL,address`).

Driver pri štarte zapíše do Name Registry property `NV34NDRV,diag` na `NVDA,Display-A`
(verzia, `PMC_BOOT_0`, hlava, endianita, adresy MMIO/FB, pôvodné CR28/CR46/pitch, GENERAL_CONTROL,
FP_TG_CONTROL, PCRTC_CONFIG).

### Známy problém: štart s uloženou inou hĺbkou než Millions

Ak bol pred vypnutím nastavený iný režim, pri štarte sa hardvér prepne do uloženej hĺbky, ale
QuickDraw ostane v 32 bpp → pruhovaný, roztiahnutý obraz. Oprava: štart so **Shift**, súbor
`System Folder:Preferences:Display Preferences` presunúť preč, reštart. Ladí sa cez záznam volaní
v 4.0.1 (NV34 Info 4 v `Startup Items`).

### Stav na PB 6,2 (4.0)

Rozlíšenia 1024×768 / 800×600 / 640×480 a hĺbky 256 / Thousands / Millions fungujú.

- **Hlava:** panel ťahá hlava 1. Driver vyberá hlavu podľa zhody CR28 a pitchu s OF `depth`/`linebytes`.
- **Endianita:** OF štartuje v 32 bpp s `PCRTC_CONFIG` bit 31 (big-endian scanout); driver ho nechá,
  CR46 LFB swap nezapína (iba ak by štartoval v 8 bpp).
- **Škálované režimy:** FP scaler podľa nouveau `dispnv04/dfp.c`.
- **2D:** voliteľný INIT **NV34 QD Accel** (1.5) prenáša kopírovanie obrazovka → obrazovka (presun
  okien, scroll) na GPU — ~100× rýchlejšie než CPU, ktoré číta VRAM cez AGP len ~12 MB/s
  ([docs/faza3.md](docs/faza3.md)). Funguje v 256 / Thousands / Millions aj pri prepínaní za behu.
  Vypnutie: Shift pri štarte alebo `boot-args nv34noaccel`.

## Štruktúra

```
driver/ndrv.c       DoDriverIO, TheDriverDescription, Video Driver API (csc*)
driver/nv34.c/.h    NV34 CRTC / RAMDAC / FP (pitch, pixel formát, CLUT, scaler)
driver/ndrv_api.h   ručne prepísaná podmnožina Universal Interfaces (Video.h, Devices.h, …)
tools/pef.py        PEF info / scan / extract / ioreg (fáza 1)
tools/mkndrv.sh     PEF → OS 9 'ndrv' súbor + HFS obraz
tools/macres.py     resource fork ('cfrg', 'vers') + MacBinary
nv34info/           OS 9 app: dump Name Registry NVIDIA uzlov + diag drivera
nv34bench/          OS 9 app: meranie QuickDraw a priameho prístupu k VRAM
accel/              NV34 2D engine (PFIFO/PGRAPH, port z xf86-video-nv, MIT)
nv34accel/          OS 9 app: test 2D enginu (init, výplň, blit, meranie)
nv34qd/             OS 9 INIT: 68k stub + PPC fragment, GPU CopyBits obrazovka → obrazovka
                    (68k časť potrebuje Retro68 m68k toolchain v ~/Retro68-build-68k)
docs/               fáza 0 (HW dump), fáza 1 (extrakcia hotového NDRV)
```

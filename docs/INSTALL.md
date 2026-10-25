# NV34 pre Mac OS 9 — inštalácia a použitie

Ovládače a 3D pre **GeForce FX Go5200 (NV34)** v **PowerBooku G4 12" (PowerBook6,2)** pod
**Mac OS 9.2.x**. Výsledok fáz 1–4 (driver, 2D akcelerácia, RAVE, vlastné OpenGL).

## Obsah balíka (`NV34 Release.dsk`)

| Priečinok | Súbor | Čo to je | Kam patrí |
|---|---|---|---|
| Do Extensions | **NV34 NDRV** 4.0.4 | video driver: 1024×768 / 800×600 / 640×480, 256 / Thousands / Millions | `Systémová zložka:Extensions` |
| Do Extensions | **NV34 QD Accel** 1.9 | INIT: 2D akcelerácia QuickDraw, správca GPU (push buffer, VRAM) pre 3D | `Systémová zložka:Extensions` |
| Do Extensions | **NV34 RAVE** 1.1 | QuickDraw 3D RAVE engine (Unreal Tournament) | `Systémová zložka:Extensions` |
| Do priecinka hry | **OpenGLLibrary** (NV34 MiniGL 1.0) | vlastné OpenGL 1.1 + AGL na GPU | **do priečinka hry** vedľa aplikácie, nie do Extensions |
| Nastroje | NV34 Info, NV34 GL Check, NV34 DSp Test, NV34 RAVE Test | diagnostika | kamkoľvek |
| | Citaj ma | tento návod bez diakritiky | |

Prenos: `.dsk` (Disk Copy) alebo `.bin` (StuffIt Expander). FAT kľúč, FTP či e-mail zahodia
resource fork a typ súboru → OS 9 súbor ignoruje.

## Inštalácia

1. Extensions Manager: vypnúť pôvodné NVIDIA 3.1 (Driver, Engine, 2D, OpenGL…) a ATI extensions.
2. `NV34 NDRV`, `NV34 QD Accel` a `NV34 RAVE` skopírovať do `Systémová zložka:Extensions`.
3. Reštart. Monitors: **Thousands** alebo **Millions** (3D v 256 farbách nefunguje).

Apple OpenGL (OpenGLLibrary, OpenGLEngine…) v Extensions môže ostať — ostatné programy ho
používajú ďalej; naša knižnica platí iba pre hru, v ktorej priečinku leží.

## Hry

| Hra | Cesta | Nastavenie | Výkon (640×480) |
|---|---|---|---|
| **Unreal Tournament** | NV34 RAVE | v štartovacom dialógu zvoliť RAVE | 30–50 fps |
| **Quake II** | MiniGL | `OpenGLLibrary` do priečinka hry, ovládač OpenGL | timedemo 53–54 fps |
| **Half-Life** (Xash3D, SDL „macosclassic“) | MiniGL | `OpenGLLibrary` do priečinka hry; `valve:video.cfg` celá obrazovka **len v 1024×768** | 25–75 fps |
| **Quake III** (MacQuake3 1.1x) | MiniGL | `OpenGLLibrary` do priečinka hry; dáta zhodné s verziou aplikácie (pre 1.1x iba `pak0.pk3`); v Get Info viac pamäte | timedemo 61 fps |
| **Return to Castle Wolfenstein** | MiniGL | `OpenGLLibrary` do priečinka hry; v Get Info aspoň 256 MB pamäte | 15–90 fps (náročné scény ~20 fps, brzdí CPU) |

Bez `OpenGLLibrary` v priečinku hra použije Apple OpenGL (softvérové, pomalé).

MiniGL podporuje dve textúry naraz (GL_ARB_multitexture) a v `glDrawElements` hardvérovú
transformáciu (T&L) — polohy vrcholov, orezanie a odvrátené steny počíta GPU.

Procesor PowerBooku 6,2 beží pod Mac OS 9 na ~750 MHz namiesto 1 GHz (PMU ho neprepne
na plnú rýchlosť, Mac OS 9 o tom nevie). Hry s náročnou logikou (Wolfenstein) brzdí CPU.

## Známe obmedzenia

- **Half-Life** pri prepnutí na nižšie rozlíšenie (640×480) zamrzne — chyba portu (aj s Apple OpenGL).
- **Quake III**: raz po studenom štarte zaseknutie GPU hneď pri spustení („hw_sync: GPU nedobehol
  do 2 s“ v logu); neopakovalo sa. Pri zamrznutí reštart a znova.
- MiniGL nemá: stencil buffer (tiene `cg_shadows 2`), `glClipPlane` (zrkadlá/portály),
  hmlu (Wolfenstein bez hmly), `glDrawPixels`/`glBitmap`, display listy.
- Škálované režimy 800×600 a 640×480 sú rozmazané (FP scaler panelu).

## Záchrana

- **Shift pri štarte driver NEvypne.** V Open Firmware (Cmd-Option-O-F):
  `setenv boot-args nv34off` a `boot` → NV34 NDRV sa nenaviaže (Apple framebuffer).
  Zrušenie: `set-default boot-args`.
- `nv34noaccel` vypne 2D akceleráciu NV34 QD Accel, `nv34no3d` 3D (RAVE aj MiniGL).
- Odinštalovanie: súbory z Extensions a z priečinkov hier vymazať.

## Diagnostika (boot-args)

Viac volieb naraz oddeliť medzerou: `setenv boot-args "nv34glstats nv34gldebug"`, potom `reset-all`.

| boot-args | Komponent | Účinok |
|---|---|---|
| `nv34off` | NDRV | driver sa nenaviaže |
| `nv34noaccel` / `nv34no3d` | QD Accel | bez 2D akcelerácie / bez 3D |
| `nv34filelog` | RAVE | log do `Preferences:NV34 RAVE live.txt` |
| `nv34trace` | RAVE | Caps Lock = záznam jednej snímky do logu |
| `nv34z16`, `nv34no2x`, `nv34immediate`, `nv34raveati`, `nv34tex16swap` | RAVE | testovacie prepínače |
| `nv34glstats` | MiniGL | každých 250 snímok fps a rozdelenie času do logu |
| `nv34gldebug` | MiniGL | podrobný log (pixel formát, každá výmena snímky) |
| `nv34gltex` | MiniGL | log textúr a dávok (prvých 3000 riadkov) |
| `nv34glvtx12`, `nv34glcolorle` | MiniGL | pôvodný formát vrcholov / opačné poradie bajtov farby |
| `nv34glnomt` | MiniGL | bez multitextúry |
| `nv34glnotnl` | MiniGL | bez HW T&L (transformácia na CPU) |
| `nv34glz24` | MiniGL | vždy 24-bitový z-buffer (inak Z16 pri 16 bpp, ak hra pýta ≤ 16 bitov) |

MiniGL vždy zapisuje stručný log do `Systémová zložka:Preferences:NV34 MiniGL log.txt`
(načítanie, kontext, zadný buffer, chyby, voľná VRAM pri ukončení).

## Build

```bash
make release
```

Potrebný Retro68 (`~/Retro68-build/toolchain`). Výstup: `dist/NV34 Release.dsk` a `dist/*.bin`.
Podrobnosti vývoja: [PLAN.md](../PLAN.md), [faza4-opengl.md](faza4-opengl.md).

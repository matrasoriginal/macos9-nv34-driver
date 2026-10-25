# Fáza 4 — hardvérové OpenGL na NV34 (GeForce FX Go5200)

Softvérový renderer Apple OpenGL na PB 6,2 funguje, ale je nepoužiteľne pomalý. NVIDIA OpenGL 3.1
čip NV34 nepozná. Cieľ: OpenGL 1.x (rozsah hier typu Quake/Quake 2/Half-Life) na 3D engine NV34.

## Kľúčové zistenie

NV30/NV34 má okrem programovateľných shaderov aj **fixed-function hardvér** z generácie NV2x:
transformáciu cez matice MODELVIEW/PROJECTION (bez vertex programu) a **register combiners**.
xf86-video-nouveau (DDX) takto kreslil kompozíciu: fixed-function vrcholy + jeden krátky fragment
program na čítanie textúr + combiners. To sa takmer priamo mapuje na OpenGL 1.x fixed pipeline —
netreba portovať prekladač shaderov z Mesy.

## Referencie (MIT)

- Linux nouveau `nvkm/engine/gr/nv30.c` (`nv30_gr_init`), `nv34.c` (predvolený PGRAPH kontext
  0x46dc B), `nv20.c` (tabuľka kontextov, auto-prepínanie kontextu).
- xf86-video-nouveau `nv30_exa.c` (`NVAccelInitNV30TCL`, kreslenie trojuholníkov, fragment programy).
- Mesa 23.3 `gallium/drivers/nouveau/nv30` (úplný 3D driver: textúry, blending, depth, stavy) a
  `nv30-40_3d.xml.h` (čísla metód).

## Etapy

| | cieľ | výstup |
|---|---|---|
| **M1** | 3D engine kreslí trojuholník | `NV34 GL Test` (variant FP kódovania × combiners) |
| M2 | textúry, depth buffer, blending, meranie fill rate | rozšírený test |
| M3 | rozhodnutie o napojení na aplikácie (nižšie) | |
| M4 | OpenGL pre hry (Quake ako prvý cieľ) | knižnica |

### Napojenie na aplikácie (M3) — možnosti

1. **Náhrada `OpenGLLibrary` + AGL** vlastným fragmentom s rovnakými exportmi (gl*, agl*), ktorý kreslí
   na NV34. Nezávislé od neverejného rozhrania Apple. Rozsah môže byť „MiniGL“ — len to, čo
   používajú konkrétne hry (ako 3dfx MiniGL pre Quake).
2. **Renderer plugin pre Apple OpenGL Engine** (`gld1`, ako `OpenGLRendererATI`). Úplná integrácia
   so všetkými GL aplikáciami, ale rozhranie je neverejné — treba ho vyčítať z binárok
   (`OpenGLRenderer`, `OpenGLRendererATI` z OS 9.2.2).

Rozhodne sa podľa M1/M2 a podľa exportov Apple OpenGL binárok.

## M1 — NV34 GL Test

`accel/nv34_3d.c` + `nv34gl/main.c`:

- PGRAPH init pre 3D (`nv30_gr_init` + NV34 RDI zápisy), kontext kanála 0 v PRAMIN `0x20000`
  s predvolenými hodnotami z `nv34.c`, tabuľka kontextov `0x14000`, auto-prepínanie
  (`DEBUG_3` bit 2) a zneplatnenie aktuálneho kontextu → prvá metóda načíta kontext.
- Objekt `0x0697` (handle `0x80000020`, subkanál 7) a DMA objekt `0x3d` cez VRAM
  (handle `0x80000030`) v RAMHT; opätovné naviazanie 2D objektov na subkanály 0–6.
- Počiatočný stav z DDX, render target = framebuffer (A8R8G8B8 linear, offset `fboffset`),
  zeta v scratch VRAM (16 MB), fragment program v VRAM (24 MB).
- Fragment program `MOVR R0, f[COL0]; END` = `01003E01 1c9dc801 0001c800 0001c800`
  (kódovanie podľa `nvfx_shader.h` a príkladov v DDX).
- 4 varianty: kódovanie FP vo VRAM (polovice prehodené ako `PUSH_DATAs`, s/bez prehodenia bajtov)
  × výstup cez register combiners (final = primárna farba) áno/nie. Biely a červený trojuholník,
  spätné čítanie pixelov, pri chybe PGRAPH `INTR/NSOURCE/NSTATUS/TRAPPED_ADDR/DATA`.

Test beží bez NV34 QD Accel (prestavuje engine).

### Výsledok gl-1 (2026-10-14)

`3D init: TIMEOUT`, PGRAPH nečinný bez chyby; posledná spracovaná metóda = väzba subkanála 6
(`trapped addr 00060000`, data `0x1207`). Kontext sa načítal, 2D väzby prešli, zastavila sa väzba
3D objektu na subkanál 7 — PFIFO zrejme nenašiel handle v RAMHT (zvyšok RAMHT obsahoval starý
obsah VRAM, xf86 ho nenuluje). gl-2: `nva_init_engine` nuluje celú RAMHT, test loguje stav PFIFO.

## Apple OpenGL v OS 9 (OpenGL 1.2.x, súbory z MacOS9Lives 9.2.2)

| súbor | rozhranie |
|---|---|
| `OpenGLLibrary` | 474 exportov `gl*` + `agl*` (to volajú aplikácie) |
| `OpenGLEngine` | 17 exportov `gli*` |
| `OpenGLRenderer` | softvérový renderer: `gld*` (22) + `GLSystem` (`gls*`) + 4 rasterizéry `glr*` |
| `OpenGLRendererATI` | `gld*` (22), kreslí cez **QuickDraw 3D RAVE** (`QADrawContextNew`, `QATextureNew`, `QAEngineGestalt`…) |
| `OpenGLMemory`, `OpenGLUtility` | `glm*` alokácie, `glu*` |

`OpenGLRendererATI` vyberá podľa RAVE enginu názov a rozšírenia („Rage 128 / Radeon / Radeon 8500 /
Radeon 9000 OpenGL Engine“). Nová možnosť pre M3: **RAVE engine pre NV34** (RAVE je zdokumentované
rozhranie pluginu) → RAVE hry priamo a potenciálne celé Apple OpenGL cez ATI renderer, ak prijme
náš engine (overiť: kontrola vendor/engine ID, súkromné ATI rozšírenia). Lokálne kópie binárok:
`reference/opengl-os9/` (Apple binárky, nedistribuovať).

### Výsledok gl-2

`3D init: OK`, všetky metódy prešli bez chyby PGRAPH/PFIFO. 3D engine **kreslí**, ale pixely sú
rozhádzané v mriežke cez celú šírku — zápis do tiled framebuffera bez prekladu dlaždíc. 2D používa
kópiu tile regiónov v `PGRAPH 0x400900..`, 3D aj RDI tabuľky `0x00EA0010/30/50` (nouveau
`nv20_gr_tile`). gl-3 ich nastavuje z `PFB 0x100240..`.

### Výsledok gl-3 — prvé trojuholníky ✔

Po nastavení RDI tile tabuliek sú trojuholníky na správnom mieste a v správnom tvare, bez chýb
PGRAPH/PFIFO. Kreslí iba cesta cez **register combiners** (V2/V3); samotný fragment program bez
combiners nič (V0/V1) — ako v DDX, FP slúži na čítanie textúr, výstup robia combiners.
Farby sú v LE poradí: červená RGBA(1,0,0,1) → pamäť `00 00 FF FF` (tyrkysová, alfa v modrom kanáli).
**M1 splnené.**

gl-4: 3D objekt s/bez BE príznaku (`0x00080000`), textúra RECT A8R8G8B8 64×64 (šachovnica) cez
FP `TXPH H0, a[tex0], t[0]` (`18009e81 1c9dc801 0001c800 3fe1c800`) a combiners `tex0 × primárna`.

### Výsledok gl-4

BE príznak 3D objektu (`0x00080697`) opravuje farby: červená = `ffff0000`. 3D pobeží s BE objektom.
Textúrovaný štvorec sa nenakreslil (čierny) — fragment program zatiaľ nikdy nepreukázal výstup
a BE príznak môže meniť aj čítanie FP z VRAM. gl-5: diagnostický `MOVH H0, f[COL0]`
(`01003E81 …`) + combiners final = H0 (zdroj 0x08), plus textúra, oboje v oboch kódovaniach FP.

### Výsledok gl-5 — fragment program a textúry ✔

S BE 3D objektom platí **kódovanie FP 1** (GPU číta program z VRAM bez prehadzovania bajtov).
`MOVH H0, f[COL0]` → combiners `final = H0` kreslí; RECT textúra A8R8G8B8 cez `TXPH H0` a combiners
`tex0 × primárna` dáva správnu šachovnicu (červená `ffff0000`, zelená `ff00ff00`).
`f[COL0]` vo FP má prehodené kanály (zelená → červená) — farby sa preto miešajú v combineroch
(primárna farba je správne, gl-4), FP iba číta textúry (ako DDX).

gl-6: z-buffer (2 mierky z vo viewporte), blending SRC_ALPHA/ONE_MINUS_SRC_ALPHA, swizzled POT
textúra s REPEAT a normalizovanými súradnicami, meranie fill rate a trojuholníkov/s.

### Výsledok gl-6

| test | výsledok |
|---|---|
| blending SRC_ALPHA/ONE_MINUS_SRC_ALPHA | ✔ (`bfff8080` cez červenú, `40808080` cez čiernu) |
| swizzled POT textúra, REPEAT, normalizované súradnice, lineárny filter | ✔ |
| fill rate (textúra) | ≥ 589 Mpix/s (rozlíšenie merania 1 tick) |
| malé trojuholníky (immediate mode) | ~600 k/s |
| z-buffer | ✘ nič sa nenakreslí (pravdepodobne nezabral `CLEAR_BUFFERS`) |

gl-7: spätné čítanie zety po `CLEAR_BUFFERS`, zeta vyplnená CPU (`FFFFFFFF`), hodnoty zety po
kreslení; dlhšie merania výkonu.

### Výsledok gl-7 — z-buffer ✔ (okrem vymazania)

So zetou vyplnenou CPU (`FFFFFFFF`) a **mierkou z = 1** vo viewporte funguje test LESS aj zápis:
A+C modrá, A+B+C červená, A zelená. Zeta po kreslení: z 0.75 → `bfffffff`, 0.5 → `7fffffff`,
0.25 → `3fffffff` — hardvér sám mapuje z 0..1 na 24-bit hĺbku (Z24S8, stencil v najnižšom bajte).
Mierka 2²⁴−1 nekreslí nič (všetko mimo rozsahu). `CLEAR_BUFFERS` zmenil `12345678` → `00000078`:
hĺbku vynuloval, stencil nechal — pravdepodobne kvôli `DEPTH_WRITE_ENABLE 0` počas clearu alebo
poradiu bajtov hodnoty.

| meranie (10× dlhšie) | výsledok |
|---|---|
| fill rate, textúra s lineárnym filtrom | 327 Mpix/s |
| malé trojuholníky (immediate mode) | 705 k/s |

gl-8: `CLEAR_BUFFERS` v 4 variantoch (`ffffff00`/`ffffffff`/`00ffffff`, zápis hĺbky zapnutý/vypnutý,
`STENCIL_MASK 0xff`), po každom spätné čítanie zety a kreslenie troch obdĺžnikov.

### Výsledok gl-8 — M2 splnené ✔

| `CLEAR_DEPTH_VALUE` | zápis hĺbky | zeta po clear | kreslenie |
|---|---|---|---|
| `ffffff00` | 1 | `00000078` | ✘ nič |
| `ffffffff` | 0 | `ffffff00` | ✔ |
| `ffffffff` | 1 | `ffffffff` | ✔ |
| `00ffffff` | 1 | `ffffffff` | ✔ |

`ffffff00` (Z24S8 ako v Mese) nefunguje; `ffffffff` vymaže hĺbku na 1.0 → `nv3d_clear_depth` ho
používa. Presný význam bajtov hodnoty (stencil) sa doladí, keď bude treba stencil.
Výkon: 327 Mpix/s, 666–705 k trojuholníkov/s.

## M3 — analýza `OpenGLRendererATI`

- `glrVendorIdentify`: `QAEngineGestalt(kQAGestalt_VendorID)` musí vrátiť **1** (ATI). Iný vendor →
  renderer engine ignoruje.
- `glrVendorInit`: `EngineID` (<5 Rage II/Pro podľa `kQAOptional_BlendAlpha`, 5/6/7 novšie čipy,
  inak typ 7), na kontexte `setInt(1050, 1)`, `getPtr(1021)`, `getInt(1011)` (777 = Rage Mobility).
- Kreslí cez štandardné metódy `TQADrawContext` (setInt/Float, DrawTri*, DrawVTexture,
  RenderStart/End, AccessDrawBuffer/ZBuffer, notice metódy…) plus **~60 súkromných ATI tagov
  1000–1100** v `setInt`/`setFloat` a čítanie `getInt` 1011, 1058, 1096–1100, `getPtr` 1021.
  Hlavička ATI RAVE rozšírení nie je verejne dostupná → význam tagov treba zistiť z volaní.

Postup M3: (a) **NV34 RAVE engine** podľa verejného RAVE 1.6 (`reference/rave/RAVE.h`,
`RAVESystem.h`, Apple „Empty Engine“) — hneď použiteľný pre RAVE/QD3D aplikácie; (b) engine
voliteľne hlási vendor ATI a loguje všetky súkromné tagy → postupná emulácia, kým
`OpenGLRendererATI` nebude kresliť; (c) ak by to bolo neprechodné, vlastná `OpenGLLibrary` (MiniGL).

### RAVE systém v OS 9 (súbory z 9.2.2, `reference/rave-os9/`, nedistribuovať)

- RAVE manager „QuickDraw™ 3D RAVE“ (fragment „QuickDraw™ 3D Accelerator“) načíta enginy =
  zdieľané knižnice typu **`shlb`/`tnsl`** v Extensions; ich CFM init rutina (`TtRegister`)
  volá `QARegisterEngine`. ATI enginy: „ATI 3D Accelerator“ (Rage II/Pro), „ATI Rage 128 3D
  Accelerator“, „ATI Radeon 3D Accelerator“, „ATI 8500 3D Accelerator“ — obsahujú implementáciu
  súkromných tagov 1000+ (zdroj pre emuláciu, ktorú chce `OpenGLRendererATI`).

## M3a — NV34 RAVE engine (R1)

- **NV34 QD Accel 1.6** zdieľa GPU (`accel/nv34_shared.h`, property `NV34ACCEL,shared`):
  `ensure3D` (3D init na požiadanie + obnova 2D cez `nva_restore_2d`), alokátor VRAM
  (4–24 MB), id kontextov, kruhový log 32 KB. Vypínač `nv34no3d` v boot-args.
- **NV34 RAVE** (`nv34rave/`, `shlb`/`tnsl`, init `TtRegister` nastavený cez
  `tools/pef.py setinit`): vendor `'NV34'`, kontexty na GDevice v Millions, DoubleBuffer
  (zadný buffer pod viditeľnou plochou s pitchom obrazovky → `RenderEnd` = 2D blit cez klip
  región), z-buffer Z24S8, Gouraud, textúry (všetky bežné formáty → A8R8G8B8 swizzled, mipmapy,
  filter Fast/Mid/Best, GL wrap), perspektíva cez projektívnu súradnicu `(u/w, v/w, 0, 1/w)`
  a `TXPH`, blending PreMultiply/Interpolate/OpenGL, bitmapy (RECT textúra), body/čiary,
  trimesh, notice metódy RenderCompletion/BufferInitialize, AccessDrawBuffer. Neznáme tagy
  a gestalty → log.
- **NV34 RAVE Test**: zoznam enginov, z-buffer, blending, textúra (asymetrická → overí
  poradie bitov swizzle), perspektíva, bitmapa, výkon; uloží `NV34 RAVE.txt` a log enginu
  `NV34 RAVE log.txt`.

### Výsledok rave-1/rave-2

- Engine sa registruje, RAVE ho ponúka vedľa „Apple Software“, kontext/textúra/bitmapa OK.
- Zadný buffer s pitchom obrazovky (4096) a zetou s pitchom 1920 → 3D kreslí rozsekane
  (stĺpce po 128 px). **Farba a zeta musia mať rovnaký pitch** (gl testy mali oba 4096).
  Zadný buffer s pitchom šírky kontextu (= pitch zety) + 2D blit z iného povrchu
  (`nva_copy_from`) → všetky kontrolné pixely správne (z-buffer, blend, textúra vrátane poradia
  bitov swizzle, bitmapa) v zadnom buffri aj na obrazovke.
- Výkon immediate mode: ~171 snímok/s × 2000 textúrovaných trojuholníkov = 343 k/s (CPU).
- UT: RAVE voľba nedostupná — `CheckDevice: GDevice 16 bpp odmietnuty` (UT hrá v Thousands).
  rave-3: 16 bpp kontexty (X1R5G5B5 + Z16, dithering podľa `kQAContext_NoDither`).

### Výsledok rave-3

- **16 bpp ✔**: všetky kontrolné pixely správne (bit 15 = 1), 199 snímok/s (398 k trojuholníkov/s).
- 32 bpp: prvý kontext po 3D inite nemal z-test (A, C, D chýbajú, blending bez z-testu OK) —
  rovnaký jav ako prvý `CLEAR_BUFFERS` v gl-7/gl-8. rave-4: pri novom kontexte / 3D inite
  naprázdno jeden CLEAR hĺbky.
- UT: proces sa spýta iba gestalt 0 a 1 (OptionalFeatures, FastFeatures) a engine nepoužije.
  rave-4 loguje každý dotaz s odpoveďou; boot-args `nv34raveati` hlási vendor ATI (1) /
  engine 5 — test, či UT vyžaduje ATI.

### Výsledok rave-4

- **32 bpp ✔** aj pri prvom kontexte po 3D inite (CLEAR hĺbky naprázdno pomohol) → RAVE engine
  R1 funguje v Millions aj Thousands.
- `nv34raveati` (vendor ATI): `OpenGLRendererATI` engine prijme — UT pri štarte cez OpenGL
  vytvorí kontext 32×32 na obrazovke a pýta `getInt 1011` (glrVendorInit). Potom sa v Finderi
  rozbilo prekresľovanie okien (príčina zatiaľ neznáma) → podvrh vendora zatiaľ nepoužívať.
- UT RAVE nevidí ani s vendorom ATI. UT CD (`System:Default.ini`): sekcia
  `[RaveDrv.RaveRenderDevice]` má **`NonATI=False`** → RaveDrv bez tohto prepínača akceptuje iba
  ATI engine (a zrejme kontroluje aj konkrétne ATI ID). Ďalší krok: `NonATI=True` a
  `GameRenderDevice=RaveDrv.RaveRenderDevice` v `UnrealTournament.ini`.

### UT s RAVE (rave-4 + `NonATI=True`)

UT („Rave engine found = NV34 RAVE Engine“) beží v 640×480×16 s double bufferom, plynulo
(11933 snímok, 16 M trojuholníkov). Chyby:
- menu a text: každá dlaždica/písmeno **hore nohami** → RAVE (QD3D) má počiatok textúry
  vľavo dole, `kQATexture_FlipOrigin` = vľavo hore. rave-5 to obracia (bitmapy ostávajú zhora).
- `textura 8x8: malo VRAM` → časť textúr chýba (zlé farby/textúry). UT nahráva RGB16/ARGB16
  (typ 1/2, flags `0x20000004`). rave-5: 16-bit textúry v A1R5G5B5/A4R4G4B4 (polovica VRAM),
  arena 4–31 MB (FP presunutý na `0x3F0000`), QD Accel 1.7.
- súkromný ATI tag `setInt 1022 = 0`, `getInt 1011`; `setNoticeMethod 2` (ReloadTextures).
- rave-5 loguje blend módy a textureOp (Highlight/Decal zatiaľ chýbajú) a v teste overí
  16-bit textúru (poradie texelov; `nv34tex16swap` v boot-args ich prehodí).

### Výsledok rave-5

- Test 5: všetko ✔ v 32 aj 16 bpp vrátane 16-bit textúry (poradie texelov správne, bez swapu).
- UT menu ✔ (orientácia textúr). Model hráča v menu chýba; v logu stále `malo VRAM` pre malé
  textúry (16×16, 16×2…) → pravdepodobne plná tabuľka blokov alokátora (512).
- UT blend módy: PreMultiply (1/303), OpenGL 306/300 (modulácia svetelnými mapami), 1/301,
  302/303 → GL enumy, fungujú. Súkromné ATI tagy 1022, 1035–1037 = 0.
- rave-6: QD Accel 1.8 (4096 blokov), textúry s kópiou v RAM a LRU výmenou vo VRAM.

### rave-6/7: model hráča v menu UT

- rave-6: VRAM už stačí (0 vyhodených textúr, 27 MB voľných), model stále chýba.
- rave-7 (log vrcholov): UT posiela **z = 1 − invW** (napr. z 0,993 / invW 0,007), model v menu
  je ďaleko (invW ≈ 0,000x) → z ≈ 1,0 alebo tesne nad → hardvér vrcholy orezal (depth clip).
  UT nastavuje ZFunction LE (3), TextureFilter Mid/Fast, Blend OpenGL 306/300, 1/301, 302/303.
- rave-8: z obmedzené na [0; 0,99998] a `DEPTH_CONTROL` = clamp (0x10) namiesto orezania.
- rave-8: model stále chýba. Hypotéza: 2D panely menu (z = 0, ZFunction LE, zápis hĺbky) majú
  v okne s modelom priehľadné texely (alfa 0), ktoré aj tak zapíšu hĺbku 0 → model za nimi.
  rave-9: alpha test GREATER 0 pri RAVE miešaní (PreMultiply/Interpolate).
- rave-11 (záznam snímky Caps Lockom): UT v menu nakreslí pozadie ako 12 dlaždíc 160×160
  s z = 0, invW = 1, ZFunction LE a zápisom hĺbky, z-buffer nemaže, potom model (z ≈ 0,93) →
  z-test ho zahodí. Tag 1022 sa prepína 0/1 len okolo priehľadného textu.
  rave-12: 2D primitívy (všetky vrcholy z ≤ 0 a invW = 1) hĺbku nezapisujú.
- rave-13 ✔ **model hráča v menu UT sa zobrazuje** (2D bez zápisu hĺbky). Záznam snímky menu:
  pozadie 12× RGB16 256×256 (z 0), text ARGB16 (blend OpenGL 1/301, 302/303), model 3 časti
  textúr 128×128 RGB16 (~380 trojuholníkov, z ≈ 0,93), potom ďalšie 2D prvky.
- rave-14: čistenie logu (bez vzoriek vrcholov); záznam snímky Caps Lockom ostáva.
- rave-15 (fotky + 2 záznamy): miznúce steny = **presvitá obloha**. UT kreslí najprv oblohu
  (z ≈ 0,999), potom svet s LE; z = 1 − 1/w, vzdialené steny (w 250–1000) sa v Z16 líšia od
  oblohy o menej než 1 krok (Δ1/w ≈ 1,5·10⁻⁵) → občas prehrajú. Softvérový clamp 0,99998 to
  ešte zhoršoval.
  rave-16: zadný buffer v Thousands so **Z24S8** (pitch farby = pitch zety = šírka × 4),
  bez softvérového clampu (len DEPTH_CONTROL); `nv34z16` vráti Z16. Test 6 pridal kontrolu G
  (z 0,999990 vs 0,999995).
- rave-16: obloha stále presvitá. Záznam vylučoval tag 1022 → nevidno ho okolo oblohy.
  V menu ho UT nastavuje na 0 okolo priehľadného textu (nemá zapisovať hĺbku) → 1022 je
  pravdepodobne **ATI „zápis hĺbky“**. rave-17: 1022 = 0 vypne zápis hĺbky; v zázname `zw`.
- rave-17: Test 6 v Thousands ✔ vrátane G → **Z24 pri 16 bpp funguje**. Tag 1022 potvrdený:
  UT ho dáva na 0 pri priehľadných vrstvách (oblaky oblohy, efekty), nie však pri základnej
  textúre oblohy (bl0, z ≈ 0,999, zw1). Obloha stále presvitá → UT po oblohe zrejme maže
  z-buffer inou cestou (podozrenie: AccessZBuffer, ktoré engine odmieta).
  rave-18: záznam všetkých volaní (AccessZBuffer, Flush, Sync, getInt/Ptr…).
- **Analýza UT (`reference/ut/UT.bin`, RaveDrv v aplikácii):** `URaveRenderDevice::Init` nastaví
  príznak „ATI“ iba ak `QAEngineGestalt(VendorID) == 1`; potom `getPtr(1021)` → tabuľka funkcií
  ATI. `ClearZ` (volá sa po oblohe) robí **iba** pre ATI `tabuľka[1](ctx, 0)`, inak nič → na
  ne-ATI engine obloha presvitá vždy. `Lock`: ATI `[0]` zmaže farbu, `[1]` z-buffer.
  `DownloadTexture`: ATI `[2](4, ?, &TQAImage, textúra)` nahradí obsah existujúcej textúry
  (inak Delete + New). ATI ďalej pýta gestalt 2000/2001, tagy 1000, 1010, 1020, 1053, 1054.
  rave-19: vendor ATI iba pre proces „Unreal…“ (alebo `nv34raveati`), `getPtr(1021)` vráti
  vlastnú tabuľku: [0] ClearColor, [1] ClearZ, [2] nový obsah úrovne 0, [3..39] log.
- rave-19/20: obloha ✔ (ATI[1] ClearZ). Zostali tmavé pásy/stĺpce: UT kreslí maskované textúry
  (ARGB16, alfa 1 bit) s OpenGL miešaním SRC_ALPHA/1−SRC_ALPHA a zápisom hĺbky, potom svetelnú
  mapu so ZFunction EQ → diery zapíšu hĺbku, mapa ich stmaví a zakryje, čo je za nimi.
  rave-21: alpha test (> 0) aj pri OpenGL miešaní so SRC_ALPHA.
- rave-21: previsy ✔. Tmavá stena = „modulated“ povrchy bez základnej vrstvy (DST_COLOR × ZERO,
  a = 1). UT ukladá svetelné mapy a modulované textúry v polovičnom jase a počíta s 2×
  moduláciou (OpenGLDrv: DST_COLOR × SRC_COLOR) — pre ATI zrejme cez tag 1020 = 1 (UT ho nastaví
  pri inite). rave-22: pri 1020 = 1 sa DST_COLOR × ZERO kreslí ako DST_COLOR × SRC_COLOR
  (`nv34no2x` vypne). Voda: ATI[2] (nový obsah textúry každú snímku) čakal na GPU →
  rave-22 strieda dve oblasti VRAM bez čakania.
- rave-22: bez zmeny. UT už posiela DST_COLOR × **SRC_COLOR** (0x306/0x300 = 2× modulácia),
  takže zmena pre 1020 sa neuplatní; tmavá stena je asi „modulated“ vrstva nad čiernym
  pozadím (stena pod ňou sa nekreslí) — porovnať so softvérovým rendererom UT.
  FPS: pri vode < 20, inde 30–50 (CPU: immediate mode).
- rave-23: vrcholy ako inline pole (`VERTEX_DATA` 0x1818 non-increment, `VTXFMT` 0x1740:
  pozícia 0, farba 3, textúra 8, float×4) — jedna hlavička na dávku; `nv34immediate` = staré.
  Nahrávanie textúr cez swizzle tabuľky (bez slučky po bitoch na texel).
- rave-23: voda ~30 fps (predtým < 20), inde 30–50; Test 7 E: 186 snímok/2 s. Tmavá stena je aj
  v softvérovom rendereri UT → vlastnosť mapy.
- Štartovací dialóg UT (`IsRageProAvail`): prepne na 16 bpp, hľadá engine s vendorom 1,
  vytvorí kontext 32×32 a `getInt(1011)`: < 772 → RAVE nedostupné; EngineID×100 + Revision
  < 434 → hláška o starom ovládači ATI; ≥ 1280 → vráti 2. rave-24: pre UT `getInt(1011)` = 1280.
- rave-24 ✔: RAVE sa dá vybrať v štartovacom dialógu UT. **M3a (RAVE engine pre UT) hotové.**

## M4 — Apple OpenGL cez NV34 RAVE (OpenGLRendererATI), cieľ Quake II

- `OpenGLRendererATI` volá tabuľku `getPtr(1021)` na pozíciách 8, 9, 12–16, 19, 21, 23–30
  (návratové hodnoty väčšinou nepoužíva; 30 vracia ukazovateľ kontrolovaný na 0, 16 vracia
  výsledok cez pamäť). UT používa iba 0–2.
- gl-q2-1: vendor ATI aj pre procesy „Quake…“, log mena procesu pri registrácii; všetky
  neznáme tagy/hodnoty, gestalty a pozície tabuľky sa logujú.
- gl-q2-1: proces „Quake 2…“ dostal vendor ATI, ale ATI renderer engine nevybral (žiadny
  gestalt 2/kontext). `glrGetFullScreenRendererInfo` volá `QADeviceGetFirstEngine` so
  zariadením {GDevice, NULL}; engine ho v CheckDevice odmietal. gl-q2-2: prijať.
- gl-q2-2: bez zmeny (CheckDevice s NULL sa nevolá). ATI renderer má dve slučky: pre obrazovku
  (`QADeviceGetFirstEngine({GDevice, gd})`) berie iba enginy s `kQAOptional_BoundToDevice`,
  „full screen“ s NULL iba bez neho. gl-q2-3: BoundToDevice pre ATI procesy.
- gl-q2-3: ATI renderer engine vzal (BoundToDevice), potom pád Quake II (chyba 3) po gestalte
  1002. `glrVendorBoardMemory`: gestalt 1001 = VRAM (vstup = zariadenie). `glrSetConfigData`:
  gestalt 1002 vracia **ukazovateľ na tabuľku funkcií**, volá [8](blk{1, zariadenie}, &x) →
  pole 124 konfigurácie (asi VRAM), [7](blk, &flagy) bity 0–2; gestalt 1005 bit 0, gestalt 11.
  gl-q2-4: gestalty 1000–1010 vždy odpovedajú (1001 VRAM, 1002 tabuľka [7]=7, [8]=VRAM, inak 0).
- gl-q2-4: Quake II zamrzol na bielej obrazovke (log v RAM sa stratil). gl-q2-5: v procese
  Quake log aj do súboru `Preferences:NV34 RAVE live.txt` (každý riadok zapísaný a FlushVol),
  sled volaní enginu (prvých 600: kontext, RenderStart/End, Sync, Busy, Access, textúry…).
- gl-q2-5 (live log): ATI renderer vytvorí kontext 640×480×32 (flags 0x80000004), tagy 1000,
  1050, 32, 1053, 1054, 1099, getInt 1058, setPtr 26 (multitextúra), notice 1 a 2, tabuľka
  [23] (začiatok snímky: (ctx, tabuľka, 0x280, …), potom príznak „kreslím“), [20], [28];
  textúry typ 8 (RGB24) 8×8 mipmap a 256×256 → potom zamrznutie bez ďalšieho volania.
  gl-q2-6: RGB24 textúry, každé volanie tabuľky a kroky TextureNew v live logu.
- gl-q2-6: textúra prešla celá (upload ok, VRAM), potom ATI[20] a zamrznutie.
  `glrVendorGetDispatchTable`: [20](ctx, flagy, …) → bit 0 výsledku = príznak; potom kopíruje
  z tabuľky pozície 31–60 (bajty 124–240) do dispatch tabuľky kreslenia — naša tabuľka mala
  40 položiek → skok do smetí. gl-q2-7: tabuľka 80 položiek (ďalšie pozície = log).
- gl-q2-7: Quake II beží niekoľko snímok cez ATI renderer: [23]/[24]/[25]/[29] rámec snímky,
  kreslenie cez pozície 48, 50, 52, 54 (stovky volaní), 58–60 s (rctx, vrcholy, počet, ?),
  textúry aj súkromného typu 1006 (32-bit), potom [16](0x44, 0, 8, 0) a pád (16 vracia výsledok
  cez pamäť). Kresliace pozície sú vstupy GL dispatch tabuľky (prvý argument = kontext ATI
  renderera, +8 = RAVE kontext), formát vrcholov Apple GL enginu nie je zdokumentovaný;
  ATI RAVE enginy sú bez mien funkcií. Odhad: ~30 funkcií na spätnú analýzu.
  Alternatíva: vlastná `OpenGLLibrary` (MiniGL) v priečinku Quake II — CFM ju nájde skôr ako
  systémovú, hardvérová transformácia NV34 (MODELVIEW/PROJECTION) priamo z GL matíc.

## M4 NV34 MiniGL — vlastná OpenGLLibrary (`minigl/`)

Cesta cez ATI renderer Apple GL opustená (nezdokumentovaný dispatch, ~30 funkcií). Namiesto
toho vlastná zdieľaná knižnica s menom `OpenGLLibrary` (typ `shlb`), ktorá sa položí do
priečinka hry: CFM hľadá importované knižnice najprv v priečinku aplikácie, takže hra (Quake II
`ref_gl.lib` importuje 346 symbolov z `OpenGLLibrary`, bez požiadaviek na verziu) dostane našu
knižnicu, ostatné programy ďalej Apple OpenGL.

- Exportuje všetkých 474 symbolov Apple OpenGLLibrary 1.2 (zoznam `minigl/apple_exports.txt`,
  len mená). Neimplementované generuje `gen_stubs.py` podľa `nm` (prvé volanie → log, vracia 0).
- `minigl.c`: GL 1.1 stavový stroj. Matice a zásobníky, glBegin/glEnd (všetky primitívy,
  pásy/vejáre aj cez viac ako 1024 vrcholov), MVP na CPU, orezanie blízkou rovinou a
  ochranným pásom 4× (Sutherland–Hodgman), odvrátené steny, flat shading, viewport/DepthRange
  na CPU, viewport ∩ scissor ako hardvérový scissor. Dávkovanie: trojuholníky sa zbierajú,
  kým sa nezmení stav (každá stavová funkcia volá FLUSH). Textúry: úrovne v RAM ako ARGB32
  (RGB/RGBA/BGRA/L/LA/A/I, UNPACK_ALIGNMENT/ROW_LENGTH), do VRAM pri kreslení (swizzled, mip
  reťazec len pri mipmap filtri, LRU vyhadzovanie, ping-pong pri zmene počas snímky),
  TexEnv MODULATE/REPLACE/DECAL. Vertex arrays (DrawArrays/DrawElements/ArrayElement),
  glGet*, ReadPixels (snímky obrazovky), PushAttrib/PopAttrib (celý stav).
- `agl.c`: pixel formát, renderer info (ACCELERATED, VIDEO_MEMORY = VRAM), kontext,
  drawable = okno (CGrafPtr) → zadný buffer vo VRAM (farba + Z24S8, rovnaký pitch),
  aglSwapBuffers = 2D blit cez visRgn okna. Fullscreen/offscreen zatiaľ nie.
- `hw.c`: z NV34 RAVE enginu — render target, combiners, inline vrcholy, swizzled textúry, blit.
- Log: `Systémová zložka:Preferences:NV34 MiniGL log.txt` (každý riadok hneď na disk).
- Inštalácia: `OpenGLLibrary` (z `OpenGLLibrary.bin` / disku `NV34 MiniGL`) do priečinka
  Quake II vedľa aplikácie. Odstránenie súboru = späť na Apple OpenGL.

### Testy
- minigl-1: prvá verzia pre Quake II.
- minigl-1: Quake II ostal v softvéri, MiniGL log nevznikol v Quake. NV34 GL Check (nový
  program `nv34glcheck/`: GetSharedLibrary + FindSymbol + GetDiskFragment ref_gl.lib s menom
  chýbajúceho symbolu): knižnica z priečinka hry sa načíta (init zaloguje), ale `glGetString`
  → -2802 cfragNoSymbolErr. Príčina: MakePEF zapisuje exporty nezoradené podľa slotov hash
  tabuľky (pri 476 exportoch 64 slotov) → CFM nenájde takmer nič, ref_gl.lib sa neprepojí.
  minigl-2: `pef.py fixhash` exporty preusporiada (overené prepočtom hash tabuľky).
- minigl-2: **Quake II beží cez MiniGL hardvérovo** (1000–3400 trojuholníkov/snímku, 5–77 dávok,
  VRAM ~20 MB voľná). Chyba: občas nepriehľadný trojuholník cez pol obrazovky = R_PolyBlend
  (štvoruholník 10 jednotiek pred kamerou, ±100). Simulácia: po orezaní pásom 4× vrcholy
  -960…1600 px; vykreslená len jedna polovica, nepriesvitne → NV34 rasterizér také súradnice
  nezvláda. minigl-3: ochranný pás 1,01 (orezanie tesne za viewportom).
- minigl-3: trojuholník stále → nie orezanie. Príčina: `emit_poly` určil farbu (REPLACE → biela,
  alfa 1) podľa textúry predchádzajúcej dávky; prvá polovica záblesku po svete (GL_REPLACE) bola
  biela nepriehľadná, druhá správne priesvitná. minigl-4: textúra podľa aktuálneho stavu,
  ochranný pás späť 4×.
- minigl-4: záblesk správny, ale čierna čiara na uhlopriečke (spoločná hrana dvoch obrích
  trojuholníkov). **timedemo demo1: 35,4 fps** (640×480×32). minigl-5: ochranný pás 1,01.
- minigl-5: čiara preč, **Quake II plne funkčný cez NV34 MiniGL, timedemo demo1 34,2 fps**
  (pás 1,01 stojí ~1 fps). Ďalej: Half-Life (aglSetFullScreen, hmla, dynamické načítanie GL).

### Optimalizácia (opt-1 → minigl-6)
- glTexSubImage2D: ak textúra má vo VRAM jednu úroveň a GPU ju v tejto snímke ešte nečítal,
  zapíše sa iba zmenený obdĺžnik priamo na miesto (`hw_tex_subupload`); predtým každá zmena
  lightmapy = celé nahratie 64 KB (+ ping-pong / čakanie na GPU).
- Vrcholy do push buffra priamo (miesto rezervované v `nva_begin`), bez volania na každé slovo.
- Log každých 500 snímok: počet celých/čiastočných nahratí, KB a čakaní na GPU.
- minigl-6: timedemo 35,7 fps (+1,5). Log: nahrávanie textúr zanedbateľné (stovky za 500 snímok,
  ~10 čakaní na GPU). Push buffer len 32 KB (8191 slov) na konci VRAM, snímka Quake II ~430 KB
  → ~13 pretočení za snímku, CPU čaká na GPU, malý súbeh.
- opt-2 (minigl-7 + **NV34 QD Accel 1.9**): push buffer 512 KB na 0x400000 (`NVAccel.dmaSize`
  na konci štruktúry kvôli kompatibilite klientov), 3D aréna od 0x480000. MiniGL meria časy
  (mftb, kalibrácia cez TickCount): každých 250 snímok fps a ms/snímku pre glEnd (CPU geometria
  + dávky), dávky, čakanie na push buffer/sync a swap.
- minigl-7 + QD Accel 1.9: **timedemo 46,7 fps** (z 35,7). Mimo dema 120–150 fps. V deme ~50 fps:
  20 ms/snímku, glEnd ~9,5 ms (dávky 3,3, čakanie 1,9) → CPU geometria ~5–6 ms na ~2900
  trojuholníkov (~2 µs/troj.), zvyšok ~10 ms Quake.
- opt-3 (minigl-8): rýchla cesta pre trojuholníkové primitívy celé v ochrannom páse (bez flat):
  outcode a premietnutie každého vrcholu raz, pri trojuholníku len test frustumu, plocha, cull
  a kópia. Log: %02u s nulami.
- minigl-8: **timedemo 51 fps**. Statická scéna: glEnd 4,9 ms pri 1800 troj. (dávky 0,7) → stále
  ~2,3 µs/troj. na CPU. Disassembly: kópie štruktúr vrcholov (HwVtx 40 B, V 44 B) idú cez
  vlastný `memcpy`, ktorý kopíroval po bajtoch.
- opt-4 (minigl-9): `memcpy` po slovách (aj v NV34 RAVE).
- minigl-9: **timedemo 54,1 fps**. Statická scéna glEnd 3,8 ms (z 4,9). V deme glEnd 5–6 ms, z toho
  čakanie na GPU 3–4,5 ms → teraz čaká CPU na GPU; Quake sám ~13 ms/snímku.
  UT s novým RAVE plynulejší, ale príliš svetlý. Od rave-24 sa pre UT (vendor ATI) zmenilo aj
  gl-q2: ATI gestalty 1000–1010 a BoundToDevice. minigl-10 (len NV34 RAVE): tie len pre Quake
  (`gAtiGL`), UT ako v rave-24.
- minigl-11 (live log UT): UT posiela súkromné ATI tagy 1012–1016, 1019, 1039 a setPtr 1018
  (svetelná mapa ako druhá textúra) → engine ich ignoruje, hra bez tieňov. `URaveRenderDevice::
  Unlock`: multitextúra ak getInt(1011) > 772 (+ ďalšie podmienky). Chyba z rave-24 (1011 = 1280
  kvôli štartovaciemu dialógu, ktorému stačí ≥ 772). minigl-12: UT 772, Quake 1280.
- minigl-12: **UT svetlo správne** (tiene späť).
- minigl-13: diagnostika len na požiadanie cez boot-args — NV34 RAVE: `nv34filelog` (log do
  súboru), `nv34trace` (Caps Lock záznam snímky; bez neho sa GetKeys nevolá); MiniGL:
  `nv34glstats` (časy každých 250 snímok). Bez nich RAVE píše len do zdieľaného RAM logu
  a MiniGL do súboru iba pri štarte/konci.

## M5 — Half-Life (Xash3D FWGS, SDL2 „macosclassic“) cez NV34 MiniGL
- Jedna PEF aplikácia (8 MB), ref_gl staticky. `OpenGLLibrary` cez GetSharedLibrary + FindSymbol
  („Classic Mac OS supports only OpenGLLibrary“, „OpenGLLibrary is missing %s“). AGL:
  ChoosePixelFormat, DescribePixelFormat (ACCELERATED, RENDERER_ID, PIXEL_SIZE…), Create/Destroy/
  Update/SetCurrent/GetCurrentContext, SetDrawable, SwapBuffers, SetInteger(SWAP_INTERVAL),
  GetError/ErrorString, aglSetFullScreen (ak zlyhá → okno). GL 1.1 fixed function, rozšírenia
  voliteľné (GL_ARB_multitexture atď.).
- hl-1: glGetTexLevelParameter (aj GL_PROXY_TEXTURE_2D), glDrawRangeElements, stencil bez
  účinku, glBlendEquation (ADD), glTexGen/glClipPlane ignorované s logom.
- hl-1: kontext 640×480 16 bpp v okne (192,144) na 1024×768, prvé snímky (758 troj., 72 dávok,
  ~16 MB textúr), potom „pokus o fullscreen“ a zamrznutie celého Macu; log končí (po 3. snímke
  sa nelogovalo). hl-2: blit orezaný aj na hranice obrazovky; pri timeoute push buffra MiniGL
  prestane kresliť (predtým písal ďalej za rezervované miesto); log zmien okna/obrazovky,
  aglUpdateContext, vynechaných present a snímok každých 100 (do 2000).
- hl-2: zamrznutie pri prepnutí na celú obrazovku (fade do čiernej → Mac stojí); log MiniGL po
  300 snímkach nič (žiadna zmena okna). Half-Life v okne (`fullscreen 0`) funguje a kreslí
  správne; Quake II zmena rozlíšenia za behu funguje. Fullscreen HL ide cez DrawSprocket
  (aglSetFullScreen sa nevolá). hl-3: podrobný log pre proces Half-Life (boot-args
  `nv34gldebug` pre iné): každý swap (začiatok / ensure3D / blit / hotovo), glViewport,
  glFinish, glReadPixels, aglSetCurrentContext.
- hl-3: posledný riadok „swap 311: hotovo“ → zamrznutie mimo MiniGL, pri prepnutí režimu cez
  DrawSprocket počas 3D (v push buffri až 512 KB príkazov). Quake II pred prepnutím ruší kontext
  (hw_sync → GPU nečinné). hl-4: **NV34 NDRV 4.0.3 rev 1** pred `nv_set_mode` čaká (max. 250 ms), kým
  DMA GET = PUT kanála 0 a PGRAPH_STATUS = 0.
- hl-4 (NDRV čaká na GPU): bez zmeny. S Apple OpenGL (bez MiniGL) zamrzne tiež, fade 2× →
  problém DrawSprocket + NV34 NDRV, nie 3D. HL používa DSpContext_FadeGammaOut/In,
  GetMonitorFrequency, FindBestContextOnDisplayID, SetState (Quake II bez fade). hl-5: program
  **NV34 DSp Test** (`nv34dsptest/`): kroky DSp s logom a FlushVol pred/po každom.
- hl-5 (DSp Test): žiadne zamrznutie; frekvencia 60 Hz, fade OK, ale `SetState(Active)` →
  -30448 kDSpConfirmSwitchWarning: škálované režimy nemali kModeSafe (zámerne kvôli potvrdeniu
  v Monitors). hl-6: **NV34 NDRV 4.0.3 rev 2** — všetky režimy kModeSafe.
- hl-6: DSp Test bez varovania. Half-Life: prepne na 640×480, biele okno, Mac stojí (myš ani
  klávesy). MiniGL log: len načítanie (3D stav 0) a ukončenie — žiadne AGL volanie → HL zrejme
  skončil s chybou ešte pred GL (po úspešnom DSp). Log sa pri každom načítaní mazal → hl-7:
  pripája (maže až nad 256 KB), oddeľovač „====“.
- hl-7: **Half-Life na celej obrazovke v natívnom 1024×768 funguje** cez MiniGL (zadný buffer
  1024×768×16, ~16 MB textúr, VRAM voľná ~1,6 MB). HL načíta OpenGLLibrary najprv na kontrolu,
  zatvorí a znova načíta. Pri prepnutí na 640×480 HL skončí ešte pred druhým načítaním (aj s Apple
  OpenGL) → chyba prepínania režimu v porte HL/SDL, obchádzka: natívne rozlíšenie.
  hl-8: podrobný log len s boot-args `nv34gldebug`.
- hl-8: hra v poriadku, ale decaly blikajú pri pohybe (glPolygonOffset ignorovaný → z-fighting),
  s baterkou čierne štvorce (VRAM ~1,6 MB voľná, textúra mimo VRAM → kreslené bez textúry →
  pri prechode svetelnej mapy čierne), FPS miestami < 30 (640×480).
  hl-9: polygon offset na CPU (sklon z okna × factor + 2⁻²⁰ × units, GL_POLYGON_OFFSET_FILL);
  pri 16 bpp obrazovke 16-bit textúry (A1R5G5B5, A4R4G4B4 pri čiastočnej alfe); pri nedostatku
  VRAM sa dávka nekreslí.
- hl-9: decaly ✔. VRAM voľná ~16 MB (16-bit textúry). Baterka stále čierne štvorce → GPU
  zaostáva (push buffer 512 KB) a čiastočné nahratie prepisuje svetelnú mapu, ktorú GPU ešte
  kreslí v predošlej snímke. Výkon: 36–75 fps pri 640×480, MiniGL 1–8 ms zo 13–27 ms.
  hl-10: GPU fence cez REF_CNT (metóda 0x50, USER+0x48): present pošle číslo snímky; textúra
  sa prepíše na mieste len ak REF ≥ snímka posledného čítania (inak druhá oblasť / sync);
  pri výmene oblastí sa vymenia aj snímky posledného čítania.
- hl-10: čierne štvorce ✔ (fence OK). Ďalší level ~25 fps: 40 ms/snímku, z toho MiniGL ~10 ms
  (push zápis 3–4,5, čakanie 1–1,7), Half-Life ~30 ms.
- hl-11: na celej obrazovke (okno = gdRect) blit celého okna bez visRgn (panel menu a control
  strip boli vynechané) + HideMenuBar/ShowMenuBar (FindSymbol v InterfaceLib) a Control Strip
  cez _ControlStripDispatch 0xAAF2 (SBIsControlStripVisible 0x0000, SBShowHideControlStrip
  0x0101, CallUniversalProc); obnova pri odpojení drawable, zrušení kontextu a ukončení.
- hl-11: skrytie aj obnova fungujú (control strip cez 0xAAF2 OK), ale HideMenuBar v InterfaceLib
  nie je → panel menu len prekreslený. hl-12: HideMenuBar aj v MenusLib, inak MBarHeight = 0
  (obnova pôvodnej výšky + DrawMenuBar).
- hl-12 ✔: HideMenuBar/ShowMenuBar nájdené v **MenusLib**; panel menu aj control strip skryté
  a obnovené, klik hore ide do hry. **Half-Life plne funkčný cez NV34 MiniGL** (celá obrazovka
  v natívnom rozlíšení; prepínanie na nižšie rozlíšenie padá v porte HL aj s Apple OpenGL).

## M6 — Quake III (MacQuake3, Mac OS 9) cez NV34 MiniGL
- q3-0: „User Interface is version 4, expected 3“ = stará aplikácia 1.1x s novšími pak*.pk3
  (nie GL). Po úprave dát: GL kontext 640×480×16 (pýta stencil 8 → 0), menu a písmo OK, ale
  obrázky levelov a takmer všetky steny biele; ~1 sync/snímku (asi glFinish); glClipPlane.
- q3-1: diagnostika textúr cez boot-args `nv34gltex`: TexImage2D (rozmery, formáty, typ),
  príprava pre GPU (úrovne, filter, 16-bit formát, prvý texel), prvé dávky (textúra, env,
  blend, alfa, z, farba a súradnice prvého vrcholu), glEnableClientState, glDrawElements.
- q3-3 (log textúr): textúry sveta nahraté, ale nikdy pripravené — svet kreslený s „tex 0“
  (texturovanie vypnuté). Q3 je s OpenGLLibrary prepojený staticky → glActiveTextureARB existuje
  (stub) aj bez rozšírenia; GL_SetDefaultState: SelectTexture(1), glDisable(GL_TEXTURE_2D),
  SelectTexture(0) → vypol jedinú jednotku. q3-4: aktívna jednotka (glActiveTextureARB,
  glClientActiveTextureARB), stav jednotky ≥ 1 (enable, bind, texEnv, texcoord pole,
  glMultiTexCoord) uložený zvlášť; nahrávanie/parametre do textúry aktívnej jednotky.
- q3-5/6: glActiveTextureARB sa vôbec nevolá, GL_TEXTURE_2D ostáva zapnuté, naviazaná textúra
  má dáta (lv0 v systémovej halde!) a predsa dávka „tex 0“ → `tex_prepare` vrátil 0, lebo
  `hw_tex_new` (NewPtrClear v halde aplikácie) zlyhal — Quake III si zaberie takmer celú haldu.
  q3-7: hw_tex_new aj zo systémovej haldy; kópie textúr v RAM: halda aplikácie → dočasná pamäť
  (TempNewHandle, zamknutá) → systémová halda (16 B hlavička s typom). Diagnostika textúr opäť
  len cez boot-args `nv34gltex`.
- q3-7 ✔: **Quake III (MacQuake3) cez NV34 MiniGL vyzerá v poriadku** (menu aj hra). Otvorené:
  ~1 hw_sync/snímku (pravdepodobne glFinish — r_finish), glClipPlane (zrkadlá/portály)
  ignorované, stencil 0, multitextúra nehlásená (Q3 kreslí svetelné mapy druhým prechodom).

### Optimalizácia opt-6 (Quake III, všetky hry)
- Vrcholy: pozícia 3×float, farba 4×U8_UNORM v jednom slove (poradie R|G<<8|B<<16|A<<24,
  boot-args `nv34glcolorbe` opačne), textúra 4×float → 8 slov s textúrou / 4 bez (predtým 12/8);
  `nv34glvtx12` = pôvodný formát.
- glFinish čaká cez fence len na predošlú snímku (aktuálna beží ďalej); bez fence hw_sync.
- Uvoľnenie VRAM textúr (glDeleteTextures, zmena veľkosti) odložené do dokončenia ich poslednej
  snímky (zoznam 512, spracovaný pri present, pri nedostatku VRAM a pri ukončení).
- opt-6: farby s ružovým nádychom (poradie R|G<<8|B<<16|A<<24 zlé — alfa v červenom kanáli),
  Quake II timedemo 53 fps (bez zisku, ~54 predtým), VRAM pri ukončení 27136 KB (bez úniku).
  opt-7: farba R<<24|G<<16|B<<8|A predvolene (`nv34glcolorle` = opačne).
- opt-7: farby ✔. **Quake III timedemo 31 fps.** Štatistika v hre: 32 ms/snímku, MiniGL ~8 ms
  (geometria ~5–6, dávky 2–3), Quake III ~24 ms; menu/intro GPU-bound (čakanie 7–8 ms).
  Raz po studenom štarte „hw_sync: GPU nedobehol do 2 s“ a zamrznutie hneď po spustení Q3
  (neopakovalo sa) — sledovať.
- opt-8: GL_EXT_compiled_vertex_array (glLockArraysEXT/glUnlockArraysEXT) + rýchla cesta
  glDrawElements(GL_TRIANGLES): pozície transformované, outcode a premietnutie raz na vrchol
  (cache 2048, pri zámku spoločná pre všetky prechody povrchu, neplatná pri zmene matíc,
  viewportu, DepthRange, RT alebo ukazovateľa poľa); farby/súradnice textúr na index.
- opt-8: **Quake III timedemo 37 fps** (z 31, +19 %), Quake II 53 fps, obraz OK. V hre 25–30 ms/
  snímku, MiniGL dávky 2,5–5 ms (z toho čakanie na GPU 1–4 ms), zvyšok Quake III; menu/intro
  GPU-bound (~9 ms čakanie). glEnd ≈ 0 (Q3 kreslí cez glDrawElements).

## Uzavretie fázy 4 (release)
- `make release` → `dist/NV34 Release.dsk`: priečinky *Do Extensions* (NV34 NDRV 4.0.4, NV34 QD
  Accel 1.9, NV34 RAVE 1.1), *Do priecinka hry* (OpenGLLibrary = NV34 MiniGL 1.0), *Nastroje*
  (NV34 Info, GL Check, DSp Test, RAVE Test) a *Citaj ma* (z `docs/INSTALL.md` bez diakritiky,
  `tools/mkreadme.py`). Verzie RAVE/MiniGL cez `VERS`/`VERS_INFO` pre `tools/macres.py`.
- MiniGL bez boot-args loguje len načítanie, kontext, zadný buffer, chyby a ukončenie.
- Návod: [INSTALL.md](INSTALL.md). Ďalšie možné kroky: multitextúra (Q3/HL), stencil, glClipPlane,
  občasné zaseknutie GPU pri prvom spustení Quake III po studenom štarte.

## M7 — Return to Castle Wolfenstein (SP) cez NV34 MiniGL
- wolf-0: menu OK (~37 fps), pri načítaní novej hry Mac zamrzol; v logu nič (ani GPU timeout).
  Podozrenie: kópie textúr v RAM (32-bit + mipmapy) minú haldu aplikácie aj dočasnú pamäť a
  `NewPtrSys` nafúkne systémovú haldu → Mac OS 9 stojí.
- wolf-1: veľké bloky len z dočasnej pamäte (rezerva 16 MB pre systém) alebo haldy aplikácie
  (rezerva 2 MB), nikdy zo systémovej haldy — inak textúra nebude (log s voľnou pamäťou);
  každých 16 MB textúr riadok do logu. Pri 16-bit obrazovke kópia v RAM rovno 16-bit
  (A1R5G5B5 / A4R4G4B4 podľa alfy úrovne 0, `TexObj.sfmt`), nahrávanie do VRAM bez konverzie.
- wolf-1 ✔: hra beží (pamäť aplikácie 256 MB; aj stará verzia s väčšou pamäťou), textúry OK,
  Quake III s 16-bit kópiami OK. GL_FOG nepodporená (Wolfenstein ju zapína). V pomalých
  miestach 16 fps (61 ms): dávky ~3 ms, čakanie na GPU ~0 — ale rýchla cesta glDrawElements
  sa nemerala. wolf-2: štatistika aj pre glDrawElements (rýchla / pomalá cesta a jej čas).
- wolf-2: v hre 40–57 ms/snímku, MiniGL ~10–12 ms (z toho dávky ~2, čakanie na GPU ~0),
  ~3900 troj. v ~97 glDrawElements, všetky rýchlou cestou → ~2,5 µs/troj. Podozrenie: farby
  UBYTE → float → UBYTE (prevody int↔float na G4 drahé) a príprava vrcholu pre každý index.
  wolf-3: HwVtx.c = zabalená farba (R<<24|G<<16|B<<8|A) z poľa bez prevodu, vrchol (okno, s/q,
  farba) pripravený raz za volanie (gLV), hw_draw zapisuje c priamo.
- wolf-3: Q3 OK; Wolfenstein glEnd 8–10 ms (z 10–12), dávky ~1 ms — stále ~2,5 µs/troj.
  Príčina pravdepodobne: rýchla cesta prepočítala celý rozsah indexov (min…max) pri každom
  volaní, hoci Wolfenstein kreslí malé kúsky z veľkého poľa. wolf-4: pozícia aj atribúty vrcholu
  až pri prvom použití (značky generácie gPStamp/gAStamp). Repo pod gitom (prvý commit).
- wolf-4: bez zlepšenia (glEnd 8–10 ms pri ~3200–3700 troj.). Podozrenie: ochranný pás 1,01
  (od minigl-5) → každý trojuholník presahujúci okraj ide cez clip_poly + emit_poly. wolf-5:
  štatistika orezaných trojuholníkov a ich času, pás cez boot-args `nv34glguard2` / `nv34glguard4`.
- wolf-5: orezávanie ~1,2 ms (pás 1,01) vs 0,3 ms (pás 2 — v Q2 sa vráti čierna čiara) → pás
  ostáva 1,01. Doterajšie „samotná hra“ = všetko mimo glEnd/DrawElements/swap, teda aj ostatné
  GL volania. Porovnanie: G4 1,25 GHz s Q3 v 640×480 ~87 fps (OS X, novšia verzia hry).
- q3-8: celkový čas vo všetkých funkciách MiniGL (CHECK → atribút cleanup, hĺbka vnorenia) v
  štatistike „MiniGL spolu“; log taktu GPU (NVPLL/MPLL) pri inicializácii.
- q3-8: **MiniGL spolu 15–19 ms z 27–32 ms** v hre (glEnd 6–7, dávky ~1, orezávanie ~1,5) — zvyšok
  ~8 ms pravdepodobne čakanie v glFinish (nepočítané do „čakanie GPU“); aj prázdne snímky
  (190 fps) 4,4 ms. **PLL: jadro 0x13b04 → ~99 MHz** (M 4, N 59, P 1, kryštál 13,5 MHz; COEFF_B
  0x401 nejasné), pamäť 0x30000f01 → ~202 MHz. Timedemo 34 fps.
- q3-9: čas a počet glFinish v štatistike; `nv34glnofinish` = glFinish bez čakania (test).
- q3-9: 34,8 / 34,5 fps (nv34glnofinish sa neuplatnil — glFinish aj v 2. behu ~10 ms).
  **glFinish čaká 8–11 ms/snímku** aj v menu → GPU je úzke miesto (snímka GPU ≈ snímka CPU);
  MiniGL na CPU ~7–9 ms, Quake III ~10–12 ms. Jadro ~99 MHz, pamäť ~202 MHz.
- q3-10: Z16 pri 16 bpp, ak hra pýta hĺbku ≤ 16 (Quake III: AGL_DEPTH_SIZE 16) — polovičný
  prenos pri teste hĺbky, farba aj zeta pitch 2 B/px; polygon offset r = 2⁻¹⁵ pre Z16;
  `nv34glz24` = vždy Z24S8.
- q3-10: **Quake III timedemo 48 fps** (z 34,8). glFinish v hre 0,1–0,8 ms (GPU už nebrzdí,
  v menu ~5 ms). CPU: MiniGL ~8–9 ms (rýchla cesta ~5, orezávanie ~1,5, dávky ~1), Q3 ~12 ms.
- q3-11: rozpad rýchlej cesty: počet trojuholníkov, výpočtov pozícií a atribútov a ich čas.
- q3-11 výsledok: v hre (≈4300 troj./snímku) pozície 6400 vrcholov ≈ 2,0 ms, atribúty ≈ 0,6 ms;
  zvyšok rýchlej cesty ≈ 2 ms (indexy, skladanie, kópia do dávky), dávky do push buffra ≈ 1,2 ms.
  Bez multitextúry Quake III kreslí každý povrch s lightmapou dvakrát.
- q3-12: **GL_ARB_multitexture** (2 jednotky, jednotka 1 len GL_MODULATE). Fragment program
  `TXP R1 ← t[1]; TXPH H0 ← t[0]; MULH H0 = H0·R1` na fpOffset + 0x100, combiners × primárna
  farba ako pri jednej textúre. Vrchol 12 slov (tex1 = atribút 9), TEX_UNITS_ENABLE = 3, blok
  textúry 1 na 0x1a20. Pri nahrávaní textúry jednotky 1 sa nevyhadzuje textúra jednotky 0
  (gTexPin). boot-args `nv34glnomt` = bez multitextúry. Merače času z vnútornej slučky preč.
- q3-12 výsledok: multitextúra funguje (v hre 500–1000 troj./snímku s 2 textúrami, obraz v poriadku),
  timedemo 50 fps (z 48). V hre ~20 ms/snímku: MiniGL ~8 ms, samotný Quake III ~12 ms — hra je
  teraz väčšia časť. q3-13: meranie skutočného taktu CPU (podozrenie na zníženú rýchlosť procesora).
- q3-14 výsledok: rozdielové meranie ~750 MHz, device tree hlási clock-frequency 1 GHz, zbernica
  132,7 MHz, cpu-version 0x80010201 (MPC7455), `dynamic-power-step` prítomné (prepínanie rýchlosti
  cez PMU). Procesor teda beží na zníženom takte.
- q3-15: Power Manager (PowerMgrLib) — log rýchlostí; boot-args `nv34cpufull` / `nv34cputoggle`
  skúsia SetProcessorSpeed a takt znova zmerajú.
- q3-15 výsledok: Power Manager hlási aktuálna = max = min = 1000 MHz, plná rýchlosť, cycling 0.
  SetProcessorSpeed(true) vráti 0, (false)/(true) vrátia 1, zmeraný takt ostáva ~750 MHz (692–767).
  Mac OS 9 o zníženom takte nevie, cez Power Manager sa preto nedá zmeniť. Skutočné prepnutie robí PMU
  príkaz 0x7d s resetom CPU (ako uspanie, Linux pmu_set_speed), z knižnice hry to nie je bezpečné.
  Ďalej: HW T&L, aby CPU robil menej.
- q3-16: **HW T&L** v rýchlom glDrawElements (boot-args `nv34gltnl`): vrcholy v súradniciach
  objektu, do PROJECTION (0x0680) zložená matica okno × MVP po riadkoch (ako nouveau NV10/NV20),
  MODELVIEW jednotková, viewport posun 0 / mierka 1 bez zmeny. Orezanie robí GPU (DEPTH_CONTROL
  0x01), odvrátené steny GPU (CULL_FACE 0x1830, FRONT_FACE 0x1834, predná strana otočená kvôli
  y dole). S polygon offsetom ostáva cesta na CPU. Varianty: `nv34gltnlt` (transponovaná matica),
  `nv34gltnlclamp` (DEPTH_CONTROL 0x10), `nv34gltnlccw` (FRONT_FACE bez otočenia).
- q3-16 výsledok: obraz správny až s `nv34gltnlccw` — GPU berie FRONT_FACE podľa GL aj pri
  y dole v matici. Timedemo **54 fps** (z 50). Na GPU ide ~4000 troj./snímku (predtým CPU
  odstránil odvrátené ~polovicu), dávky do push buffra 1–2 ms. V menu GPU úzke miesto (glFinish 4 ms).
- q3-17: T&L predvolene zapnuté (`nv34glnotnl` = vypnuté), FRONT_FACE bez otočenia
  (`nv34gltnlcw` = otočené). V T&L dávke textúrové súradnice len s, t → 6 slov na vrchol
  (s dvoma textúrami 8 namiesto 12); `nv34gltnl4` = 4 floaty.
- q3-17 výsledok: Quake III timedemo **55,8 fps**, textúry s 2 floatmi v poriadku. Wolfenstein
  v náročnej scéne ~20 fps: snímka 51 ms, MiniGL 12 ms (~9400 troj.), zvyšok hra na CPU.
- wolf-6/7: Wolfenstein sa na počet jednotiek (0x84E2) vôbec nepýta — `/gfxinfo` ukazuje
  GL_MAX_ACTIVE_TEXTURES_ARB 0 len zo svojej premennej. Multitextúru používa: s nastavením
  „fast“ (po vid_restart) ~5000 z ~17 000 troj./snímku s 2 textúrami; s predvoleným „high“ (nastavenie
  sa neukladá) 0. Počet trojuholníkov je v oboch prípadoch rovnaký (~17 000), fps rovnaké (~15–16):
  snímka ~65 ms, MiniGL ~21 ms (glEnd ~15, dávky ~5,5), zvyšok hra. Ďalší zisk len z prípravy vrcholov.
- q3-18 výsledok: GPU zastane pri prvom kreslení z VRAM (hw_sync timeout), Mac zamrzne; varianty
  nepomohli. q3-19: VTX_CACHE_INV 0x1710 len na `nv34glvbinv` (možno metóda NV40), pri zaseknutí
  log PGRAPH trap (nsource, addr = metóda, data) a vypnutie kreslenia z VRAM; log prvého kreslenia.

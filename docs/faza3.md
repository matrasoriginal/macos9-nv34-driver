# Fáza 3 — rýchlosť 2D v systéme

Stav po 4.0d4: hry bežia dobre (kreslia do vlastného bufferu a kopírujú na obrazovku), ale Finder
je pomalý — redraw okien, ťahanie, scroll. QuickDraw je čisto softvérový.

## Prečo je Finder pomalý

Framebuffer je pre CPU mapovaný **cache-inhibited**. Zápisy cez AGP sú ešte znesiteľné, ale každé
**čítanie** VRAM je samostatná transakcia po zbernici. QuickDraw číta obrazovku pri:

- ťahaní a posúvaní okien, `ScrollRect` (CopyBits obrazovka → obrazovka),
- režimoch kreslenia, ktoré miešajú s pozadím (XOR výber, `srcOr`, šedé texty, maskovanie ikon),
- ukladaní pozadia pod menu.

Hry tento problém nemajú — obrazovku iba prepisujú.

## Krok 1: write-through cache (4.0d5)

`SetProcessorCacheMode(..., kProcessorCacheModeWriteThrough)` na oblasť framebuffera:
čítania idú z L1/L2, zápisy idú okamžite do VRAM (obraz je vždy aktuálny). Keďže do VRAM
nezapisuje GPU (žiadna akcelerácia), cache je stále konzistentná. Copy-back režim by obraz
oneskoroval — nepoužívame.

Riziko: inštrukcia `dcbz` na write-through stránke vyvolá na G4 výnimku zarovnania (OS ju emuluje,
pomaly, ale správne). Ak by bol niektorý test pomalší než bez cache, je to tento prípad.

Meranie: `NV34 Bench` (výplň, obrazovka→obrazovka, offscreen→obrazovka, obrazovka→offscreen,
text). Porovnať tri behy v rovnakej hĺbke:

1. bez drivera (Apple COFB),
2. `NV34 NDRV nocache`,
3. `NV34 NDRV` (write-through).

Výsledky sa pripisujú do `NV34 Bench.txt` aj s menom drivera a cache režimom.

### Výsledok (2026-10-07, 1024×768, 32 bpp, oblasť 504×384)

| | COFB | NV34 nocache | NV34 write-through |
|---|---|---|---|
| fill (zápis) | 736 MB/s | 733 MB/s | 735 MB/s |
| offscreen → obrazovka | 259 MB/s | 268 MB/s | 267 MB/s |
| obrazovka → offscreen (čítanie) | **13 MB/s** | 13 MB/s | 12 MB/s |
| obrazovka → obrazovka | **8.5 MB/s** | 8.5 MB/s | 8.5 MB/s |

Write-through **nemá účinok** — systém mapuje PCI/AGP priestor necacheovane (pravdepodobne
BAT/pevné WIMG) a `SetProcessorCacheMode` ho nezmení. Čítanie VRAM je ~50× pomalšie než zápis;
ťahanie okna (obrazovka → obrazovka) stojí presne na ňom. COFB je rovnako pomalý — nejde o chybu
drivera.

### Šírka prístupu (NV34 Bench 2)

| priame čítanie VRAM | 4 B `lwz` | 8 B `lfd` | 16 B `lvx` |
|---|---|---|---|
| | 12.0 MB/s | 12.0 MB/s | 12.0 MB/s |

Priepustnosť nezávisí od šírky prístupu — strop je na strane zbernice/karty, nie počtu inštrukcií.
(RAM testy vrátili 0: `NewPtr` 730 KB v malej partícii aplikácie zlyhal; na záver nemajú vplyv.)

Pôvodne zvažované cesty:

- **AltiVec cesta pre CopyBits obrazovka → obrazovka** (patch QuickDraw): zmysel má, ak `lvx`
  číta VRAM výrazne rýchlejšie než 13 MB/s.
- **Tieňový framebuffer**: QuickDraw kreslí do RAM, driver kopíruje zmenené riadky do VRAM
  z VBL časovača. Všetky čítania idú z RAM; cena = porovnanie + kópia každý snímok
  (podľa RAM/RAM → VRAM priepustnosti), oneskorenie ≤ 1 snímok.
- **Hardvérový blit** (krok 2 nižšie): najrýchlejší, ale najväčší projekt.

AltiVec cesta tým odpadá. Ostali tieňový framebuffer (CPU réžia, horšie pre hry kresliace priamo
na obrazovku) a hardvérový blit (veľký výskumný projekt).

## Rozhodnutie (2026-10-07)

**Nechať tak.** Režimy fungujú, hry bežia dobre a pomalé ťahanie okien je rovnaké ako s Apple COFB —
je to vlastnosť čítania VRAM cez AGP na tomto stroji, nie regresia drivera. Write-through je
v 4.0 vypnutý (`NV_FB_CACHE=0`), ostáva ako prepínač. Ak sa k 2D niekedy vráti, východisko je
tieňový framebuffer ako voliteľný variant drivera, potom hardvérový blit podľa kroku 2.

## Krok 2: hardvérová 2D akcelerácia (od 2026-10-14)

Softvérový OpenGL renderer síce beží, ale je nepoužiteľne pomalý → ideme do vlastnej 2D akcelerácie.

### Krok 2A — oživenie enginu (`NV34 Accel`)

Zdroj pravdy: **xf86-video-nv** (MIT), ktorý na PPC Macoch s GeForce FX (`10DE:0329`) robil
akcelerované výplne a kopírovanie. Port je v [accel/nv34_accel.c](../accel/nv34_accel.c):

- `NVLoadStateExt(pNv, NULL)` pre `NV_ARCH_30`: reset PFIFO/PGRAPH cez `PMC_ENABLE`, tile regióny,
  RAMHT (handle `0x80000010..17`) a objekty v PRAMIN (`0x710000`): DMA cez celú VRAM, surface2d
  `0x62`, ROP `0x43`, pattern `0x44`, clip `0x19`, line `0x5C`, image blit `0x5F`, GDI `0x4A`,
  scaled image `0x77`, DMA objekt push buffera. PGRAPH hodnoty pre NV30, PFIFO kanál 0 v DMA režime,
  `DMA_FETCH` s bitom 31 (big-endian push buffer).
- `NVResetGraphics`: push buffer 32 KB na `VRAM − 128 KB`, väzba objektov na subkanály, formát
  povrchu, pitch, **offset povrchu = `fboffset` (0x4000)** — jediný podstatný rozdiel oproti X.
- Výplň `RECT_SOLID_RECTS`, blit `BLIT_POINT_SRC/DST/SIZE`, sync = `GET == PUT` a `PGRAPH 0x700 == 0`.
- Vynechané: PTIMER (xf86 ho zapisuje na zlý offset, netreba), CRTC časť.
- Big-endian príznak objektov (`| 0x00080000`) podľa xf86; ak výplň vráti prehodené bajty,
  test automaticky skúsi objekty bez neho.

Aplikácia `NV34 Accel` (samostatná, systém nemení): init, GPU výplň + overenie pixelu, GPU blit +
porovnanie obsahu, meranie GPU výplne / GPU blitu / CPU (QuickDraw) blitu. Log `NV34 Accel.txt`
sa zapisuje po riadkoch — pri zaseknutí ostane posledný krok. Zaseknutie GPU = reštart.

#### Výsledok NV34 Accel 1 (2026-10-14, 1024×768, 32 bpp)

Init OK, výplň a blit overené spätným čítaním (big-endian objekty správne na prvý pokus):

| | rýchlosť |
|---|---|
| GPU výplň | 2.7 GB/s |
| GPU obrazovka → obrazovka | 1.34 GB/s |
| CPU (QuickDraw) obrazovka → obrazovka | 8 MB/s |

Vedľajší efekt: celý obsah obrazovky mimo okna sa rozsypal. Najpravdepodobnejšie preto, že init
podľa xf86 vynuloval PFB tile regióny a framebuffer z OF je v tile regióne (starý obsah sa začal čítať
lineárne). NV34 Accel 2 tile regióny nemení, iba ich zrkadlí do PGRAPH, a zapíše ich hodnoty.

#### Výsledok NV34 Accel 2

`PFB tile 0: 00004001 00300000 00001000 80000040` — framebuffer z OF je tile región (base 0x4000,
limit 0x300000, pitch 4096). S nezmenenými regiónmi (iba zrkadlenými do PGRAPH) výplň aj blit
prechádzajú, obrazovka ostáva celá: GPU výplň 2.3 GB/s, blit 1.17 GB/s. **Krok 2A hotový.**

### Krok 2B — napojenie na QuickDraw (po úspešnom 2A)

Návrh: PPC background aplikácia typu `appe` v Extensions (Retro68 vie PPC aplikácie; 68k toolchain
nemáme), ktorá pri štarte nainštaluje patch `CopyBits` / `ScrollRect` / `PaintRect`/`EraseRect`
cez `NSetTrapAddress` s routine descriptormi na vlastný PPC kód a ostane rezidentná. Patch
akceleruje iba bezpečný prípad, inak volá pôvodnú rutinu:

- zdroj aj cieľ = PixMap hlavnej obrazovky (`baseAddr` = framebuffer), `srcCopy`, bez masky,
  bez zmeny mierky a farebnej tabuľky, obdĺžnikový clip (`visRgn ∩ clipRgn` je obdĺžnik),
- kurzor: pred blitom `ShieldCursor` / po ňom `ShowCursor` ako QuickDraw,
- pred akýmkoľvek CPU prístupom k obrazovke `nva_sync` → patch synchronizuje pred návratom.

Engine po zmene rozlíšenia/hĺbky treba znovu nastaviť (`nva_reset_graphics`) — appe bude sledovať
`gdPMap` hlavného zariadenia.

#### Implementácia: `NV34 QD Accel`

**Pokus 1 — `appe` (qd-1, qd-2): nefunguje.** Patch sa nainštaluje (`trapNow == myUPP`) a vlastný
`CopyBits` cez InterfaceLib ním prejde (`selfTestCalls 1`), ale Finder ani iné aplikácie nie
(`calls 1`). Process Manager berie patch nainštalovaný aplikáciou ako lokálny pre jej kontext.

**Pokus 2 — INIT** ([nv34qd/](../nv34qd)): súbor typu `INIT` v Extensions:

- `init68k.c` — 68k stub (Start Manager spúšťa iba 68k INIT kód): načíta resource `'nvqd' 128`
  (sysHeap, locked) cez `GetMemFragment` v systémovej zóne, nájde `nv34qd_install` a zavolá ho
  cez routine descriptor (Mixed Mode 68k → PPC).
- `qdpatch.c` — PPC fragment bez runtime aplikácie: engine, patch `CopyBits` (`A8EC`), štatistiky.
  Patch inštalovaný počas INIT fázy je globálny.

Podmienky akcelerácie (bezo zmeny):

- zdroj aj cieľ sú framebuffer hlavného `GDevice` (CGrafPort portBits = PixMapHandle, PixMap, nie
  1-bit BitMap), `srcCopy`, rovnaká veľkosť, zdroj celý v hraniciach bitmapy,
- port má čierne popredie a biele pozadie, rovnaký `ctSeed` farebných tabuliek,
- klip = `dstRect ∩ bounds ∩ (visRgn ∩ clipRgn, ak cieľ = port) ∩ maskRgn`, rozložený na ≤ 512
  obdĺžnikov; poradie podľa smeru posunu (prekryv), jeden obdĺžnik = jeden HW blit,
- `ShieldCursor` / `ShowCursor` okolo, `nva_sync` pred návratom (CPU kreslí hneď potom).

Inak pôvodný CopyBits (`CallUniversalProc`). Pri timeoute reinit enginu, po druhom zlyhaní sa
akcelerácia vypne. Zmena hĺbky/rozlíšenia → `nva_reset_graphics`. Vypínače `boot-args`:
`nv34off`, `nv34noaccel`. Štatistiky: `NV34ACCEL,stats` → NV34 Info.

**Profil Findera (qd-4, qd-5):** 2 min práce → 25–31 s v CPU CopyBits, ktoré patch nespoznal.
Všetko išlo na jeden typ: **1-bit `BitMap` s `baseAddr` framebuffera** (`rowBytes 128`, `screenBits` /
port Window Managera), cieľ = zdroj, s maskou — presúvanie okien. QuickDraw ju nahradí PixMapom
zariadenia (425 ms za 1010×714 = 32 bpp kópia s čítaním VRAM). Ostatné (offscreen → obrazovka,
ikony cez `CopyMask`/`CopyDeepMask`, text) spolu < 0.5 s. Verzia 1.3 berie aj tento alias ako obrazovku.

**Výsledok 1.3 (qd-6, 2026-10-14):** okná sa kreslia správne, Finder citeľne rýchlejší.

| | volaní | spolu |
|---|---|---|
| GPU (akcelerované) | 289 | 363 ms |
| CPU offscreen → obrazovka | 1886 | 457 ms |
| CPU z obrazovky (čítanie VRAM) | 27 | 230 ms |
| CPU ostatné | 92 | 3 ms |
| CopyMask + CopyDeepMask | 1106 | 135 ms |

Čas v CopyBits za ~2 min práce klesol z 25–31 s na ~1.2 s. Zvyšok: kópie obrazovka → offscreen
(ukladanie pozadia, ~8.5 ms/volanie) — GPU by ich vedelo urobiť iba s offscreen bufferom vo VRAM.

**1.4:** v Thousands sa po presune okna prehadzovali susedné pixely. BE príznak objektov (z xf86)
prehadzuje 32-bit slová; pri 16/8 bpp je v slove viac pixelov → posun o nepárny počet ich zamieša.
Blit v LE režime je presná kópia bajtov v každej hĺbke → QD Accel používa `beObjects = 0`.

**1.5:** v Millions okná po presune zožltli — formát X8R8G8B8 nuluje „X“ bajt, ktorý je pri LE pohľade
na BE framebuffer modrá. QD Accel teraz používa raw formáty Y8/Y16/Y32 (presná kópia bajtov).
V Thousands sa rozbila celá obrazovka — zmena povrchu za behu volala `nva_reset_graphics`, ktorá
začala push buffer od nuly, kým GPU čítal od starého `GET` (staré príkazy = náhodné blity).
Po štarte sa povrch mení už iba príkazmi v existujúcom prúde (`nva_set_surface`).

**Stav 1.5 (2026-10-14):** presun okien a scroll cez GPU správne v 256, Thousands aj Millions,
vrátane prepínania hĺbky za behu. Uspanie sa na PB 6,2 v OS 9 nedá vyskúšať (sleep v OS 9 na
tomto modeli nefunguje). Krok 2B hotový.

Neakcelerované: `ScrollRect` (ak nevolá CopyBits cez trap), výplne (CPU zápis je rýchly),
kopírovanie obrazovka ↔ offscreen.

### Pôvodná úvaha

Vyžadovala by dve samostatné veci, obe veľké:

1. **NV34 PGRAPH / FIFO** — inicializácia grafického enginu, kanál (PIO alebo DMA push buffer),
   objekty `surf2d` / `imageblit` / `gdi_rect`, kontext, synchronizácia s CPU (notifier). Referencia:
   nouveau `nv04_fbcon` / `nv10_graph` / envytools. Bez binárneho blobu NV34 potrebuje aj ctxprog.
2. **Napojenie na QuickDraw** — OS 9 nemá verejné API pre 2D akcelerátor. NVIDIA/ATI to robili
   cez súkromné háčiky (INIT patchujúci `CopyBits`/`StdBits`/`FillRect` bottlenecky). Každý patch
   musí rozpoznať, či ide o hlavnú obrazovku, jednoduchý obdĺžnik bez masky a podporovaný transfer
   mód, inak volať pôvodnú rutinu; a pred každým CPU prístupom čakať na GPU.

Rozhodnutie podľa výsledkov kroku 1: ak write-through odstráni väčšinu spomalenia (ťahanie okien,
scroll), krok 2 sa neoplatí.

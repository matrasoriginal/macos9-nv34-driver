/*
 * NV34 2D engine (PFIFO DMA kanál 0 + PGRAPH NV04-štýl objekty).
 *
 * Port inicializácie z xf86-video-nv (nv_hw.c NVLoadStateExt pre NV_ARCH_30,
 * nv_xaa.c NVResetGraphics / DMA), licencia MIT, Copyright (c) 2003 NVIDIA Corp.
 * Bez závislostí na OS — volajúci dodá adresy MMIO/VRAM a hodinky pre timeouty.
 */
#ifndef NV34_ACCEL_H
#define NV34_ACCEL_H

typedef unsigned long   NVU32;
typedef unsigned char   NVU8;

enum {
    kNVAOK          = 0,
    kNVATimeout     = 1,    /* GPU neodpovedá — GET sa nehýbe alebo PGRAPH busy */
    kNVABadArgs     = 2
};

typedef struct NVAccel {
    volatile NVU8  *mmio;           /* BAR0 */
    volatile NVU8  *vram;           /* BAR1 (logická adresa) */
    NVU32           vramSize;       /* bajty VRAM (FbMapSize) */
    int             bigEndianRegs;  /* PMC_BOOT_1 = 0x01000001 */
    int             beObjects;      /* 2D objekty v big-endian režime (xf86 na PPC) */
    int             rawFormats;     /* povrch Y8/Y16/Y32: blit = presná kópia bajtov (iba kopírovanie) */
    NVU32         (*ticks)(void);   /* 60 Hz počítadlo pre timeouty */

    /* push buffer vo VRAM */
    NVU32           dmaOffset;      /* offset vo VRAM */
    volatile NVU32 *dmaBase;
    NVU32           dmaPut, dmaCurrent, dmaFree, dmaMax;

    /* aktuálny povrch */
    NVU32           depth;          /* 8, 15/16, 24/32 */
    NVU32           pitch;
    NVU32           surfOffset;
    NVU32           currentRop;

    /* diagnostika poslednej chyby */
    NVU32           errGet, errPut, errPgraphStatus, errPgraphIntr, errPfifoIntr;

    /* opt-2: veľkosť push buffra v bajtoch (0 = 32 KB na konci VRAM ako xf86); ak ≠ 0, volajúci
     * nastaví aj dmaOffset. Na konci štruktúry, aby staršie buildy klientov (RAVE) sedeli. */
    NVU32           dmaSize;
} NVAccel;

/* Reset a inicializácia PFIFO + PGRAPH + objektov v PRAMIN. Obrazu sa nedotýka. */
void    nva_init_engine(NVAccel *a);

/* Push buffer, väzba objektov na subkanály, formát a pitch povrchu (offset = začiatok
 * framebuffera vo VRAM). Mac 16 bpp = 555 → depth 15. */
int     nva_reset_graphics(NVAccel *a, NVU32 depth, NVU32 pitch, NVU32 surfOffset);

/* Zmena povrchu za behu (hĺbka/pitch/offset) — príkazy do existujúceho prúdu,
 * bez resetu push buffera (GPU pokračuje od aktuálneho GET). */
int     nva_set_surface(NVAccel *a, NVU32 depth, NVU32 pitch, NVU32 surfOffset);

/* Po 3D inite (nové načítanie PGRAPH kontextu) znova naviaže 2D objekty a nastaví povrch,
 * klip a ROP podľa posledného nva_set_surface. */
int     nva_restore_2d(NVAccel *a);

void    nva_fill(NVAccel *a, int x, int y, int w, int h, NVU32 color);
void    nva_copy(NVAccel *a, int sx, int sy, int dx, int dy, int w, int h);
/* Blit z iného povrchu (offset/pitch vo VRAM, rovnaká hĺbka) na aktuálny povrch. */
void    nva_copy_from(NVAccel *a, NVU32 srcOffset, NVU32 srcPitch, int sx, int sy, int dx, int dy, int w, int h);
void    nva_kickoff(NVAccel *a);

/* Počká, kým GPU spracuje push buffer a PGRAPH je nečinný. Pred CPU prístupom do VRAM. */
int     nva_sync(NVAccel *a, NVU32 timeoutTicks);

/* Vypne PFIFO caches (núdzové zastavenie po chybe). */
void    nva_shutdown(NVAccel *a);

NVU32   nva_rd32(NVAccel *a, NVU32 reg);
void    nva_wr32(NVAccel *a, NVU32 reg, NVU32 val);

/* Surový prístup k push bufferu: metóda `mthd` na subkanáli `subc`, `count` dát nasleduje. */
int     nva_begin(NVAccel *a, int subc, NVU32 mthd, NVU32 count);
void    nva_push(NVAccel *a, NVU32 data);

#endif

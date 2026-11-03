/*
 * NV34 2D engine (PFIFO DMA channel 0 + PGRAPH NV04-style objects).
 *
 * Initialization ported from xf86-video-nv (nv_hw.c NVLoadStateExt for NV_ARCH_30,
 * nv_xaa.c NVResetGraphics / DMA), MIT license, Copyright (c) 2003 NVIDIA Corp.
 * No OS dependencies — the caller supplies the MMIO/VRAM addresses and a clock for timeouts.
 */
#ifndef NV34_ACCEL_H
#define NV34_ACCEL_H

typedef unsigned long   NVU32;
typedef unsigned char   NVU8;

enum {
    kNVAOK          = 0,
    kNVATimeout     = 1,    /* GPU not responding — GET does not move or PGRAPH busy */
    kNVABadArgs     = 2
};

typedef struct NVAccel {
    volatile NVU8  *mmio;           /* BAR0 */
    volatile NVU8  *vram;           /* BAR1 (logical address) */
    NVU32           vramSize;       /* VRAM bytes (FbMapSize) */
    int             bigEndianRegs;  /* PMC_BOOT_1 = 0x01000001 */
    int             beObjects;      /* 2D objects in big-endian mode (xf86 on PPC) */
    int             rawFormats;     /* Y8/Y16/Y32 surface: blit = exact byte copy (copy only) */
    NVU32         (*ticks)(void);   /* 60 Hz counter for timeouts */

    /* push buffer in VRAM */
    NVU32           dmaOffset;      /* offset in VRAM */
    volatile NVU32 *dmaBase;
    NVU32           dmaPut, dmaCurrent, dmaFree, dmaMax;

    /* current surface */
    NVU32           depth;          /* 8, 15/16, 24/32 */
    NVU32           pitch;
    NVU32           surfOffset;
    NVU32           currentRop;

    /* diagnostics of the last error */
    NVU32           errGet, errPut, errPgraphStatus, errPgraphIntr, errPfifoIntr;

    /* opt-2: push buffer size in bytes (0 = 32 KB at the end of VRAM like xf86); if ≠ 0 the caller
     * also sets dmaOffset. At the end of the struct so older client builds (RAVE) still match. */
    NVU32           dmaSize;
} NVAccel;

/* Reset and initialization of PFIFO + PGRAPH + objects in PRAMIN. Does not touch the display. */
void    nva_init_engine(NVAccel *a);

/* Push buffer, binding objects to subchannels, surface format and pitch (offset = start of
 * the framebuffer in VRAM). Mac 16 bpp = 555 → depth 15. */
int     nva_reset_graphics(NVAccel *a, NVU32 depth, NVU32 pitch, NVU32 surfOffset);

/* Change the surface on the fly (depth/pitch/offset) — commands into the existing stream,
 * no push buffer reset (the GPU continues from the current GET). */
int     nva_set_surface(NVAccel *a, NVU32 depth, NVU32 pitch, NVU32 surfOffset);

/* After a 3D init (fresh PGRAPH context load) rebinds the 2D objects and sets the surface,
 * clip and ROP from the last nva_set_surface. */
int     nva_restore_2d(NVAccel *a);

void    nva_fill(NVAccel *a, int x, int y, int w, int h, NVU32 color);
void    nva_copy(NVAccel *a, int sx, int sy, int dx, int dy, int w, int h);
/* Blit from another surface (offset/pitch in VRAM, same depth) to the current surface. */
void    nva_copy_from(NVAccel *a, NVU32 srcOffset, NVU32 srcPitch, int sx, int sy, int dx, int dy, int w, int h);
void    nva_kickoff(NVAccel *a);

/* Waits until the GPU has processed the push buffer and PGRAPH is idle. Before CPU access to VRAM. */
int     nva_sync(NVAccel *a, NVU32 timeoutTicks);

/* Disables the PFIFO caches (emergency stop after an error). */
void    nva_shutdown(NVAccel *a);

NVU32   nva_rd32(NVAccel *a, NVU32 reg);
void    nva_wr32(NVAccel *a, NVU32 reg, NVU32 val);

/* Raw push buffer access: method `mthd` on subchannel `subc`, `count` data words follow. */
int     nva_begin(NVAccel *a, int subc, NVU32 mthd, NVU32 count);
void    nva_push(NVAccel *a, NVU32 data);

#endif

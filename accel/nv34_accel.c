/*
 * NV34 2D engine — port z xf86-video-nv (MIT):
 *
 * Copyright (c) 2003 NVIDIA, Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Zmeny: iba cesta NV_ARCH_30 (NV34), bez CRTC časti, bez PTIMER, PFB tile regióny
 * sa nemenia (OF ich používa pre framebuffer), timeouty,
 * big-endian objekty voliteľné za behu, nezávislé od X/OS.
 */
#include "nv34_accel.h"

/* ---- MMIO ---------------------------------------------------------------- */

static inline void io_barrier(void)
{
    __asm__ volatile("eieio" ::: "memory");
}

NVU32 nva_rd32(NVAccel *a, NVU32 reg)
{
    volatile NVU8 *p = a->mmio + reg;
    NVU32 v;
    if (a->bigEndianRegs)
        v = *(volatile NVU32 *)p;
    else
        __asm__ volatile("lwbrx %0,0,%1" : "=r"(v) : "r"(p) : "memory");
    io_barrier();
    return v;
}

static void wr32(NVAccel *a, NVU32 reg, NVU32 val)
{
    volatile NVU8 *p = a->mmio + reg;
    if (a->bigEndianRegs)
        *(volatile NVU32 *)p = val;
    else
        __asm__ volatile("stwbrx %0,0,%1" : : "r"(val), "r"(p) : "memory");
    io_barrier();
}

void nva_wr32(NVAccel *a, NVU32 reg, NVU32 val)
{
    wr32(a, reg, val);
}

#define RD(r)       nva_rd32(a, (r))
#define WR(r, v)    wr32(a, (r), (v))

/* bloky registrov (xf86 nv_setup.c) */
#define PMC         0x000000
#define PFIFO       0x002000
#define PFB         0x100000
#define PGRAPH      0x400000
#define PRAMIN      0x710000
#define USER_FIFO   0x800000

/* xf86 indexuje PFIFO a PRAMIN po dwordoch, PGRAPH/PFB/PMC po bajtoch/4 → tu všetko v bajtoch */
#define PFIFO_D(i)  (PFIFO + (i) * 4)
#define PRAMIN_D(i) (PRAMIN + (i) * 4)

/* ---- inicializácia (NVLoadStateExt, state == NULL, NV_ARCH_30) --------- */

void nva_init_engine(NVAccel *a)
{
    NVU32 fbMapSize = a->vramSize;
    NVU32 fbUsable = fbMapSize - 128 * 1024;
    int i;

    if (!a->dmaSize || a->dmaSize > 0x100000) {
        a->dmaOffset = fbUsable;
        a->dmaSize = 32 * 1024;
    }

    WR(PMC + 0x0140, 0x00000000);           /* prerušenia z GPU vypnuté */
    WR(PMC + 0x0200, 0xFFFF00FF);           /* reset PFIFO/PGRAPH (bity 8–15) */
    WR(PMC + 0x0200, 0xFFFFFFFF);

    /* xf86 tu zapisuje aj PTIMER — vynechané (na Macu ho nik nepotrebuje). */

    /* xf86 tu tile regióny PFB 0x240.. vynuluje. Na PB 6,2 je framebuffer z OF v tile regióne:
     * vypnutie zmení rozloženie VRAM pre scanout aj CPU → starý obsah obrazovky sa rozsype
     * (NV34 Accel 1). Regióny nechávame a nižšie ich iba zrkadlíme do PGRAPH. */

    /* RAMHT (4 KB): vynulovať — xf86 zapisuje iba svojich 8 položiek a zvyšok PRAMIN obsahuje
     * starý obsah VRAM; ďalší handle by sa vložil mimo okna, ktoré PFIFO prehľadáva (gl-1). */
    for (i = 0; i < 0x1000; i += 4)
        WR(PRAMIN + i, 0);
    /* handle 0x80000010..17 → objekty (hash(h) = index, kanál 0, engine PGRAPH) */
    for (i = 0; i < 8; i++) {
        WR(PRAMIN_D(i * 2), 0x80000010 + i);
        WR(PRAMIN_D(i * 2 + 1), 0x80011201 + i);
    }

    /* 0x1200: DMA objekt cez celú VRAM */
    WR(PRAMIN_D(0x0800), 0x00003000);
    WR(PRAMIN_D(0x0801), fbMapSize - 1);
    WR(PRAMIN_D(0x0802), 0x00000002);
    WR(PRAMIN_D(0x0803), 0x00000002);
    /* 0x1201: surface 2d (0x62) */
    WR(PRAMIN_D(0x0804), 0x01008062);
    WR(PRAMIN_D(0x0805), 0x00000000);
    WR(PRAMIN_D(0x0806), 0x12001200);
    WR(PRAMIN_D(0x0807), 0x00000000);
    /* 0x1202: ROP (0x43) */
    WR(PRAMIN_D(0x0808), 0x01008043);
    WR(PRAMIN_D(0x0809), 0x00000000);
    WR(PRAMIN_D(0x080A), 0x00000000);
    WR(PRAMIN_D(0x080B), 0x00000000);
    /* 0x1203: pattern (0x44) */
    WR(PRAMIN_D(0x080C), 0x01008044);
    WR(PRAMIN_D(0x080D), 0x00000002);
    WR(PRAMIN_D(0x080E), 0x00000000);
    WR(PRAMIN_D(0x080F), 0x00000000);
    /* 0x1204: clip (0x19) */
    WR(PRAMIN_D(0x0810), 0x01008019);
    WR(PRAMIN_D(0x0811), 0x00000000);
    WR(PRAMIN_D(0x0812), 0x00000000);
    WR(PRAMIN_D(0x0813), 0x00000000);
    /* 0x1205: line (0x5C) */
    WR(PRAMIN_D(0x0814), 0x0100A05C);
    WR(PRAMIN_D(0x0815), 0x00000000);
    WR(PRAMIN_D(0x0816), 0x00000000);
    WR(PRAMIN_D(0x0817), 0x00000000);
    /* 0x1206: image blit (0x5F) */
    WR(PRAMIN_D(0x0818), 0x0100805F);
    WR(PRAMIN_D(0x0819), 0x00000000);
    WR(PRAMIN_D(0x081A), 0x12001200);
    WR(PRAMIN_D(0x081B), 0x00000000);
    /* 0x1207: GDI rect (0x4A) */
    WR(PRAMIN_D(0x081C), 0x0100804A);
    WR(PRAMIN_D(0x081D), 0x00000002);
    WR(PRAMIN_D(0x081E), 0x00000000);
    WR(PRAMIN_D(0x081F), 0x00000000);
    /* 0x1208: scaled image (0x77) */
    WR(PRAMIN_D(0x0820), 0x01018077);
    WR(PRAMIN_D(0x0821), 0x00000000);
    WR(PRAMIN_D(0x0822), 0x12001200);
    WR(PRAMIN_D(0x0823), 0x00000000);
    /* 0x1209: DMA objekt push buffera (dmaSize na dmaOffset) */
    WR(PRAMIN_D(0x0824), 0x00003002);
    WR(PRAMIN_D(0x0825), a->dmaSize - 1);
    WR(PRAMIN_D(0x0826), a->dmaOffset | 0x00000002);
    WR(PRAMIN_D(0x0827), 0x00000002);

    if (a->beObjects) {
        static const NVU32 objs[] = { 0x0804, 0x0808, 0x080C, 0x0810, 0x0814, 0x0818, 0x081C, 0x0820 };
        for (i = 0; i < 8; i++)
            WR(PRAMIN_D(objs[i]), RD(PRAMIN_D(objs[i])) | 0x00080000);
        WR(PRAMIN_D(0x080D), 0x00000001);
        WR(PRAMIN_D(0x081D), 0x00000001);
    }

    /* PGRAPH (NV_ARCH >= 10, NV_ARCH_30) */
    WR(PGRAPH + 0x0080, 0xFFFFFFFF);
    WR(PGRAPH + 0x0080, 0x00000000);
    WR(PGRAPH + 0x0140, 0x00000000);
    WR(PGRAPH + 0x0100, 0xFFFFFFFF);
    WR(PGRAPH + 0x0144, 0x10010100);
    WR(PGRAPH + 0x0714, 0xFFFFFFFF);
    WR(PGRAPH + 0x0720, 0x00000001);
    WR(PGRAPH + 0x0710, (RD(PGRAPH + 0x0710) & 0x0007ff00) | 0x00020100);

    WR(PGRAPH + 0x0084, 0x40108700);
    WR(PGRAPH + 0x0890, 0x00140000);
    WR(PGRAPH + 0x008C, 0xf00e0431);
    WR(PGRAPH + 0x0090, 0x00008000);
    WR(PGRAPH + 0x0610, 0xf04b1f36);
    WR(PGRAPH + 0x0B80, 0x1002d888);
    WR(PGRAPH + 0x0B88, 0x62ff007f);

    for (i = 0; i < 32; i++) {
        NVU32 v = RD(PFB + 0x0240 + i * 4);
        WR(PGRAPH + 0x0900 + i * 4, v);
        WR(PGRAPH + 0x6900 + i * 4, v);
    }

    WR(PGRAPH + 0x09A4, RD(PFB + 0x0200));
    WR(PGRAPH + 0x09A8, RD(PFB + 0x0204));
    WR(PGRAPH + 0x0750, 0x00EA0000);
    WR(PGRAPH + 0x0754, RD(PFB + 0x0200));
    WR(PGRAPH + 0x0750, 0x00EA0004);
    WR(PGRAPH + 0x0754, RD(PFB + 0x0204));

    WR(PGRAPH + 0x0820, 0);
    WR(PGRAPH + 0x0824, 0);
    WR(PGRAPH + 0x0864, fbMapSize - 1);
    WR(PGRAPH + 0x0868, fbMapSize - 1);

    WR(PGRAPH + 0x0B20, 0x00000000);
    WR(PGRAPH + 0x0B04, 0xFFFFFFFF);

    WR(PGRAPH + 0x053C, 0);
    WR(PGRAPH + 0x0540, 0);
    WR(PGRAPH + 0x0544, 0x00007FFF);
    WR(PGRAPH + 0x0548, 0x00007FFF);

    /* PFIFO: kanál 0 v DMA režime */
    WR(PFIFO_D(0x0140), 0x00000000);        /* CACHES vypnuté počas nastavovania */
    WR(PFIFO_D(0x0141), 0x00000001);
    WR(PFIFO_D(0x0480), 0x00000000);
    WR(PFIFO_D(0x0494), 0x00000000);
    WR(PFIFO_D(0x0481), 0x00000100);        /* CACHE1_PUSH1: kanál 0, DMA */
    WR(PFIFO_D(0x0490), 0x00000000);        /* DMA_PUT */
    WR(PFIFO_D(0x0491), 0x00000000);        /* DMA_GET */
    WR(PFIFO_D(0x048B), 0x00001209);        /* DMA_INSTANCE → objekt push buffera */
    WR(PFIFO_D(0x0400), 0x00000000);
    WR(PFIFO_D(0x0414), 0x00000000);
    WR(PFIFO_D(0x0084), 0x03000100);        /* RAMHT na 0x10000 */
    WR(PFIFO_D(0x0085), 0x00000110);        /* RAMFC */
    WR(PFIFO_D(0x0086), 0x00000112);        /* RAMRO */
    WR(PFIFO_D(0x0143), 0x0000FFFF);
    WR(PFIFO_D(0x0496), 0x0000FFFF);
    WR(PFIFO_D(0x0050), 0x00000000);
    WR(PFIFO_D(0x0040), 0xFFFFFFFF);
    WR(PFIFO_D(0x0415), 0x00000001);
    WR(PFIFO_D(0x048C), 0x00000000);
    WR(PFIFO_D(0x04A0), 0x00000000);
    WR(PFIFO_D(0x0489), 0x800F0078);        /* DMA_FETCH, bit 31 = push buffer big endian */
    WR(PFIFO_D(0x0488), 0x00000001);        /* DMA_PUSH */
    WR(PFIFO_D(0x0480), 0x00000001);        /* CACHE1_PUSH0 */
    WR(PFIFO_D(0x0494), 0x00000001);        /* CACHE1_PULL0 */
    WR(PFIFO_D(0x0495), 0x00000001);
    WR(PFIFO_D(0x0140), 0x00000001);        /* CACHES zapnuté */
}

/* ---- push buffer (nv_xaa.c) ---------------------------------------------- */

#define SKIPS 8

#define READ_GET()  (RD(USER_FIFO + 0x44) >> 2)

static void write_put(NVAccel *a, NVU32 put)
{
    volatile NVU8 scratch;
    __asm__ volatile("sync" ::: "memory");  /* zápisy push buffera do VRAM pred PUT */
    scratch = a->vram[0];                   /* vyprázdni posted writes (xf86: FbStart[0]) */
    (void)scratch;
    WR(USER_FIFO + 0x40, put << 2);
}

#define DMA_NEXT(d)  (a->dmaBase[a->dmaCurrent++] = (d))

static int dma_wait(NVAccel *a, NVU32 size);

static int dma_start(NVAccel *a, NVU32 tag, NVU32 size)
{
    if (a->dmaFree <= size && dma_wait(a, size) != kNVAOK)
        return kNVATimeout;
    DMA_NEXT((size << 18) | tag);
    a->dmaFree -= size + 1;
    return kNVAOK;
}

static int dma_wait(NVAccel *a, NVU32 size)
{
    NVU32 get, t0 = a->ticks();

    size++;
    while (a->dmaFree < size) {
        if (a->ticks() - t0 > 60)
            return kNVATimeout;
        get = READ_GET();
        if (a->dmaPut >= get) {
            a->dmaFree = a->dmaMax - a->dmaCurrent;
            if (a->dmaFree < size) {
                DMA_NEXT(0x20000000);       /* jump na začiatok */
                if (get <= SKIPS) {
                    if (a->dmaPut <= SKIPS)
                        write_put(a, SKIPS + 1);
                    do {
                        get = READ_GET();
                        if (a->ticks() - t0 > 60)
                            return kNVATimeout;
                    } while (get <= SKIPS);
                }
                write_put(a, SKIPS);
                a->dmaCurrent = a->dmaPut = SKIPS;
                a->dmaFree = get - (SKIPS + 1);
            }
        } else {
            a->dmaFree = get - a->dmaCurrent - 1;
        }
    }
    return kNVAOK;
}

int nva_begin(NVAccel *a, int subc, NVU32 mthd, NVU32 count)
{
    return dma_start(a, ((NVU32)subc << 13) | mthd, count);
}

void nva_push(NVAccel *a, NVU32 data)
{
    DMA_NEXT(data);
}

void nva_kickoff(NVAccel *a)
{
    if (a->dmaCurrent != a->dmaPut) {
        a->dmaPut = a->dmaCurrent;
        write_put(a, a->dmaPut);
    }
}

/* metódy (nv_dma.h): subkanál << 13 | metóda */
#define SURFACE_FORMAT      0x00000300
#define ROP_SET             0x00002300
#define PATTERN_FORMAT      0x00004300
#define PATTERN_COLOR_0     0x00004310
#define BLIT_POINT_SRC      0x0000A300
#define RECT_FORMAT         0x0000C300
#define RECT_SOLID_COLOR    0x0000C3FC
#define RECT_SOLID_RECTS0   0x0000C400
#define LINE_FORMAT         0x00008300
#define CLIP_POINT          0x00006300

/* Formáty: surface / pattern / rect. Raw (Y8/Y16/Y32) nekonvertuje kanály — pri X8R8G8B8 engine
 * nuluje "X" bajt, čo je pri LE pohľade na BE framebuffer modrá (qd-7: okná zožltli). */
static void pick_formats(NVAccel *a, NVU32 depth, NVU32 *surf, NVU32 *pat, NVU32 *rect)
{
    switch (depth) {
    case 24:
    case 32:
        *surf = a->rawFormats ? 0xB : 6;    /* Y32 / X8R8G8B8 */
        *pat = 3;
        *rect = 3;
        break;
    case 15:
    case 16:
        *surf = a->rawFormats ? 5 : 4;      /* Y16 / R5G6B5 (xf86 aj pre 15) */
        *pat = 1;
        *rect = 1;
        break;
    default:
        *surf = 1;                          /* Y8 */
        *pat = 3;
        *rect = 3;
        break;
    }
}

static int emit_surface(NVAccel *a, NVU32 depth, NVU32 pitch, NVU32 surfOffset)
{
    NVU32 surf, pat, rect;

    if (pitch == 0 || pitch > 0x7fff || (surfOffset & 63))
        return kNVABadArgs;
    pick_formats(a, depth, &surf, &pat, &rect);
    a->depth = depth;
    a->pitch = pitch;
    a->surfOffset = surfOffset;

    if (dma_start(a, SURFACE_FORMAT, 4))
        return kNVATimeout;
    DMA_NEXT(surf);
    DMA_NEXT(pitch | (pitch << 16));
    DMA_NEXT(surfOffset);               /* xf86: 0 (FB na začiatku VRAM); Mac: fboffset */
    DMA_NEXT(surfOffset);
    dma_start(a, PATTERN_FORMAT, 1);
    DMA_NEXT(pat);
    dma_start(a, RECT_FORMAT, 1);
    DMA_NEXT(rect);
    dma_start(a, LINE_FORMAT, 1);
    DMA_NEXT(rect);
    return kNVAOK;
}

static int emit_2d_state(NVAccel *a, NVU32 depth, NVU32 pitch, NVU32 surfOffset);

int nva_reset_graphics(NVAccel *a, NVU32 depth, NVU32 pitch, NVU32 surfOffset)
{
    int i;

    if (pitch == 0 || pitch > 0x7fff || (surfOffset & 63))
        return kNVABadArgs;

    /* iba hneď po nva_init_engine: DMA_GET/PUT sú 0, buffer začína odznova */
    a->dmaBase = (volatile NVU32 *)(a->vram + a->dmaOffset);
    for (i = 0; i < SKIPS; i++)
        a->dmaBase[i] = 0;
    /* subkanál i ← objekt 0x80000010 + i */
    for (i = 0; i < 8; i++) {
        a->dmaBase[SKIPS + i * 2] = 0x00040000 | (i << 13);
        a->dmaBase[SKIPS + i * 2 + 1] = 0x80000010 + i;
    }
    a->dmaPut = 0;
    a->dmaCurrent = 16 + SKIPS;
    a->dmaMax = (a->dmaSize ? a->dmaSize : 32768) / 4 - 1;
    a->dmaFree = a->dmaMax - a->dmaCurrent;

    return emit_2d_state(a, depth, pitch, surfOffset);
}

/* povrch + klip + ROP (časť resetu, ktorá nemení push buffer) */
static int emit_2d_state(NVAccel *a, NVU32 depth, NVU32 pitch, NVU32 surfOffset)
{
    int err = emit_surface(a, depth, pitch, surfOffset);
    if (err)
        return err;

    /* NVDisableClipping */
    dma_start(a, CLIP_POINT, 2);
    DMA_NEXT(0);
    DMA_NEXT(0x7FFF7FFF);

    /* NVSetRopSolid(GXcopy, ~0): vzor = samé jednotky, ROP 0xCC (copy) */
    dma_start(a, PATTERN_COLOR_0, 4);
    DMA_NEXT(~0UL);
    DMA_NEXT(~0UL);
    DMA_NEXT(~0UL);
    DMA_NEXT(~0UL);
    dma_start(a, ROP_SET, 1);
    DMA_NEXT(0xCC);
    a->currentRop = 0xCC;

    nva_kickoff(a);
    return kNVAOK;
}

int nva_restore_2d(NVAccel *a)
{
    int i;
    /* subkanály 0–6 ← 2D objekty (po načítaní PGRAPH kontextu z 3D initu) */
    for (i = 0; i < 7; i++) {
        if (nva_begin(a, i, 0, 1) != kNVAOK)
            return kNVATimeout;
        nva_push(a, 0x80000010 + i);
    }
    return emit_2d_state(a, a->depth, a->pitch, a->surfOffset);
}

int nva_set_surface(NVAccel *a, NVU32 depth, NVU32 pitch, NVU32 surfOffset)
{
    int err = emit_surface(a, depth, pitch, surfOffset);
    if (!err)
        nva_kickoff(a);
    return err;
}

void nva_fill(NVAccel *a, int x, int y, int w, int h, NVU32 color)
{
    dma_start(a, RECT_SOLID_COLOR, 1);
    DMA_NEXT(color);
    dma_start(a, RECT_SOLID_RECTS0, 2);
    DMA_NEXT(((NVU32)x << 16) | (NVU32)y);
    DMA_NEXT(((NVU32)w << 16) | (NVU32)h);
}

void nva_copy(NVAccel *a, int sx, int sy, int dx, int dy, int w, int h)
{
    dma_start(a, BLIT_POINT_SRC, 3);
    DMA_NEXT(((NVU32)sy << 16) | (NVU32)sx);
    DMA_NEXT(((NVU32)dy << 16) | (NVU32)dx);
    DMA_NEXT(((NVU32)h << 16) | (NVU32)w);
}

void nva_copy_from(NVAccel *a, NVU32 srcOffset, NVU32 srcPitch, int sx, int sy, int dx, int dy, int w, int h)
{
    NVU32 surf, pat, rect;

    pick_formats(a, a->depth, &surf, &pat, &rect);
    dma_start(a, SURFACE_FORMAT, 4);
    DMA_NEXT(surf);
    DMA_NEXT(srcPitch | (a->pitch << 16));
    DMA_NEXT(srcOffset);
    DMA_NEXT(a->surfOffset);
    nva_copy(a, sx, sy, dx, dy, w, h);
    dma_start(a, SURFACE_FORMAT, 4);            /* späť: zdroj = obrazovka */
    DMA_NEXT(surf);
    DMA_NEXT(a->pitch | (a->pitch << 16));
    DMA_NEXT(a->surfOffset);
    DMA_NEXT(a->surfOffset);
}

int nva_sync(NVAccel *a, NVU32 timeoutTicks)
{
    NVU32 t0 = a->ticks();

    nva_kickoff(a);
    while (READ_GET() != a->dmaPut) {
        if (a->ticks() - t0 > timeoutTicks)
            goto fail;
    }
    while (RD(PGRAPH + 0x0700)) {
        if (a->ticks() - t0 > timeoutTicks)
            goto fail;
    }
    return kNVAOK;

fail:
    a->errGet = READ_GET();
    a->errPut = a->dmaPut;
    a->errPgraphStatus = RD(PGRAPH + 0x0700);
    a->errPgraphIntr = RD(PGRAPH + 0x0100);
    a->errPfifoIntr = RD(PFIFO + 0x0100);
    return kNVATimeout;
}

void nva_shutdown(NVAccel *a)
{
    WR(PFIFO_D(0x0140), 0x00000000);
    WR(PFIFO_D(0x0480), 0x00000000);
    WR(PFIFO_D(0x0494), 0x00000000);
}

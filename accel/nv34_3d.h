/*
 * NV34 3D engine (rankine, trieda 0x0697) — bring-up pre OpenGL.
 *
 * PGRAPH kontext a init registre podľa Linux nouveau (nvkm/engine/gr/nv30.c, nv34.c, MIT),
 * počiatočný stav 3D podľa xf86-video-nouveau (nv30_exa.c NVAccelInitNV30TCL, MIT),
 * metódy z nv30-40_3d.xml.h (rules-ng-ng, MIT). Kreslí cez fixed-function transformáciu
 * (matice MODELVIEW/PROJECTION) a krátky fragment program.
 */
#ifndef NV34_3D_H
#define NV34_3D_H

#include "nv34_accel.h"

#define NV3D_SUBC       7               /* subkanál pre 3D objekt (pôvodne scaled image 0x77) */

/* handle v RAMHT */
#define NV3D_HANDLE_3D  0x80000020
#define NV3D_HANDLE_VRAM 0x80000030

/* inštancie v PRAMIN (bajtový offset od začiatku PRAMIN, BAR0 0x700000) */
#define NV3D_INST_3D    0x12100         /* objekt 0x0697 (16 B) */
#define NV3D_INST_DMA   0x12140         /* DMA objekt 0x3d cez celú VRAM (16 B) */
#define NV3D_INST_CTXTAB 0x14000        /* tabuľka kontextov 32 × 4 B */
#define NV3D_INST_CTX   0x20000         /* PGRAPH kontext kanála 0 (0x46dc B) */

typedef struct NV3D {
    NVAccel    *a;
    NVU32       fbOffset, pitch, width, height;
    NVU32       scratchOffset;          /* VRAM: zeta + fragment programy */
    NVU32       fpOffset;
} NV3D;

/* PGRAPH init pre 3D (nv30_gr_init + NV34), kontext, objekty, väzby subkanálov.
 * Volať po nva_init_engine + nva_reset_graphics. Vráti kNVAOK alebo kNVATimeout. */
int     nv3d_init(NV3D *g, NVAccel *a, NVU32 fbOffset, NVU32 pitch, NVU32 width, NVU32 height);

/* Fragment program do VRAM. `swapMode`: 0 = GPU číta polovice prehodené (ako PUSH_DATAs),
 * 1 = bez prehodenia bajtov (test variantov). */
void    nv3d_upload_fp(NV3D *g, const NVU32 *words, int count, int swapMode);
void    nv3d_use_fp(NV3D *g, NVU32 fpControl, NVU32 regControl);

/* Výstup cez register combiners: final = primárna farba (obíde fragment program). */
void    nv3d_rc_primary(NV3D *g);

/* 3D objekt s/bez big-endian príznaku (0x00080000) a nové naviazanie na subkanál. */
void    nv3d_object_be(NV3D *g, int be);

/* Combiners: final = priamo vstup `input` (0x04 primárna farba, 0x08 FP register H0…). */
void    nv3d_rc_final(NV3D *g, NVU32 input);

/* Combiners: final = textúra0 × primárna farba (RGB aj alfa). */
void    nv3d_rc_modulate_tex(NV3D *g);

/* Textúra RECT A8R8G8B8 (lineárna, nenormalizované súradnice) v jednotke 0. */
void    nv3d_texture_rect(NV3D *g, NVU32 vramOffset, NVU32 w, NVU32 h, NVU32 pitch);

/* Obdĺžnik z 2 trojuholníkov s textúrovými súradnicami 0..tw, 0..th (texely). */
void    nv3d_quad_tex(NV3D *g, int x, int y, int w, int h, int tw, int th, NVU32 r, NVU32 gr, NVU32 b, NVU32 al);

/* Z-buffer: mierka z vo viewporte (float bity), test LESS + zápis, vymazanie hĺbky na 1.0. */
void    nv3d_depth(NV3D *g, int enable, NVU32 zScale);
void    nv3d_clear_depth(NV3D *g);
void    nv3d_clear_depth_value(NV3D *g, NVU32 value, int writeEnable);

/* Blending: 0 = vypnutý, 1 = SRC_ALPHA / ONE_MINUS_SRC_ALPHA. */
void    nv3d_blend(NV3D *g, int enable);

/* Obdĺžnik s hĺbkou z (float bity) a farbou (combiners = primárna farba). */
void    nv3d_rect_z(NV3D *g, int x, int y, int w, int h, NVU32 z, NVU32 r, NVU32 gr, NVU32 b, NVU32 al);

/* Swizzled POT textúra A8R8G8B8 v jednotke 0, REPEAT, lineárny filter. */
void    nv3d_texture_swz(NV3D *g, NVU32 vramOffset, NVU32 log2w, NVU32 log2h);

/* Obdĺžnik s normalizovanými textúrovými súradnicami (float bity). */
void    nv3d_quad_texf(NV3D *g, int x, int y, int w, int h, NVU32 s1, NVU32 t1);

/* Trojuholník v pixeloch obrazovky, farba RGBA 0..1 (float bity). */
void    nv3d_triangle(NV3D *g, int x0, int y0, int x1, int y1, int x2, int y2,
                      NVU32 r, NVU32 gr, NVU32 b, NVU32 al);

/* Diagnostika PGRAPH po chybe + obnovenie FIFO. */
typedef struct NV3DTrap {
    NVU32 intr, nsource, nstatus, addr, data, ctxUser, status;
    NVU32 fifoIntr, cache1Pull0, cache1Status, dmaGet, dmaPut, cache1Push1, cacheGet, cachePut;
} NV3DTrap;
void    nv3d_trap(NV3D *g, NV3DTrap *t);

#endif

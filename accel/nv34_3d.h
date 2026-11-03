/*
 * NV34 3D engine (rankine, class 0x0697) — bring-up for OpenGL.
 *
 * PGRAPH context and init registers per Linux nouveau (nvkm/engine/gr/nv30.c, nv34.c, MIT),
 * initial 3D state per xf86-video-nouveau (nv30_exa.c NVAccelInitNV30TCL, MIT),
 * methods from nv30-40_3d.xml.h (rules-ng-ng, MIT). Draws using fixed-function transformation
 * (MODELVIEW/PROJECTION matrices) and a short fragment program.
 */
#ifndef NV34_3D_H
#define NV34_3D_H

#include "nv34_accel.h"

#define NV3D_SUBC       7               /* subchannel for the 3D object (originally scaled image 0x77) */

/* handles in RAMHT */
#define NV3D_HANDLE_3D  0x80000020
#define NV3D_HANDLE_VRAM 0x80000030

/* instances in PRAMIN (byte offset from the start of PRAMIN, BAR0 0x700000) */
#define NV3D_INST_3D    0x12100         /* object 0x0697 (16 B) */
#define NV3D_INST_DMA   0x12140         /* DMA object 0x3d spanning all of VRAM (16 B) */
#define NV3D_INST_CTXTAB 0x14000        /* context table 32 × 4 B */
#define NV3D_INST_CTX   0x20000         /* PGRAPH context of channel 0 (0x46dc B) */

typedef struct NV3D {
    NVAccel    *a;
    NVU32       fbOffset, pitch, width, height;
    NVU32       scratchOffset;          /* VRAM: zeta + fragment programs */
    NVU32       fpOffset;
} NV3D;

/* PGRAPH init for 3D (nv30_gr_init + NV34), context, objects, subchannel bindings.
 * Call after nva_init_engine + nva_reset_graphics. Returns kNVAOK or kNVATimeout. */
int     nv3d_init(NV3D *g, NVAccel *a, NVU32 fbOffset, NVU32 pitch, NVU32 width, NVU32 height);

/* Fragment program into VRAM. `swapMode`: 0 = the GPU reads the halves swapped (like PUSH_DATAs),
 * 1 = no byte swapping (for testing variants). */
void    nv3d_upload_fp(NV3D *g, const NVU32 *words, int count, int swapMode);
void    nv3d_use_fp(NV3D *g, NVU32 fpControl, NVU32 regControl);

/* Output through register combiners: final = primary color (bypasses the fragment program). */
void    nv3d_rc_primary(NV3D *g);

/* 3D object with/without the big-endian flag (0x00080000), rebound to its subchannel. */
void    nv3d_object_be(NV3D *g, int be);

/* Combiners: final = input `input` directly (0x04 primary color, 0x08 FP register H0…). */
void    nv3d_rc_final(NV3D *g, NVU32 input);

/* Combiners: final = texture0 × primary color (RGB and alpha). */
void    nv3d_rc_modulate_tex(NV3D *g);

/* RECT A8R8G8B8 texture (linear, unnormalized coordinates) in unit 0. */
void    nv3d_texture_rect(NV3D *g, NVU32 vramOffset, NVU32 w, NVU32 h, NVU32 pitch);

/* Rectangle from 2 triangles with texture coordinates 0..tw, 0..th (texels). */
void    nv3d_quad_tex(NV3D *g, int x, int y, int w, int h, int tw, int th, NVU32 r, NVU32 gr, NVU32 b, NVU32 al);

/* Z-buffer: z scale in the viewport (float bits), test LESS + write, clear depth to 1.0. */
void    nv3d_depth(NV3D *g, int enable, NVU32 zScale);
void    nv3d_clear_depth(NV3D *g);
void    nv3d_clear_depth_value(NV3D *g, NVU32 value, int writeEnable);

/* Blending: 0 = off, 1 = SRC_ALPHA / ONE_MINUS_SRC_ALPHA. */
void    nv3d_blend(NV3D *g, int enable);

/* Rectangle with depth z (float bits) and a color (combiners = primary color). */
void    nv3d_rect_z(NV3D *g, int x, int y, int w, int h, NVU32 z, NVU32 r, NVU32 gr, NVU32 b, NVU32 al);

/* Swizzled POT A8R8G8B8 texture in unit 0, REPEAT, linear filter. */
void    nv3d_texture_swz(NV3D *g, NVU32 vramOffset, NVU32 log2w, NVU32 log2h);

/* Rectangle with normalized texture coordinates (float bits). */
void    nv3d_quad_texf(NV3D *g, int x, int y, int w, int h, NVU32 s1, NVU32 t1);

/* Triangle in screen pixels, RGBA color 0..1 (float bits). */
void    nv3d_triangle(NV3D *g, int x0, int y0, int x1, int y1, int x2, int y2,
                      NVU32 r, NVU32 gr, NVU32 b, NVU32 al);

/* PGRAPH diagnostics after an error + FIFO recovery. */
typedef struct NV3DTrap {
    NVU32 intr, nsource, nstatus, addr, data, ctxUser, status;
    NVU32 fifoIntr, cache1Pull0, cache1Status, dmaGet, dmaPut, cache1Push1, cacheGet, cachePut;
} NV3DTrap;
void    nv3d_trap(NV3D *g, NV3DTrap *t);

#endif

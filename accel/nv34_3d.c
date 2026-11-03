/*
 * NV34 3D engine bring-up. Sources (MIT):
 *   Linux nouveau  nvkm/engine/gr/nv30.c (nv30_gr_init), nv34.c (nv34_gr_chan_new),
 *                  nv20.c (context table, automatic switching)
 *   xf86-video-nouveau  nv30_exa.c (NVAccelInitNV30TCL)
 *   nv30-40_3d.xml.h    method numbers
 */
#include "nv34_3d.h"

#define PRAMIN      0x700000

/* NV30_3D methods (nv30-40_3d.xml.h) */
#define M_OBJECT            0x0000
#define M_FLIP_SET_READ     0x0120
#define M_DMA_NOTIFY        0x0180
#define M_DMA_TEXTURE0      0x0184
#define M_DMA_COLOR0        0x0194
#define M_DMA_UNK1AC        0x01ac
#define M_DMA_UNK1B0        0x01b0
#define M_RT_HORIZ          0x0200
#define M_RT_ENABLE         0x0220
#define M_TEX_UNITS_ENABLE  0x023c
#define M_VIEWPORT_TX_ORIGIN 0x02b8
#define M_VIEWPORT_CLIP_H(i) (0x02c0 + (i) * 8)
#define M_DITHER_ENABLE     0x0300
#define M_ALPHA_FUNC_ENABLE 0x0304
#define M_BLEND_FUNC_ENABLE 0x0310
#define M_COLOR_MASK        0x0324
#define M_STENCIL_ENABLE(i) (0x0328 + (i) * 32)
#define M_SHADE_MODEL       0x0368
#define M_COLOR_LOGIC_OP_ENABLE 0x0374
#define M_DEPTH_RANGE_NEAR  0x0394
#define M_MIPMAP_ROUNDING   0x03b0
#define M_MODELVIEW(i)      (0x0480 + (i) * 4)
#define M_PROJECTION(i)     (0x0680 + (i) * 4)
#define M_SCISSOR_HORIZ     0x08c0
#define M_FP_ACTIVE_PROGRAM 0x08e4
#define M_RC_FINAL0         0x08f4
#define M_VIEWPORT_HORIZ    0x0a00
#define M_VIEWPORT_TRANSLATE_X 0x0a20
#define M_DEPTH_WRITE_ENABLE 0x0a70
#define M_POLYGON_OFFSET_FACTOR 0x0a78
#define M_FP_REG_CONTROL    0x1450
#define M_FLATSHADE_FIRST   0x1454
#define M_POLYGON_STIPPLE(i) (0x1480 + (i) * 4)
#define M_VERTEX_BEGIN_END  0x1808
#define M_POLYGON_MODE_FRONT 0x1828
#define M_CULL_FACE_ENABLE  0x183c
#define M_VTX_ATTR_2I(i)    (0x1900 + (i) * 4)
#define M_TEX_ENABLE(i)     (0x1a0c + (i) * 32)
#define M_VTX_ATTR_4F(i)    (0x1c00 + (i) * 16)
#define M_RC_IN_ALPHA(i)    (0x0900 + (i) * 32)
#define M_DEPTH_FUNC        0x0a6c
#define M_VTX_ATTR_2F(i)    (0x1880 + (i) * 8)
#define M_CLEAR_DEPTH_VALUE 0x1d8c
#define M_CLEAR_BUFFERS     0x1d94
#define M_TEX_OFFSET(i)     (0x1a00 + (i) * 32)
#define M_TEX_MATRIX_ENABLE(i) (0x0240 + (i) * 4)
#define M_FP_CONTROL        0x1d60
#define M_MULTISAMPLE_CONTROL 0x1d7c
#define M_COORD_CONVENTIONS 0x1d88

#define F_0     0x00000000
#define F_1     0x3f800000

static void begin(NV3D *g, NVU32 mthd, NVU32 n)
{
    nva_begin(g->a, NV3D_SUBC, mthd, n);
}

static void data(NV3D *g, NVU32 v)
{
    nva_push(g->a, v);
}

static void wr(NV3D *g, NVU32 reg, NVU32 v)
{
    nva_wr32(g->a, reg, v);
}

static NVU32 rd(NV3D *g, NVU32 reg)
{
    return nva_rd32(g->a, reg);
}

/* RAMHT (0x10000, 4 KB = 512 entries): hash per nouveau nv04_ramht */
static void ramht_insert(NV3D *g, NVU32 handle, NVU32 context)
{
    NVU32 h = handle, hash = 0, i;
    while (h) {
        hash ^= h & 0x1ff;
        h >>= 9;
    }
    hash <<= 3;
    for (i = 0; i < 512; i++, hash = (hash + 8) & 0xfff) {
        NVU32 cur = rd(g, PRAMIN + 0x10000 + hash);
        if (cur == 0 || cur == handle) {
            wr(g, PRAMIN + 0x10000 + hash, handle);
            wr(g, PRAMIN + 0x10000 + hash + 4, context);
            return;
        }
    }
}

int nv3d_init(NV3D *g, NVAccel *a, NVU32 fbOffset, NVU32 pitch, NVU32 width, NVU32 height)
{
    NVU32 i, ctx = PRAMIN + NV3D_INST_CTX;

    g->a = a;
    g->fbOffset = fbOffset;
    g->pitch = pitch;
    g->width = width;
    g->height = height;
    g->scratchOffset = 0x01000000;      /* 16 MB: zeta (≤ 3 MB) — only NV34 GL Test without QD Accel */
    g->fpOffset = 0x003F0000;           /* fragment programs: between the framebuffer (≤ 3 MB + 16 KB)
                                         * and the QD Accel VRAM allocator (from 4 MB) */

    /* --- nv30_gr_init (Linux nouveau) --- */
    wr(g, 0x400100, 0xFFFFFFFF);        /* INTR: clear */
    wr(g, 0x400140, 0x00000000);        /* INTR_EN: no interrupts (we have no OS 9 handler) */
    wr(g, 0x400080, 0xFFFFFFFF);        /* DEBUG_0 */
    wr(g, 0x400080, 0x00000000);
    wr(g, 0x400084, 0x401287c0);        /* DEBUG_1 */
    wr(g, 0x400890, 0x01b463ff);
    wr(g, 0x40008c, 0xf2de0475);        /* DEBUG_3 (bit 2 = automatic context switching) */
    wr(g, 0x400090, 0x00008000);        /* DEBUG_4 */
    wr(g, 0x400610, 0xf04bdff6);        /* LIMIT_VIOL_PIX */
    wr(g, 0x400B80, 0x1003d888);
    wr(g, 0x400B84, 0x0c000000);
    wr(g, 0x400098, 0x00000000);
    wr(g, 0x40009C, 0x0005ad00);
    wr(g, 0x400B88, 0x62ff00ff);
    wr(g, 0x4000a0, 0x00000000);
    wr(g, 0x4000a4, 0x00000008);
    wr(g, 0x4008a8, 0xb784a400);
    wr(g, 0x400ba0, 0x002f8685);
    wr(g, 0x400ba4, 0x00231f3f);
    wr(g, 0x4008a4, 0x40000020);
    /* NV34 */
    wr(g, 0x400750, 0x00EA0004);
    wr(g, 0x400754, 0x00200201);
    wr(g, 0x400750, 0x00EA0008);
    wr(g, 0x400754, 0x00000008);
    wr(g, 0x400750, 0x00EA0000);
    wr(g, 0x400754, 0x00000032);
    wr(g, 0x400750, 0x00E00004);
    wr(g, 0x400754, 0x00000002);
    wr(g, 0x4000c0, 0x00000016);
    wr(g, 0x4009A4, rd(g, 0x100200));
    wr(g, 0x4009A8, rd(g, 0x100204));

    /* tile regions for 3D too: besides PGRAPH 0x400900.. nv20_gr_tile also writes the RDI tables
     * (EA0010 address, EA0030 limit, EA0050 pitch). Without them 3D draws into the tiled OF framebuffer
     * as if it were linear → scrambled rows (gl-2). */
    for (i = 0; i < 8; i++) {
        NVU32 addr = rd(g, 0x100240 + i * 16);
        NVU32 limit = rd(g, 0x100244 + i * 16);
        NVU32 tpitch = rd(g, 0x100248 + i * 16);
        wr(g, 0x400904 + i * 16, limit);    /* TLIMIT */
        wr(g, 0x400908 + i * 16, tpitch);   /* TSIZE */
        wr(g, 0x400900 + i * 16, addr);     /* TILE */
        wr(g, 0x400750, 0x00EA0030 + 4 * i);
        wr(g, 0x400754, limit);
        wr(g, 0x400750, 0x00EA0050 + 4 * i);
        wr(g, 0x400754, tpitch);
        wr(g, 0x400750, 0x00EA0010 + 4 * i);
        wr(g, 0x400754, addr);
    }

    /* --- channel 0 context (nv34_gr_chan_new) --- */
    for (i = 0; i < 0x46e0; i += 4)
        wr(g, ctx + i, 0);
    wr(g, ctx + 0x0028, 0x00000001);    /* | chid << 24, chid = 0 */
    wr(g, ctx + 0x040c, 0x01000101);
    wr(g, ctx + 0x0420, 0x00000111);
    wr(g, ctx + 0x0424, 0x00000060);
    wr(g, ctx + 0x0440, 0x00000080);
    wr(g, ctx + 0x0444, 0xffff0000);
    wr(g, ctx + 0x0448, 0x00000001);
    wr(g, ctx + 0x045c, 0x44400000);
    wr(g, ctx + 0x0480, 0xffff0000);
    for (i = 0x04d4; i < 0x04dc; i += 4)
        wr(g, ctx + i, 0x0fff0000);
    wr(g, ctx + 0x04e0, 0x00011100);
    for (i = 0x04fc; i < 0x053c; i += 4)
        wr(g, ctx + i, 0x07ff0000);
    wr(g, ctx + 0x0544, 0x4b7fffff);
    wr(g, ctx + 0x057c, 0x00000080);
    wr(g, ctx + 0x0580, 0x30201000);
    wr(g, ctx + 0x0584, 0x70605040);
    wr(g, ctx + 0x0588, 0xb8a89888);
    wr(g, ctx + 0x058c, 0xf8e8d8c8);
    wr(g, ctx + 0x05a0, 0xb0000000);
    for (i = 0x05f0; i < 0x0630; i += 4)
        wr(g, ctx + i, 0x00010588);
    for (i = 0x0630; i < 0x0670; i += 4)
        wr(g, ctx + i, 0x00030303);
    for (i = 0x06b0; i < 0x06f0; i += 4)
        wr(g, ctx + i, 0x0008aae4);
    for (i = 0x06f0; i < 0x0730; i += 4)
        wr(g, ctx + i, 0x01012000);
    for (i = 0x0730; i < 0x0770; i += 4)
        wr(g, ctx + i, 0x00080008);
    wr(g, ctx + 0x0850, 0x00040000);
    wr(g, ctx + 0x0854, 0x00010000);
    for (i = 0x0858; i < 0x0868; i += 4)
        wr(g, ctx + i, 0x00040004);
    for (i = 0x15ac; i <= 0x271c; i += 16) {
        wr(g, ctx + i + 0, 0x10700ff9);
        wr(g, ctx + i + 4, 0x0436086c);
        wr(g, ctx + i + 8, 0x000c001b);
    }
    for (i = 0x274c; i < 0x275c; i += 4)
        wr(g, ctx + i, 0x0000ffff);
    wr(g, ctx + 0x2ae0, 0x3f800000);
    wr(g, ctx + 0x2e9c, 0x3f800000);
    wr(g, ctx + 0x2eb0, 0x3f800000);
    wr(g, ctx + 0x2edc, 0x40000000);
    wr(g, ctx + 0x2ee0, 0x3f800000);
    wr(g, ctx + 0x2ee4, 0x3f000000);
    wr(g, ctx + 0x2eec, 0x40000000);
    wr(g, ctx + 0x2ef0, 0x3f800000);
    wr(g, ctx + 0x2ef8, 0xbf800000);
    wr(g, ctx + 0x2f00, 0xbf800000);

    /* context table: channel 0 → NV3D_INST_CTX */
    for (i = 0; i < 32 * 4; i += 4)
        wr(g, PRAMIN + NV3D_INST_CTXTAB + i, 0);
    wr(g, PRAMIN + NV3D_INST_CTXTAB, NV3D_INST_CTX >> 4);
    wr(g, 0x400780, NV3D_INST_CTXTAB >> 4); /* CHANNEL_CTX_TABLE */

    wr(g, 0x400144, 0x10000100);        /* CTX_CONTROL */
    wr(g, 0x400714, 0xFFFFFFFF);        /* STATE */
    wr(g, 0x40075c, 0x00000001);

    /* current context invalid → the first method on channel 0 loads the context from the table */
    wr(g, 0x400144, 0x10000000);
    wr(g, 0x400148, (rd(g, 0x400148) & 0x00ffffff) | 0x1f000000);

    /* --- objects: 3D (0x0697) and DMA 0x3d spanning all of VRAM --- */
    wr(g, PRAMIN + NV3D_INST_3D + 0, 0x00080697);     /* BE flag: correct colors (gl-4) */
    wr(g, PRAMIN + NV3D_INST_3D + 4, 0);
    wr(g, PRAMIN + NV3D_INST_3D + 8, 0);
    wr(g, PRAMIN + NV3D_INST_3D + 12, 0);
    wr(g, PRAMIN + NV3D_INST_DMA + 0, 0x0000303d);     /* IN_MEMORY, VRAM, PT present+linear */
    wr(g, PRAMIN + NV3D_INST_DMA + 4, a->vramSize - 1);
    wr(g, PRAMIN + NV3D_INST_DMA + 8, 0x00000002);     /* address 0, RW */
    wr(g, PRAMIN + NV3D_INST_DMA + 12, 0x00000002);
    ramht_insert(g, NV3D_HANDLE_3D, 0x80010000 | (NV3D_INST_3D >> 4));
    ramht_insert(g, NV3D_HANDLE_VRAM, 0x80010000 | (NV3D_INST_DMA >> 4));

    /* after the context load the subchannel bindings are gone → bind everything again */
    for (i = 0; i < 7; i++) {
        nva_begin(a, i, M_OBJECT, 1);
        nva_push(a, 0x80000010 + i);
    }
    begin(g, M_OBJECT, 1);
    data(g, NV3D_HANDLE_3D);

    /* --- NVAccelInitNV30TCL (xf86-video-nouveau) --- */
    begin(g, M_DMA_TEXTURE0, 3);
    data(g, NV3D_HANDLE_VRAM);
    data(g, NV3D_HANDLE_VRAM);
    data(g, NV3D_HANDLE_VRAM);
    begin(g, M_DMA_UNK1AC, 1);
    data(g, NV3D_HANDLE_VRAM);
    begin(g, M_DMA_COLOR0, 2);
    data(g, NV3D_HANDLE_VRAM);
    data(g, NV3D_HANDLE_VRAM);
    begin(g, M_DMA_UNK1B0, 1);
    data(g, NV3D_HANDLE_VRAM);
    for (i = 1; i < 8; i++) {
        begin(g, M_VIEWPORT_CLIP_H(i), 2);
        data(g, 0);
        data(g, 0);
    }
    begin(g, M_RT_ENABLE, 1);
    data(g, 1);
    begin(g, M_MIPMAP_ROUNDING, 1);
    data(g, 0x00100000);
    begin(g, M_FLATSHADE_FIRST, 1);
    data(g, 0);
    begin(g, 0x1d80, 1);
    data(g, 3);
    begin(g, M_FP_REG_CONTROL, 1);
    data(g, 0x00030004);
    begin(g, 0x1e98, 1);
    data(g, 0);
    begin(g, 0x17e0, 3);
    data(g, 0);
    data(g, 0);
    data(g, F_1);
    begin(g, 0x1f80, 16);
    for (i = 0; i < 16; i++)
        data(g, i == 8 ? 0x0000ffff : 0);
    begin(g, M_FLIP_SET_READ, 3);
    data(g, 0);
    data(g, 1);
    data(g, 2);
    begin(g, M_COORD_CONVENTIONS, 1);
    data(g, 0x00001200);
    begin(g, M_MULTISAMPLE_CONTROL, 1);
    data(g, 0xffff0000);
    begin(g, M_STENCIL_ENABLE(0), 1);
    data(g, 0);
    begin(g, M_STENCIL_ENABLE(1), 1);
    data(g, 0);
    begin(g, M_ALPHA_FUNC_ENABLE, 1);
    data(g, 0);
    begin(g, M_DEPTH_WRITE_ENABLE, 2);
    data(g, 0);
    data(g, 0);
    begin(g, M_COLOR_MASK, 1);
    data(g, 0x01010101);
    begin(g, M_CULL_FACE_ENABLE, 1);
    data(g, 0);
    begin(g, M_BLEND_FUNC_ENABLE, 5);
    data(g, 0);
    data(g, 0);
    data(g, 0);
    data(g, 0);
    data(g, 0x8006);
    begin(g, M_COLOR_LOGIC_OP_ENABLE, 2);
    data(g, 0);
    data(g, 0x1503);
    begin(g, M_DITHER_ENABLE, 1);
    data(g, 1);
    begin(g, M_SHADE_MODEL, 1);
    data(g, 0x1d01);
    begin(g, M_POLYGON_OFFSET_FACTOR, 2);
    data(g, F_0);
    data(g, F_0);
    begin(g, M_POLYGON_MODE_FRONT, 2);
    data(g, 0x1b02);
    data(g, 0x1b02);
    for (i = 0; i < 4; i++) {
        begin(g, M_TEX_ENABLE(i), 1);
        data(g, 0);
    }
    begin(g, M_POLYGON_STIPPLE(0), 0x20);
    for (i = 0; i < 0x20; i++)
        data(g, 0xFFFFFFFF);
    begin(g, M_DEPTH_RANGE_NEAR, 2);
    data(g, F_0);
    data(g, F_1);

    /* render target = screen framebuffer (A8R8G8B8 linear + Z24S8 in scratch VRAM) */
    begin(g, M_RT_HORIZ, 5);
    data(g, width << 16);
    data(g, height << 16);
    data(g, 0x148);
    data(g, (pitch << 16) | pitch);
    data(g, fbOffset);
    begin(g, 0x0214, 1);                /* ZETA_OFFSET */
    data(g, g->scratchOffset);
    begin(g, M_VIEWPORT_TX_ORIGIN, 1);
    data(g, 0);
    begin(g, M_VIEWPORT_HORIZ, 2);
    data(g, width << 16);
    data(g, height << 16);
    begin(g, M_VIEWPORT_CLIP_H(0), 2);
    data(g, (width - 1) << 16);
    data(g, (height - 1) << 16);
    begin(g, M_SCISSOR_HORIZ, 2);
    data(g, width << 16);
    data(g, height << 16);
    begin(g, M_VIEWPORT_TRANSLATE_X, 8);
    data(g, F_0);
    data(g, F_0);
    data(g, F_0);
    data(g, F_0);
    data(g, F_1);
    data(g, F_1);
    data(g, F_1);
    data(g, F_0);
    begin(g, M_MODELVIEW(0), 16);
    for (i = 0; i < 16; i++)
        data(g, (i % 5) == 0 ? F_1 : F_0);
    begin(g, M_PROJECTION(0), 16);
    for (i = 0; i < 16; i++)
        data(g, (i % 5) == 0 ? F_1 : F_0);

    nva_kickoff(a);
    return nva_sync(a, 60);
}

static NVU32 bswap32(NVU32 v)
{
    return (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24);
}

/* gl-5: with the BE 3D object swapMode 1 applies (no byte swapping) */
void nv3d_upload_fp(NV3D *g, const NVU32 *words, int count, int swapMode)
{
    volatile NVU32 *dst = (volatile NVU32 *)(g->a->vram + g->fpOffset);
    int i;

    for (i = 0; i < count; i++) {
        NVU32 v = words[i];
        NVU32 hs = (v << 16) | (v >> 16);           /* PUSH_DATAs: halves swapped */
        /* the CPU is BE, the GPU reads VRAM as LE: swapMode 0 → the GPU sees hs */
        dst[i] = swapMode == 0 ? bswap32(hs) : hs;
    }
    __asm__ volatile("sync" ::: "memory");
}

void nv3d_use_fp(NV3D *g, NVU32 fpControl, NVU32 regControl)
{
    begin(g, M_FP_ACTIVE_PROGRAM, 1);
    data(g, g->fpOffset | 0x00000001);  /* DMA0 = DMA_TEXTURE0 = VRAM */
    begin(g, M_FP_CONTROL, 1);
    data(g, fpControl);
    begin(g, M_FP_REG_CONTROL, 1);
    data(g, regControl);
    begin(g, M_TEX_UNITS_ENABLE, 1);
    data(g, 0);
}

void nv3d_rc_primary(NV3D *g)
{
    /* final = D = primary color, alpha G = primary alpha; like the DDX: FINAL0, FINAL1, RC_ENABLE */
    begin(g, M_RC_FINAL0, 3);
    data(g, 0x00000004);                /* D_INPUT_PRIMARY_COLOR */
    data(g, 0x00001400);                /* G = PRIMARY_COLOR | alpha */
    data(g, 0x01000101);
}

void nv3d_triangle(NV3D *g, int x0, int y0, int x1, int y1, int x2, int y2,
                   NVU32 r, NVU32 gr, NVU32 b, NVU32 al)
{
    int xs[3], ys[3], i;

    xs[0] = x0; ys[0] = y0;
    xs[1] = x1; ys[1] = y1;
    xs[2] = x2; ys[2] = y2;
    begin(g, M_VERTEX_BEGIN_END, 1);
    data(g, 0x00000005);                /* TRIANGLES */
    for (i = 0; i < 3; i++) {
        begin(g, M_VTX_ATTR_4F(3), 4);  /* color (attribute 3) */
        data(g, r);
        data(g, gr);
        data(g, b);
        data(g, al);
        begin(g, M_VTX_ATTR_2I(0), 1);  /* position (attribute 0) emits the vertex */
        data(g, ((NVU32)(ys[i] & 0xffff) << 16) | (NVU32)(xs[i] & 0xffff));
    }
    begin(g, M_VERTEX_BEGIN_END, 1);
    data(g, 0);
    nva_kickoff(g->a);
}

void nv3d_trap(NV3D *g, NV3DTrap *t)
{
    t->intr = rd(g, 0x400100);
    t->nsource = rd(g, 0x400108);
    t->nstatus = rd(g, 0x400104);
    t->addr = rd(g, 0x400704);
    t->data = rd(g, 0x400708);
    t->ctxUser = rd(g, 0x400148);
    t->status = rd(g, 0x400700);
    t->fifoIntr = rd(g, 0x002100);      /* PFIFO INTR */
    t->cache1Pull0 = rd(g, 0x003250);
    t->cache1Status = rd(g, 0x003214);
    t->dmaPut = rd(g, 0x003240);
    t->dmaGet = rd(g, 0x003244);
    t->cache1Push1 = rd(g, 0x003204);
    t->cacheGet = rd(g, 0x003270);
    t->cachePut = rd(g, 0x003210);
    if (t->intr) {
        wr(g, 0x400100, t->intr);       /* acknowledge */
        wr(g, 0x400720, 0x00000001);    /* PGRAPH FIFO continues */
    }
}

void nv3d_object_be(NV3D *g, int be)
{
    nva_sync(g->a, 60);
    wr(g, PRAMIN + NV3D_INST_3D, 0x00000697 | (be ? 0x00080000 : 0));
    begin(g, M_OBJECT, 1);
    data(g, NV3D_HANDLE_3D);
    nva_kickoff(g->a);
}

void nv3d_rc_modulate_tex(NV3D *g)
{
    /* stage 0: A = texture0 (fragprog H0, source 0x08), B = primary color (0x04), A×B → spare0
     * (xf86-video-nouveau NV30EXAPrepareComposite) */
    begin(g, M_RC_IN_ALPHA(0), 6);
    data(g, (0x18u << 24) | (0x14u << 16));     /* alpha: tex0.a × prim.a */
    data(g, (0x08u << 24) | (0x04u << 16));     /* rgb:  tex0.rgb × prim.rgb */
    data(g, 0);                                 /* RC_COLOR0 */
    data(g, 0);                                 /* RC_COLOR1 */
    data(g, 0x00000c00);                        /* OUT_ALPHA: AB → spare0 */
    data(g, 0x00000c00);                        /* OUT_RGB:   AB → spare0 */
    begin(g, M_RC_FINAL0, 3);
    data(g, 0x0000000c);                        /* D = spare0 */
    data(g, 0x00001c00);                        /* G = spare0.a */
    data(g, 0x01000101);
}

void nv3d_texture_rect(NV3D *g, NVU32 vramOffset, NVU32 w, NVU32 h, NVU32 pitch)
{
    NVU32 log2w = 0, log2h = 0;
    while ((1u << log2w) < w)
        log2w++;
    while ((1u << log2h) < h)
        log2h++;
    begin(g, M_TEX_OFFSET(0), 8);
    data(g, vramOffset);
    data(g, (1u << 16) | 0x08 | 0x20 | (0x12u << 8) | (log2w << 20) | (log2h << 24) | 0x01);
    data(g, (3u << 0) | (3u << 8) | (3u << 16));          /* WRAP: clamp */
    data(g, 0x40000000);                                  /* ENABLE */
    data(g, (pitch << 16) | 0xAAE4);                      /* SWIZZLE: a8r8g8b8 */
    data(g, (1u << 16) | (1u << 24) | 0x2000);            /* FILTER: nearest */
    data(g, (w << 16) | h);                               /* NPOT_SIZE */
    data(g, 0);                                           /* BORDER */
    begin(g, M_TEX_MATRIX_ENABLE(0), 1);
    data(g, 0);
    begin(g, M_TEX_UNITS_ENABLE, 1);
    data(g, 1);
}

static void vtx_tex(NV3D *g, int x, int y, int s, int t, NVU32 r, NVU32 gr, NVU32 b, NVU32 al)
{
    begin(g, M_VTX_ATTR_4F(3), 4);
    data(g, r);
    data(g, gr);
    data(g, b);
    data(g, al);
    begin(g, M_VTX_ATTR_2I(8), 1);              /* texture coordinates 0 (attribute 8) */
    data(g, ((NVU32)(t & 0xffff) << 16) | (NVU32)(s & 0xffff));
    begin(g, M_VTX_ATTR_2I(0), 1);
    data(g, ((NVU32)(y & 0xffff) << 16) | (NVU32)(x & 0xffff));
}

void nv3d_quad_tex(NV3D *g, int x, int y, int w, int h, int tw, int th, NVU32 r, NVU32 gr, NVU32 b, NVU32 al)
{
    begin(g, M_VERTEX_BEGIN_END, 1);
    data(g, 0x00000005);
    vtx_tex(g, x, y, 0, 0, r, gr, b, al);
    vtx_tex(g, x + w, y, tw, 0, r, gr, b, al);
    vtx_tex(g, x, y + h, 0, th, r, gr, b, al);
    vtx_tex(g, x + w, y, tw, 0, r, gr, b, al);
    vtx_tex(g, x + w, y + h, tw, th, r, gr, b, al);
    vtx_tex(g, x, y + h, 0, th, r, gr, b, al);
    begin(g, M_VERTEX_BEGIN_END, 1);
    data(g, 0);
    nva_kickoff(g->a);
}

void nv3d_rc_final(NV3D *g, NVU32 input)
{
    begin(g, M_RC_FINAL0, 3);
    data(g, input);                             /* D = input */
    data(g, (input << 8) | 0x1000);             /* G = input.alpha */
    data(g, 0x01000101);
}

void nv3d_depth(NV3D *g, int enable, NVU32 zScale)
{
    begin(g, M_VIEWPORT_TRANSLATE_X, 8);
    data(g, F_0);
    data(g, F_0);
    data(g, F_0);
    data(g, F_0);
    data(g, F_1);
    data(g, F_1);
    data(g, zScale);
    data(g, F_0);
    begin(g, M_DEPTH_FUNC, 1);
    data(g, 0x00000201);                        /* LESS */
    begin(g, M_DEPTH_WRITE_ENABLE, 2);
    data(g, enable);
    data(g, enable);
}

void nv3d_clear_depth(NV3D *g)
{
    begin(g, M_DEPTH_WRITE_ENABLE, 1);          /* gl-8: with depth write; ffffff00 did not work */
    data(g, 1);
    begin(g, M_CLEAR_DEPTH_VALUE, 1);
    data(g, 0xffffffff);                        /* depth 1.0 (stencil ff) */
    begin(g, M_CLEAR_BUFFERS, 1);
    data(g, 0x00000003);                        /* DEPTH | STENCIL */
    nva_kickoff(g->a);
}

void nv3d_blend(NV3D *g, int enable)
{
    if (!enable) {
        begin(g, M_BLEND_FUNC_ENABLE, 1);
        data(g, 0);
        return;
    }
    begin(g, M_BLEND_FUNC_ENABLE, 5);
    data(g, 1);
    data(g, (0x0302u << 16) | 0x0302);          /* SRC: SRC_ALPHA */
    data(g, (0x0303u << 16) | 0x0303);          /* DST: ONE_MINUS_SRC_ALPHA */
    data(g, 0);
    data(g, 0x80068006);                        /* FUNC_ADD */
}

static void vtx4(NV3D *g, NVU32 x, NVU32 y, NVU32 z, NVU32 r, NVU32 gr, NVU32 b, NVU32 al)
{
    begin(g, M_VTX_ATTR_4F(3), 4);
    data(g, r);
    data(g, gr);
    data(g, b);
    data(g, al);
    begin(g, M_VTX_ATTR_4F(0), 4);
    data(g, x);
    data(g, y);
    data(g, z);
    data(g, F_1);
}

static NVU32 fbits(float f)
{
    union { float f; NVU32 u; } c;
    c.f = f;
    return c.u;
}

void nv3d_rect_z(NV3D *g, int x, int y, int w, int h, NVU32 z, NVU32 r, NVU32 gr, NVU32 b, NVU32 al)
{
    NVU32 x0 = fbits((float)x), y0 = fbits((float)y), x1 = fbits((float)(x + w)), y1 = fbits((float)(y + h));
    begin(g, M_VERTEX_BEGIN_END, 1);
    data(g, 0x00000005);
    vtx4(g, x0, y0, z, r, gr, b, al);
    vtx4(g, x1, y0, z, r, gr, b, al);
    vtx4(g, x0, y1, z, r, gr, b, al);
    vtx4(g, x1, y0, z, r, gr, b, al);
    vtx4(g, x1, y1, z, r, gr, b, al);
    vtx4(g, x0, y1, z, r, gr, b, al);
    begin(g, M_VERTEX_BEGIN_END, 1);
    data(g, 0);
    nva_kickoff(g->a);
}

void nv3d_texture_swz(NV3D *g, NVU32 vramOffset, NVU32 log2w, NVU32 log2h)
{
    begin(g, M_TEX_OFFSET(0), 8);
    data(g, vramOffset);
    data(g, (1u << 16) | 0x08 | 0x20 | 0x600 | (log2w << 20) | (log2h << 24) | 0x01);
    data(g, (1u << 0) | (1u << 8) | (1u << 16));          /* WRAP: REPEAT */
    data(g, 0x40000000);
    data(g, 0xAAE4);                                      /* SWIZZLE a8r8g8b8 (no rect pitch) */
    data(g, 0x00020000 | 0x02000000 | 0x2000);            /* FILTER: linear */
    data(g, ((1u << log2w) << 16) | (1u << log2h));
    data(g, 0);
    begin(g, M_TEX_MATRIX_ENABLE(0), 1);
    data(g, 0);
    begin(g, M_TEX_UNITS_ENABLE, 1);
    data(g, 1);
}

static void vtx_texf(NV3D *g, int x, int y, NVU32 s, NVU32 t)
{
    begin(g, M_VTX_ATTR_4F(3), 4);
    data(g, F_1);
    data(g, F_1);
    data(g, F_1);
    data(g, F_1);
    begin(g, M_VTX_ATTR_2F(8), 2);
    data(g, s);
    data(g, t);
    begin(g, M_VTX_ATTR_2I(0), 1);
    data(g, ((NVU32)(y & 0xffff) << 16) | (NVU32)(x & 0xffff));
}

void nv3d_quad_texf(NV3D *g, int x, int y, int w, int h, NVU32 s1, NVU32 t1)
{
    begin(g, M_VERTEX_BEGIN_END, 1);
    data(g, 0x00000005);
    vtx_texf(g, x, y, F_0, F_0);
    vtx_texf(g, x + w, y, s1, F_0);
    vtx_texf(g, x, y + h, F_0, t1);
    vtx_texf(g, x + w, y, s1, F_0);
    vtx_texf(g, x + w, y + h, s1, t1);
    vtx_texf(g, x, y + h, F_0, t1);
    begin(g, M_VERTEX_BEGIN_END, 1);
    data(g, 0);
    nva_kickoff(g->a);
}

void nv3d_clear_depth_value(NV3D *g, NVU32 value, int writeEnable)
{
    begin(g, M_DEPTH_WRITE_ENABLE, 1);
    data(g, writeEnable);
    begin(g, 0x032c, 1);                        /* STENCIL_MASK (stage 0) = 0xff so that the clear writes stencil */
    data(g, 0xff);
    begin(g, M_CLEAR_DEPTH_VALUE, 1);
    data(g, value);
    begin(g, M_CLEAR_BUFFERS, 1);
    data(g, 0x00000003);
    nva_kickoff(g->a);
}

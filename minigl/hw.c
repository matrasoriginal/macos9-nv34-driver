/*
 * NV34 MiniGL — hardware layer (NV30 3D class 0x0697, methods from nv30-40_3d.xml.h, MIT).
 * Based on the NV34 RAVE engine (nv34rave/engine.c): the same render targets (color and zeta
 * with the same pitch), inline vertices (VERTEX_DATA), combiners, swizzled textures, 2D blit.
 */
#include <MacMemory.h>
#include <Events.h>
#include "minigl.h"

#define SUBC                7
#define M_DMA_VTXBUF0       0x019c
#define M_RT_HORIZ          0x0200
#define M_TEX_UNITS_ENABLE  0x023c
#define M_FOG_ENABLE        0x036c
#define M_FOG_COORD_DIST    0x08c8
#define M_TEX_MATRIX_ENABLE 0x0240
#define M_VIEWPORT_CLIP_H   0x02c0
#define M_MODELVIEW0        0x0480
#define M_PROJECTION0       0x0680
#define M_DITHER_ENABLE     0x0300
#define M_ALPHA_FUNC_ENABLE 0x0304
#define M_BLEND_FUNC_ENABLE 0x0310
#define M_COLOR_MASK        0x0324
#define M_SCISSOR_HORIZ     0x08c0
#define M_FP_ACTIVE_PROGRAM 0x08e4
#define M_RC_FINAL0         0x08f4
#define M_RC_IN_ALPHA0      0x0900
#define M_VIEWPORT_HORIZ    0x0a00
#define M_VIEWPORT_TRANSLATE_X 0x0a20
#define M_DEPTH_FUNC        0x0a6c
#define M_DEPTH_WRITE       0x0a70
#define M_FP_REG_CONTROL    0x1450
#define M_VTXBUF0           0x1680
#define M_VTX_CACHE_INV     0x1710
#define M_VTXFMT            0x1740
#define M_VB_ELEMENT_U16    0x1800
#define M_VB_ELEMENT_U32    0x1804
#define M_VB_VERTEX_BATCH   0x1814
#define M_VERTEX_BEGIN_END  0x1808
#define M_VERTEX_DATA       0x1818
#define M_CULL_FACE         0x1830
#define M_CULL_FACE_ENABLE  0x183c
#define M_TEX_OFFSET0       0x1a00
#define M_DEPTH_CONTROL     0x1d78
#define M_FP_CONTROL        0x1d60
#define M_CLEAR_DEPTH_VALUE 0x1d8c
#define M_CLEAR_COLOR_VALUE 0x1d90
#define M_CLEAR_BUFFERS     0x1d94

/* TXPH H0, a[tex0], t[0]; END (encoded for the BE 3D object, gl-5) */
static const NVU32 kFpTex[4] = { 0x18009e81, 0x1c9dc801, 0x0001c800, 0x3fe1c800 };

/* q3-12: two textures (GL_MODULATE on unit 1), then combiners × primary color as with one:
 *   TXP R1, a[tex1], t[1]
 *   TXPH H0, a[tex0], t[0]
 *   MULH H0, H0, R1; END
 * The program lies after the first one (fpOffset + FP2_OFF). */
#define FP2_OFF 0x100
static const NVU32 kFpTex2[12] = {
    0x1802be02, 0x1c9dc801, 0x0001c800, 0x3fe1c800,
    0x18009e80, 0x1c9dc801, 0x0001c800, 0x3fe1c800,
    0x02001e81, 0x1c9dc900, 0x0001c804, 0x3fe1c800,
};

/* like nv3d_upload_fp (swapMode 1), at offset off */
static void fp_upload(NVU32 off, const NVU32 *words, int count)
{
    volatile NVU32 *dst = (volatile NVU32 *)(S->g->a->vram + S->g->fpOffset + off);
    int i;
    for (i = 0; i < count; i++)
        dst[i] = (words[i] << 16) | (words[i] >> 16);
    __asm__ volatile("sync" ::: "memory");
}

static int memcmp_w(const NVU32 *a, const NVU32 *b, int n)
{
    while (n--)
        if (*a++ != *b++)
            return 1;
    return 0;
}

HwTex *gTexPin;

static NVU32 gFrame = 1, gEpoch = 1;
static HwTex *gTexList;

/* last hardware state set (-1 = unknown) */
static long hwMode = -1, hwBlend = -1, hwAlpha = -1, hwDepth = -1, hwMask = -1, hwScis = -1, hwFmt = -1;
static long hwTex = -1, hwTexKey = -1, hwTexEpoch = -1;
static long hwTex1 = -1, hwTex1Key = -1, hwTex1Epoch = -1;
static long hwTnl = -1, hwMatGen = -1, hwCull = -1, hwMv = -1;
static long hwFin = -1, hwFogOn = -1, hwProg = -1;
static NVU32 hwFogK[6];
static int gVtx12 = -1, gColorBE, gCompact;        /* vertex format (opt-5, q3-17) */
static int gNoFog, gFogPlane;

static void invalidate(void)
{
    hwMode = hwBlend = hwAlpha = hwDepth = hwMask = hwScis = hwFmt = -1;
    hwTex = hwTexKey = hwTexEpoch = -1;
    hwTex1 = hwTex1Key = hwTex1Epoch = -1;
    hwTnl = hwMatGen = hwCull = hwMv = -1;
    hwFin = hwFogOn = hwProg = -1;
}

/* hl-2: if the GPU does not respond (timeout waiting for room in the push buffer), write nothing more —
 * writing past the reserved space would overwrite commands the GPU is still reading */
static int gHwDead;

static void hw_fail(const char *what)
{
    if (!gHwDead)
        lg("GPU not responding (%s): GET %x PUT %x, drawing disabled", what, S->a->errGet, S->a->errPut);
    gHwDead = 1;
}

static void begin(NVU32 m, NVU32 n)
{
    if (!gHwDead && nva_begin(S->a, SUBC, m, n) != kNVAOK)
        hw_fail("push buffer");
}
static void data(NVU32 v)            { if (!gHwDead) nva_push(S->a, v); }
static void dataf(float f)           { union { float f; NVU32 u; } c; c.f = f; if (!gHwDead) nva_push(S->a, c.u); }

void hw_kick(void)  { if (S) nva_kickoff(S->a); }
NVU32 gHwUploads, gHwUploadBytes, gHwSubUploads, gHwSyncs;
NVU32 gTUpl, gTEvict, gFrUplN, gFrUplB;            /* max-14: time spent writing textures to VRAM, evicting */
NVU32 gTEnd, gTFlush, gTWait, gTSwap;

static int gVbOn = -1, gVbUsed;

int hw_sync(void)
{
    NVU32 t0 = tb();
    int ok;
    gHwSyncs++;
    ok = S && !gHwDead && nva_sync(S->a, 120) == kNVAOK;
    if (S && !ok && !gHwDead && first_time(0x70000000)) {
        NV3DTrap tr;
        nv3d_trap(S->g, &tr);                       /* q3-19: which method stopped the GPU */
        lg("hw_sync: GPU did not finish within 2 s; PGRAPH intr %x nsource %x nstatus %x addr %x data %x status %x, "
           "PFIFO intr %x get %x put %x%s", tr.intr, tr.nsource, tr.nstatus, tr.addr, tr.data, tr.status,
           tr.fifoIntr, tr.dmaGet, tr.dmaPut, gVbUsed ? " (after drawing from VRAM, disabling it)" : "");
        if (gVbUsed)
            gVbOn = 0;
        if (tr.intr)
            ok = nva_sync(S->a, 120) == kNVAOK;     /* after acknowledging the error try to finish */
    }
    gTWait += tb() - t0;
    return ok;
}
NVU32 hw_frame(void) { return gFrame; }

/* hl-10: GPU fence via the channel's REF_CNT (method 0x50, read USER + 0x48, like nouveau nv10_fence).
 * On present the number of the finished frame is sent; a texture used in frame F may be overwritten in place
 * only once REF ≥ F (512 KB push buffer → the GPU may lag a whole frame behind; the flashlight in Half-Life
 * drew old frames with new lightmaps = black squares). */
static int gRefOK = -1;

static NVU32 gpu_done(void)
{
    if (gRefOK != 1)
        return gFrame - 1;                          /* no fence: as before */
    return nva_rd32(S->a, 0x800048);
}

static void ref_emit(NVU32 v)
{
    begin(0x0050, 1);
    data(v);
}

int hw_init(void)
{
    if (!shared() || S->state3D == 2)
        return 0;
    if (S->ensure3D() != 0)
        return 0;
    if (gRefOK < 0) {
        ref_emit(0x5A000001);
        hw_sync();
        gRefOK = nva_rd32(S->a, 0x800048) == 0x5A000001;
        ref_emit(0);
        hw_sync();
        lg("GPU fence (REF_CNT): %s", gRefOK ? "ok" : "not working");
        {
            /* q3-8: core and memory clocks (PLL as in nouveau nv04_pm / nv30: f = crystal × N / M >> P;
             * NV31+ may have a second stage in *_COEFF_B) — whether the GPU runs in a power-saving mode */
            NVU32 boot = nva_rd32(S->a, 0x101000);
            NVU32 xtal = (boot & 0x40) ? 14318 : 13500;    /* kHz */
            NVU32 n1 = nva_rd32(S->a, 0x680500), n2 = nva_rd32(S->a, 0x680570);
            NVU32 m1 = nva_rd32(S->a, 0x680504), m2 = nva_rd32(S->a, 0x680574);
            NVU32 nm = n1 & 0xff, nn = (n1 >> 8) & 0xff, np = (n1 >> 16) & 7;
            NVU32 mm = m1 & 0xff, mn = (m1 >> 8) & 0xff, mp = (m1 >> 16) & 7;
            lg("PLL: core %x/%x (~%u MHz), memory %x/%x (~%u MHz), crystal %u kHz", n1, n2,
               nm ? (xtal * nn / nm >> np) / 1000 : 0, m1, m2, mm ? (xtal * mn / mm >> mp) / 1000 : 0, xtal);
        }
    }
    return 1;
}

int hw_target_alloc(HwTarget *t, int w, int h, int bpp, int zbits)
{
    t->w = w;
    t->h = h;
    t->bpp = bpp;
    /* color and zeta with the same pitch (rave-2, rave-16). q3-10: at 16 bpp with a requested depth of
     * ≤ 16 bits Z16 — half the memory traffic for the depth test (Quake III is GPU bound);
     * boot-args nv34glz24 = always Z24S8 */
    t->z16 = bpp == 16 && zbits > 0 && zbits <= 16 && !gl_boot_arg("nv34glz24");
    t->colorPitch = t->zPitch = (w * (t->z16 ? 2 : 4) + 63) & ~63L;
    t->colorOff = S->vramAlloc(t->colorPitch * h, 4096);
    t->zOff = S->vramAlloc(t->zPitch * h, 4096);
    if (!t->colorOff || !t->zOff) {
        hw_target_free(t);
        return 0;
    }
    t->id = S->nextCtxId++;
    t->generation = (NVU32)-1;
    return 1;
}

void hw_target_free(HwTarget *t)
{
    if (!S)
        return;
    hw_sync();
    if (t->colorOff)
        S->vramFree(t->colorOff);
    if (t->zOff)
        S->vramFree(t->zOff);
    t->colorOff = t->zOff = 0;
    if (S->boundCtxId == t->id)
        S->boundCtxId = 0;
}

int hw_bind(HwTarget *t)
{
    int fresh;
    if (!S || S->state3D != 1 || !t->colorOff || gHwDead)
        return 0;
    if (S->boundCtxId == t->id && S->generation == t->generation)
        return 1;
    fresh = S->generation != t->generation;
    if (fresh) {
        nv3d_upload_fp(S->g, kFpTex, 4, 1);
        fp_upload(FP2_OFF, kFpTex2, 12);
        t->generation = S->generation;
    }
    begin(M_RT_HORIZ, 5);
    data(t->w << 16);
    data(t->h << 16);
    data((t->bpp == 16 ? 0x102 : 0x108) | (t->z16 ? 0x20 : 0x40));   /* color | Z16/Z24S8 | linear */
    data((t->zPitch << 16) | t->colorPitch);
    data(t->colorOff);
    begin(0x0214, 1);
    data(t->zOff);
    begin(M_VIEWPORT_HORIZ, 2);
    data(t->w << 16);
    data(t->h << 16);
    begin(M_VIEWPORT_CLIP_H, 2);
    data((t->w - 1) << 16);
    data((t->h - 1) << 16);
    begin(M_VIEWPORT_TRANSLATE_X, 8);
    dataf(0); dataf(0); dataf(0); dataf(0);
    dataf(1); dataf(1); dataf(1); dataf(0);
    begin(M_FP_ACTIVE_PROGRAM, 1);
    data(S->g->fpOffset | 1);
    begin(M_DMA_VTXBUF0, 1);                        /* q3-18: vertices in VRAM (VTXBUF without bit 31) */
    data(NV3D_HANDLE_VRAM);
    begin(M_FP_CONTROL, 1);
    data(0);
    begin(M_FP_REG_CONTROL, 1);
    data(0x0001000f);
    begin(M_TEX_MATRIX_ENABLE, 1);
    data(0);
    begin(M_CULL_FACE_ENABLE, 1);                  /* back-face culling is done by the CPU */
    data(0);
    begin(M_DEPTH_CONTROL, 1);                      /* clamp depth, do not clip */
    data(0x10);
    begin(M_DITHER_ENABLE, 1);
    data(t->bpp == 16);
    if (fresh) {                                    /* the first depth CLEAR after init does not take (rave-3) */
        begin(M_DEPTH_WRITE, 1);
        data(1);
        begin(M_CLEAR_DEPTH_VALUE, 1);
        data(0xffffffff);
        begin(M_CLEAR_BUFFERS, 1);
        data(0x03);
    }
    S->boundCtxId = t->id;
    invalidate();
    return 1;
}

/* opt-5: position 3×float (w = 1), color 4×U8_UNORM in one word, texture 4×float →
 * 8 words per textured vertex (previously 12), 4 without (previously 8). boot-args nv34glvtx12 =
 * the original format. Color R<<24|G<<16|B<<8|A (opt-6 with the reverse order had a pink tint);
 * nv34glcolorle = reverse order. */
static void vtx_init(void)
{
    if (gVtx12 < 0) {
        gVtx12 = gl_boot_arg("nv34glvtx12");
        gColorBE = !gl_boot_arg("nv34glcolorle");   /* opt-7: the GPU reads R in the top byte */
        gNoFog = gl_boot_arg("nv34glnofog");
        gFogPlane = gl_boot_arg("nv34glfogplane");
    }
}

static void set_vtxfmt(int tex)
{
    int i, key = tex | (gCompact << 4);
    if (hwFmt == key)
        return;
    begin(M_VTXFMT, 16);
    if (gVtx12) {
        NVU32 f = 0x2 | (4u << 4) | ((tex ? 48u : 32u) << 8);
        for (i = 0; i < 16; i++)
            data(i == 0 || i == 3 || (i == 8 && tex) ? f : 0x2);
    } else if (gCompact) {
        NVU32 st = (tex == 2 ? 32u : tex ? 24u : 16u) << 8;   /* q3-17: s, t */
        for (i = 0; i < 16; i++)
            data(i == 0 ? (0x2 | (3u << 4) | st) :
                 i == 3 ? (0x4 | (4u << 4) | st) :
                 (i == 8 && tex) ? (0x2 | (2u << 4) | st) :
                 (i == 9 && tex == 2) ? (0x2 | (2u << 4) | st) : 0x2);
    } else {
        NVU32 st = (tex == 2 ? 48u : tex ? 32u : 16u) << 8;   /* q3-12: 2 textures = 12 words */
        for (i = 0; i < 16; i++)
            data(i == 0 ? (0x2 | (3u << 4) | st) :           /* V32_FLOAT × 3 */
                 i == 3 ? (0x4 | (4u << 4) | st) :           /* U8_UNORM × 4 */
                 (i == 8 && tex) ? (0x2 | (4u << 4) | st) :
                 (i == 9 && tex == 2) ? (0x2 | (4u << 4) | st) : 0x2);
    }
    hwFmt = key;
}

static NVU32 tex_filter(GLenum minFilter, GLenum magFilter, const HwTex *t)
{
    NVU32 mag = magFilter == GL_NEAREST ? 0x01000000 : 0x02000000, min;
    if (t->levels <= 1) {
        min = (minFilter == GL_NEAREST || minFilter == GL_NEAREST_MIPMAP_NEAREST ||
               minFilter == GL_NEAREST_MIPMAP_LINEAR) ? 0x00010000 : 0x00020000;
    } else {
        switch (minFilter) {
        case GL_NEAREST:                min = 0x00010000; break;
        case GL_LINEAR:                 min = 0x00020000; break;
        case GL_NEAREST_MIPMAP_NEAREST: min = 0x00030000; break;
        case GL_LINEAR_MIPMAP_NEAREST:  min = 0x00040000; break;
        case GL_NEAREST_MIPMAP_LINEAR:  min = 0x00050000; break;
        default:                        min = 0x00060000; break;
        }
    }
    return mag | min | 0x2000;
}

static NVU32 wrap_mode(GLenum w)
{
    return w == GL_REPEAT ? 1 : 3;                  /* REPEAT / CLAMP_TO_EDGE */
}

static int tex_resident(HwTex *t);

static void set_scissor(HwTarget *t, const HwState *st)
{
    static NVU32 lastWH;
    long key;
    int x = 0, y = 0, w = t->w, h = t->h;
    if (st->scissor) {
        x = st->sx; y = st->sy; w = st->sw; h = st->sh;
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > t->w) w = t->w - x;
        if (y + h > t->h) h = t->h - y;
        if (w < 0) w = 0;
        if (h < 0) h = 0;
    }
    /* max-9: exact key (previously a hash of st->sx..sh, could collide) */
    key = (long)(((NVU32)x << 16) | (NVU32)y);
    if (hwScis == key && lastWH == (((NVU32)w << 16) | (NVU32)h))
        return;
    begin(M_SCISSOR_HORIZ, 2);
    data(((NVU32)w << 16) | (NVU32)x);
    data(((NVU32)h << 16) | (NVU32)y);
    hwScis = key;
    lastWH = ((NVU32)w << 16) | (NVU32)h;
}

/* texture unit u (method block 0x1a00 + 0x20·u), state in cTex, cKey, cEpoch */
static void tex_unit(int u, HwTex *x, GLenum minF, GLenum magF, GLenum ws, GLenum wt, long *cTex, long *cKey,
                     long *cEpoch)
{
    NVU32 filt = tex_filter(minF, magF, x);
    NVU32 wrap = wrap_mode(ws) | (wrap_mode(wt) << 8) | (1u << 16);
    long key = (long)(filt ^ (wrap << 4));
    if (*cTex != (long)x || *cKey != key || *cEpoch != (long)gEpoch) {
        begin(M_TEX_OFFSET0 + 0x20 * u, 8);
        data(x->off);
        data(0x08 | 0x20 | ((NVU32)x->fmt << 8) | (1u << 16) | (x->levels > 1 ? 0x00080000 : 0) |
             ((NVU32)x->log2w << 20) | ((NVU32)x->log2h << 24) | 0x01);
        data(wrap);
        data(0x40000000 | ((((NVU32)x->levels - 1) << 8) << 6));
        data(0xAAE4);
        data(filt);
        data(((NVU32)x->w << 16) | (NVU32)x->h);
        data(0);
        *cTex = (long)x;
        *cKey = key;
        *cEpoch = gEpoch;
    }
    x->frameUsed = gFrame;
    x->lastUse = gFrame;
}

/* q3-16: transformation. Fixed-function NV34 multiplies the vertex by the PROJECTION matrix (like NV10/NV20
 * in nouveau: a composite matrix), MODELVIEW stays identity; the viewport (offset 0, scale 1) is
 * in the matrix. Without T&L an identity matrix (vertices in window coordinates, w = 1).
 * boot-args nv34gltnlt = transposed matrix, nv34gltnlclamp = clamp depth (no clipping by the
 * near/far plane), nv34gltnlcw = front face flipped (q3-16 default, wrong).
 * q3-17: in a T&L batch the texture coordinates are only s, t (the GPU fills in q = 1) → 6 words per vertex
 * (8 with two textures); boot-args nv34gltnl4 = 4 floats as without T&L. */
static int gTnlT = -1, gTnlClamp, gTnlCw, gTnl4;

static void mat_upload(NVU32 mthd, const float *m)
{
    int i;
    begin(mthd, 16);
    for (i = 0; i < 16; i++)
        dataf(m ? (gTnlT ? m[(i & 3) * 4 + (i >> 2)] : m[i]) : ((i % 5) == 0 ? 1.0f : 0.0f));
}

static void tnl_args(void)
{
    if (gTnlT < 0) {
        gTnlT = gl_boot_arg("nv34gltnlt");
        gTnlClamp = gl_boot_arg("nv34gltnlclamp");
        gTnlCw = gl_boot_arg("nv34gltnlcw");
        gTnl4 = gl_boot_arg("nv34gltnl4");
    }
}

static void tnl_state(const HwState *st)
{
    long key;
    tnl_args();
    if (hwTnl != st->tnl) {
        if (hwTnl < 0 || (!st->tnl && hwMv > 0)) {
            mat_upload(M_MODELVIEW0, 0);
            hwMv = 0;
        }
        begin(M_DEPTH_CONTROL, 1);                  /* 0x01 = clip by depth (GL), 0x10 = clamp */
        data(st->tnl && !gTnlClamp ? 0x01 : 0x10);
        if (!st->tnl) {
            mat_upload(M_PROJECTION0, 0);
            hwMatGen = -1;
        }
        hwTnl = st->tnl;
        gCompact = st->tnl && !gTnl4 && !gVtx12;
    }
    /* PROJECTION = the whole window × MVP matrix (position, like nouveau nv20_emit_projection); MODELVIEW only for
     * the fog distance in eye space, otherwise identity */
    if (st->tnl && (hwMatGen != (long)st->matGen || hwMv != (st->mv != 0))) {
        mat_upload(M_PROJECTION0, st->mat);
        if (st->mv || hwMv)
            mat_upload(M_MODELVIEW0, st->mv);
        hwMatGen = st->matGen;
        hwMv = st->mv != 0;
    }
    /* back faces: without T&L the CPU removes them. The GPU takes FRONT_FACE per GL (q3-16: with flipping
     * because of y pointing down everything disappeared) */
    key = st->tnl && st->cull ? (long)((st->cullFace << 16) | st->frontFace) : 0;
    if (hwCull != key) {
        if (key) {
            begin(M_CULL_FACE, 2);
            data(st->cullFace);
            data(!gTnlCw ? st->frontFace : st->frontFace == GL_CCW ? GL_CW : GL_CCW);
        }
        begin(M_CULL_FACE_ENABLE, 1);
        data(key != 0);
        hwCull = key;
    }
}

/* ---- max-4: fragment programs for glTexEnv (GL_COMBINE, BLEND, ADD, DECAL on 2 units) ----------
 * NV30 encoding per nouveau nvfx_shader.h (verified on kFpTex/kFpTex2). Program:
 *   TXP R1, a[tex0], t[0]; (TXP R2, a[tex1], t[1]);
 *   for unit u: R3.xyz = op(RGB arguments), R3.w = op(alpha arguments)  (previous = a[col0] / R3)
 *   MOVH H0, R3; END
 * Arguments with OPERAND 1−x and constants (TEXTURE_ENV_COLOR) go into R4–R6 first (one inline constant
 * and one input per instruction). The combiners see the result H0 as TEXTURE0, the final combiner takes it directly.
 * Programs in VRAM from fpOffset + FPG_BASE, a ring of 256 B slots (key = the environment state used).
 * max-6: the constant (TEXTURE_ENV_COLOR) is in the program → Max Payne changes the color ~8× per frame; a slot is
 * overwritten only after the frame in which it was last used has finished (REF_CNT), not with hw_sync (max-5). */
#define FPG_BASE    0x400
#define FPG_SLOT    0x100
#define FPG_N       ((0x10000 - FPG_BASE) / FPG_SLOT)
#define FPG_KEYW    48

#define FO_MOV  0x01
#define FO_MUL  0x02
#define FO_ADD  0x03
#define FO_DP3  0x05
#define FO_TXP  0x18
#define FO_LRP  0x1F

typedef struct FSrc { int type, reg, neg; NVU32 swz; float c[4]; } FSrc;   /* type 0 temp, 1 input, 2 const. */

static NVU32 gFpg[160];
static int gFpgN, gFpgMaxR, gFpgLast, gFpgOver;
static NVU32 gFpgKey[FPG_N][FPG_KEYW], gFpgCtl[FPG_N];
static NVU32 gFpgFrame[FPG_N], gFpgHash[FPG_N];
static int gFpgUsed, gFpgNext, gFpgLastSlot = -1;
NVU32 gFpgEvict, gFpgNew;

/* an input (a[col0], a[texN]) is given in word 0 of the instruction, the source only has the type and swizzle (kFpTex 0x1c9dc801) */
static NVU32 fsrc_word(const FSrc *x)
{
    return (NVU32)x->type | (x->type == 0 ? (NVU32)x->reg << 2 : 0) | (x->swz << 9) | (x->neg ? 1u << 17 : 0);
}

static void fpg_emit(int op, int dst, int half, int mask, int sat, int scale, int texu, const FSrc *s0,
                     const FSrc *s1, const FSrc *s2)
{
    const FSrc *ss[3] = { s0, s1, s2 }, *cs = 0;
    NVU32 w0, w1, w2, w3, in = 0;
    int i;
    if (gFpgN + 8 > (int)(sizeof(gFpg) / 4)) {
        gFpgOver = 1;
        return;
    }
    for (i = 0; i < 3; i++)
        if (ss[i]) {
            if (ss[i]->type == 1)
                in = (NVU32)ss[i]->reg;
            if (ss[i]->type == 2)
                cs = ss[i];
        }
    if (texu >= 0)
        in = 4 + (NVU32)texu;
    w0 = ((NVU32)op << 24) | (sat ? 0x80000000u : 0) | ((NVU32)(texu > 0 ? texu : 0) << 17) | (in << 13) |
         ((NVU32)mask << 9) | (half ? 0x80u : 0) | ((NVU32)dst << 1);
    w1 = fsrc_word(s0) | (7u << 18) | (0xE4u << 21);
    w2 = s1 ? fsrc_word(s1) : 0x0001c800;
    w2 |= (NVU32)scale << 28;
    w3 = s2 ? fsrc_word(s2) : 0x3fe1c800;
    gFpgLast = gFpgN;
    gFpg[gFpgN++] = w0;
    gFpg[gFpgN++] = w1;
    gFpg[gFpgN++] = w2;
    gFpg[gFpgN++] = w3;
    if (cs)
        for (i = 0; i < 4; i++) {
            union { float f; NVU32 u; } c;
            c.f = cs->c[i];
            gFpg[gFpgN++] = c.u;
        }
    if (!half && dst > gFpgMaxR)
        gFpgMaxR = dst;
}

static FSrc fs_temp(int r, NVU32 swz) { FSrc x; memset(&x, 0, sizeof(x)); x.reg = r; x.swz = swz; return x; }

static FSrc fs_const(float a, float b, float c, float d)
{
    FSrc x;
    memset(&x, 0, sizeof(x));
    x.type = 2;
    x.swz = 0xE4;
    x.c[0] = a; x.c[1] = b; x.c[2] = c; x.c[3] = d;
    return x;
}

/* argument i of unit u (alpha = 0 RGB, 1 alpha) → FSrc, possibly via R4+i */
static FSrc fpg_arg(const HwEnvUnit *e, int u, int units, int alpha, int i)
{
    GLenum src = e->src[alpha][i], op = e->op[alpha][i];
    int am = op == GL_SRC_ALPHA || op == 0x0303, inv = op == 0x0301 || op == 0x0303;
    FSrc x;
    memset(&x, 0, sizeof(x));
    x.swz = am ? 0xFF : 0xE4;
    if (src == 0x8576) {                            /* CONSTANT: value on the CPU */
        int k;
        float v[4];
        for (k = 0; k < 4; k++) {
            v[k] = am ? e->color[3] : e->color[k];
            if (inv)
                v[k] = 1.0f - v[k];
        }
        x = fs_const(v[0], v[1], v[2], v[3]);
        fpg_emit(FO_MOV, 4 + i, 0, 0xF, 0, 0, -1, &x, 0, 0);
        return fs_temp(4 + i, 0xE4);
    }
    if (src == 0x8577 || (src == 0x8578 && u == 0)) {   /* PRIMARY_COLOR, PREVIOUS on unit 0 */
        x.type = 1;
        x.reg = 1;                                  /* a[col0] */
    } else if (src == 0x8578) {
        x.reg = 3;
    } else if (src >= 0x84C0 && src < 0x84C0 + (GLenum)units) {
        x.reg = 1 + (int)(src - 0x84C0);            /* crossbar: TEXTUREn */
    } else {
        x.reg = 1 + u;                              /* GL_TEXTURE */
    }
    if (inv) {
        FSrc one = fs_const(1, 1, 1, 1);
        x.neg = 1;
        fpg_emit(FO_ADD, 4 + i, 0, 0xF, 0, 0, -1, &x, &one, 0);
        return fs_temp(4 + i, 0xE4);
    }
    return x;
}

static int fpg_scale(float s) { return s >= 3.5f ? 2 : s >= 1.5f ? 1 : 0; }

/* one part (RGB or alpha) of unit u into R3 with mask m */
static void fpg_part(const HwEnvUnit *e, int u, int units, int alpha, int m)
{
    GLenum comb = e->comb[alpha];
    int n = comb == GL_REPLACE ? 1 : comb == 0x8575 ? 3 : 2, i, sc = fpg_scale(e->scale[alpha]);
    FSrc a[3], half;
    for (i = 0; i < n; i++)
        a[i] = fpg_arg(e, u, units, alpha, i);
    switch (comb) {
    case GL_REPLACE:
        fpg_emit(FO_MOV, 3, 0, m, 1, sc, -1, &a[0], 0, 0);
        break;
    case 0x0104:                                    /* ADD */
        fpg_emit(FO_ADD, 3, 0, m, 1, sc, -1, &a[0], &a[1], 0);
        break;
    case 0x8574:                                    /* ADD_SIGNED */
        half = fs_const(-0.5f, -0.5f, -0.5f, -0.5f);
        fpg_emit(FO_ADD, 6, 0, m, 0, 0, -1, &a[0], &a[1], 0);
        a[2] = fs_temp(6, 0xE4);
        fpg_emit(FO_ADD, 3, 0, m, 1, sc, -1, &a[2], &half, 0);
        break;
    case 0x8575:                                    /* INTERPOLATE: a0·a2 + a1·(1−a2) */
        fpg_emit(FO_LRP, 3, 0, m, 1, sc, -1, &a[2], &a[0], &a[1]);
        break;
    case 0x84E7:                                    /* SUBTRACT */
        a[1].neg = !a[1].neg;
        fpg_emit(FO_ADD, 3, 0, m, 1, sc, -1, &a[0], &a[1], 0);
        break;
    case 0x86AE: case 0x86AF: {                     /* DOT3_RGB(A): 4·(a0−½)·(a1−½) */
        FSrc t0, t1;
        half = fs_const(-0.5f, -0.5f, -0.5f, -0.5f);
        fpg_emit(FO_ADD, 4, 0, 0xF, 0, 0, -1, &a[0], &half, 0);
        fpg_emit(FO_ADD, 5, 0, 0xF, 0, 0, -1, &a[1], &half, 0);
        t0 = fs_temp(4, 0xE4);
        t1 = fs_temp(5, 0xE4);
        fpg_emit(FO_DP3, 3, 0, comb == 0x86AF ? 0xF : m, 1, 2, -1, &t0, &t1, 0);
        break;
    }
    default:                                        /* MODULATE */
        fpg_emit(FO_MUL, 3, 0, m, 1, sc, -1, &a[0], &a[1], 0);
        break;
    }
}

/* fixed modes → COMBINE */
static void fpg_norm(const HwEnvUnit *in, HwEnvUnit *e)
{
    *e = *in;
    if (in->mode == 0x8570)
        return;
    e->scale[0] = e->scale[1] = 1.0f;
    e->src[0][0] = e->src[1][0] = GL_TEXTURE;
    e->src[0][1] = e->src[1][1] = 0x8578;
    e->op[0][0] = e->op[0][1] = GL_SRC_COLOR;
    e->op[1][0] = e->op[1][1] = GL_SRC_ALPHA;
    e->comb[0] = e->comb[1] = GL_MODULATE;
    switch (in->mode) {
    case GL_REPLACE:
        e->comb[0] = e->comb[1] = GL_REPLACE;
        break;
    case GL_DECAL:                                  /* RGB = lerp(previous, tex, tex.a), alpha = previous */
        e->comb[0] = 0x8575;
        e->src[0][2] = GL_TEXTURE;
        e->op[0][2] = GL_SRC_ALPHA;
        e->comb[1] = GL_REPLACE;
        e->src[1][0] = 0x8578;
        break;
    case GL_BLEND:                                  /* RGB = lerp(environment color, previous, tex) */
        e->comb[0] = 0x8575;
        e->src[0][0] = 0x8576;
        e->src[0][2] = GL_TEXTURE;
        e->op[0][2] = GL_SRC_COLOR;
        break;
    case 0x0104:                                    /* ADD */
        e->comb[0] = 0x0104;
        break;
    }
}

/* max-5: number of arguments of an operation */
static int fpg_nargs(GLenum comb)
{
    return comb == GL_REPLACE ? 1 : comb == 0x8575 ? 3 : 2;
}

/* max-5: the key only holds what the program actually uses (max-4 kept generating new programs because of changes
 * to an unused TEXTURE_ENV_COLOR → the 64 slots got overwritten during drawing, textures flickered) */
static void fpg_canon(HwEnvUnit *e)
{
    int p, i, usesConst = 0;
    e->mode = 0x8570;
    for (p = 0; p < 2; p++) {
        int n = fpg_nargs(e->comb[p]);
        if (e->comb[p] == 0x86AE || e->comb[p] == 0x86AF)
            e->scale[p] = 1.0f;
        for (i = n; i < 3; i++)
            e->src[p][i] = e->op[p][i] = 0;
        for (i = 0; i < n; i++)
            if (e->src[p][i] == 0x8576)
                usesConst = 1;
    }
    if (e->comb[0] == 0x86AF) {                     /* DOT3_RGBA: alpha is not computed */
        e->comb[1] = 0;
        for (i = 0; i < 3; i++)
            e->src[1][i] = e->op[1][i] = 0;
    }
    if (!usesConst)
        e->color[0] = e->color[1] = e->color[2] = e->color[3] = 0;
    for (i = 0; i < 4; i++)                         /* max-7: color to 1/255 (same program) */
        e->color[i] = (float)(e->color[i] <= 0 ? 0 : e->color[i] >= 1 ? 255 : (int)(e->color[i] * 255.0f + 0.5f));
    for (p = 0; p < 2; p++)                         /* max-7: scale only 1, 2, 4 (DST_SCALE) */
        e->scale[p] = (float)fpg_scale(e->scale[p]);
}

static void fpg_key(const HwEnvUnit *env, int units, NVU32 *k)
{
    int u, i;
    memset(k, 0, FPG_KEYW * 4);
    k[0] = (NVU32)units;
    for (u = 0; u < units; u++) {
        HwEnvUnit e;
        const NVU32 *w;
        fpg_norm(&env[u], &e);
        fpg_canon(&e);
        w = (const NVU32 *)&e;
        for (i = 0; i < (int)(sizeof(e) / 4) && 1 + u * 23 + i < FPG_KEYW; i++)
            k[1 + u * 23 + i] = w[i];
    }
}

/* program for environment env (units units) → offset in VRAM and FP_CONTROL */
static NVU32 fpg_get(const HwEnvUnit *env, int units, NVU32 *ctl)
{
    NVU32 key[FPG_KEYW];
    FSrc r3;
    int i, slot, u;
    NVU32 h = 0;
    fpg_key(env, units, key);
    for (i = 0; i < FPG_KEYW; i++)
        h = (h << 5) + (h >> 27) + key[i];
    if (gFpgLastSlot >= 0 && gFpgHash[gFpgLastSlot] == h && !memcmp_w(gFpgKey[gFpgLastSlot], key, FPG_KEYW)) {
        gFpgFrame[gFpgLastSlot] = gFrame;
        *ctl = gFpgCtl[gFpgLastSlot];
        return FPG_BASE + (NVU32)gFpgLastSlot * FPG_SLOT;
    }
    for (i = 0; i < gFpgUsed; i++)
        if (gFpgHash[i] == h && !memcmp_w(gFpgKey[i], key, FPG_KEYW)) {
            gFpgLastSlot = i;
            gFpgFrame[i] = gFrame;
            *ctl = gFpgCtl[i];
            return FPG_BASE + (NVU32)i * FPG_SLOT;
        }
    gFpgN = 0;
    gFpgMaxR = 0;
    gFpgOver = 0;
    for (u = 0; u < units; u++) {
        FSrc tc = fs_temp(0, 0xE4);
        tc.type = 1;
        tc.reg = 4 + u;
        fpg_emit(FO_TXP, 1 + u, 0, 0xF, 0, 0, u, &tc, 0, 0);
    }
    for (u = 0; u < units; u++) {
        HwEnvUnit e;
        fpg_norm(&env[u], &e);
        if (e.comb[0] == 0x86AF) {
            fpg_part(&e, u, units, 0, 0xF);
        } else {
            fpg_part(&e, u, units, 0, 0x7);
            fpg_part(&e, u, units, 1, 0x8);
        }
    }
    r3 = fs_temp(3, 0xE4);
    fpg_emit(FO_MOV, 0, 1, 0xF, 0, 0, -1, &r3, 0, 0);
    gFpg[gFpgLast] |= 1;                            /* END */
    if (gFpgOver || gFpgN * 4 > FPG_SLOT) {
        static int logged;
        if (!logged++)
            lg("texenv program: too long (%d words)", gFpgN);
        *ctl = 0;
        return 0;
    }
    gFpgNew++;
    if (gFpgNew <= 40 && gFpgLastSlot >= 0) {      /* max-7: how the new key differs from the last one */
        for (i = 0; i < FPG_KEYW && key[i] == gFpgKey[gFpgLastSlot][i]; i++)
            ;
        if (i < FPG_KEYW)
            lg("texenv new key: word %d (unit %d, field %d) %x -> %x", i, i ? (i - 1) / 23 : -1,
               i ? (i - 1) % 23 : -1, gFpgKey[gFpgLastSlot][i], key[i]);
    }
    if (gFpgUsed < FPG_N) {
        slot = gFpgUsed++;
    } else {
        slot = gFpgNext;
        gFpgNext = (gFpgNext + 1) % FPG_N;
        if (gFpgFrame[slot] >= gFrame || gRefOK != 1) {
            hw_sync();                              /* the ring wrapped within a frame */
            gFpgEvict++;
        } else if (gpu_done() < gFpgFrame[slot]) {  /* wait for the frame in which the slot was used */
            NVU32 t0 = TickCount();
            nva_kickoff(S->a);
            gFpgEvict++;
            while (gpu_done() < gFpgFrame[slot])
                if (TickCount() - t0 > 120) {
                    hw_sync();
                    break;
                }
        }
    }
    for (i = 0; i < FPG_KEYW; i++)
        gFpgKey[slot][i] = key[i];
    gFpgHash[slot] = h;
    gFpgFrame[slot] = gFrame;
    gFpgCtl[slot] = (NVU32)gFpgMaxR / 2;            /* (number of R − 1) / 2 */
    fp_upload(FPG_BASE + (NVU32)slot * FPG_SLOT, gFpg, gFpgN);
    hwProg = -1;                                    /* max-7: new contents even at the same address → set again */
    if (gFpgUsed <= 12 || (gFpgNew & 1023) == 0) {
        lg("texenv program %d: %d units, %d words, R0-R%d; u0 %x %x/%x from %x %x %x / %x %x %x", slot, units, gFpgN,
           gFpgMaxR, env[0].mode, env[0].comb[0], env[0].comb[1], env[0].src[0][0], env[0].src[0][1],
           env[0].src[0][2], env[0].op[0][0], env[0].op[0][1], env[0].op[0][2]);
        lg("  u0 alpha from %x %x %x / %x %x %x, color %f %f %f %f, scale %f %f", env[0].src[1][0], env[0].src[1][1],
           env[0].src[1][2], env[0].op[1][0], env[0].op[1][1], env[0].op[1][2], (double)env[0].color[0],
           (double)env[0].color[1], (double)env[0].color[2], (double)env[0].color[3], (double)env[0].scale[0],
           (double)env[0].scale[1]);
        if (units == 2)
            lg("  u1 %x %x/%x from %x %x %x / %x %x %x", env[1].mode, env[1].comb[0], env[1].comb[1], env[1].src[0][0],
               env[1].src[0][1], env[1].src[0][2], env[1].op[0][0], env[1].op[0][1], env[1].op[0][2]);
    }
    gFpgLastSlot = slot;
    *ctl = gFpgCtl[slot];
    return FPG_BASE + (NVU32)slot * FPG_SLOT;
}

/* returns 0 without a texture, 1 with a texture, 2 with two (the textures are in VRAM) */
int hw_state(HwTarget *t, const HwState *st)
{
    long key;
    int mode = st->tex && tex_resident(st->tex), fog, gen;

    /* q3-12: second unit (not with the original 12-word format nv34glvtx12) */
    vtx_init();
    if (mode && st->tex1 && tex_resident(st->tex1) && !gVtx12)
        mode = 2;
    /* texture */
    if (mode) {
        tex_unit(0, st->tex, st->minFilter, st->magFilter, st->wrapS, st->wrapT, &hwTex, &hwTexKey, &hwTexEpoch);
        if (mode == 2)
            tex_unit(1, st->tex1, st->minFilter1, st->magFilter1, st->wrapS1, st->wrapT1, &hwTex1, &hwTex1Key,
                     &hwTex1Epoch);
    }
    if (hwMode != mode) {
        if (hwMode == 2 || hwMode < 0) {            /* disable unit 1, back to the single-texture program */
            begin(M_TEX_OFFSET0 + 0x20 + 0x0c, 1);
            data(0);
            hwTex1 = -1;
        }
        if (!mode) {
            begin(M_TEX_UNITS_ENABLE, 1);
            data(0);
        } else {
            begin(M_TEX_UNITS_ENABLE, 1);
            data(mode == 2 ? 3 : 1);                /* interpolated coordinates tex0 (and tex1) */
            begin(M_RC_IN_ALPHA0, 6);               /* tex0 (with two, tex0 × tex1 from the program) × primary → spare0 */
            data((0x18u << 24) | (0x14u << 16));
            data((0x08u << 24) | (0x04u << 16));
            data(0);
            data(0);
            data(0x00000c00);
            data(0x00000c00);
        }
        hwMode = mode;
    }
    /* program: kFpTex / kFpTex2, max-4: generated for other environments (FP_CONTROL = register count) */
    gen = 0;
    if (mode) {
        NVU32 prog = mode == 2 ? FP2_OFF : 0, ctl = 0;
        if (st->env) {
            NVU32 g = fpg_get(st->env, mode, &ctl);
            if (g) {
                prog = g;
                gen = 1;
            }
        }
        if (hwProg != (long)prog) {
            begin(M_FP_ACTIVE_PROGRAM, 1);
            data((S->g->fpOffset + prog) | 1);
            begin(M_FP_CONTROL, 1);
            data(ctl);
            hwProg = prog;
        }
    }
    /* final combiner: color = D (primary / spare0); with fog A·B + (1−A)·C, A = FOG.a (fog factor),
     * B = color, C = FOG.rgb (fog color) as in nouveau nv10_state_frag.c */
    /* wolf-8: only with T&L — source 6 (coordinate from the vertex, NV20) was rejected by NV30 (DATA_ERROR at 0x8c8) */
    fog = st->fog && st->tnl && !gNoFog && !gVtx12;
    key = (mode != 0) | (fog << 1) | (gen << 2);
    if (hwFin != key) {
        NVU32 col = gen ? 0x08 : mode ? 0x0c : 0x04;   /* max-4: the program's result (TEXTURE0) directly */
        begin(M_RC_FINAL0, 3);
        data(fog ? (0x13u << 24) | (col << 16) | (0x03u << 8) : col);
        data((col | 0x10) << 8);
        data(0x01000101);
        hwFin = key;
    }
    if (fog) {
        /* source: radial distance (RADIAL, SIGNED modes). wolf-9: ORTHOGONAL_ABS with UNSIGNED (NV20,
         * Mesa's default) gave no fog on NV30 → only boot-args nv34glfogplane (experiment) */
        int uns = gFogPlane;
        NVU32 k[6];
        int i;
        union { float f; NVU32 u; } a, b;
        a.f = st->fogK[0];
        b.f = st->fogK[1];
        k[0] = gFogPlane ? 3 : 1;
        k[1] = st->fogMode == GL_LINEAR ? (uns ? 0x0804 : 0x2601) :
               st->fogMode == GL_EXP2 ? (uns ? 0x0803 : 0x0801) : (uns ? 0x0802 : 0x0800);
        k[2] = a.u;
        k[3] = b.u;
        k[4] = 0;
        k[5] = st->fogColor;
        for (i = 0; i < 6 && k[i] == hwFogK[i]; i++)
            ;
        if (hwFogOn != 1 || i < 6) {
            static int logged;
            if (!logged) {
                logged = 1;
                lg("fog: source %u, mode %x, k %f %f/1e6, color %x (T&L %d)", k[0], k[1], (double)a.f, (double)b.f * 1e6,
                   k[5], st->tnl);
            }
            begin(M_FOG_COORD_DIST, 5);
            data(k[0]);
            data(k[1]);
            data(k[2]);
            data(k[3]);
            data(k[4]);
            if (hwFogOn != 1 || k[5] != hwFogK[5]) {
                begin(M_FOG_ENABLE, 2);
                data(1);
                data(k[5]);
            }
            memcpy(hwFogK, k, sizeof(k));
            hwFogOn = 1;
        }
    } else if (hwFogOn != 0) {
        begin(M_FOG_ENABLE, 1);
        data(0);
        hwFogOn = 0;
    }
    /* blending */
    key = st->blend ? (long)((st->blendSrc << 16) | st->blendDst) : 0;
    if (hwBlend != key) {
        if (!st->blend) {
            begin(M_BLEND_FUNC_ENABLE, 1);
            data(0);
        } else {
            begin(M_BLEND_FUNC_ENABLE, 5);
            data(1);
            data((st->blendSrc << 16) | st->blendSrc);
            data((st->blendDst << 16) | st->blendDst);
            data(0);
            data(0x80068006);
        }
        hwBlend = key;
    }
    /* alpha test */
    {
        NVU32 ref = st->alphaRef <= 0 ? 0 : st->alphaRef >= 1 ? 255 : (NVU32)(st->alphaRef * 255.0f + 0.5f);
        key = st->alphaTest ? (long)((st->alphaFunc << 8) | ref) : 0;
        if (hwAlpha != key) {
            begin(M_ALPHA_FUNC_ENABLE, 3);
            data(st->alphaTest != 0);
            data(st->alphaFunc);
            data(ref);
            hwAlpha = key;
        }
    }
    /* depth: without a test no write either (GL) */
    key = st->depthTest ? (long)((st->depthFunc << 2) | (st->depthWrite ? 2 : 0) | 1) : 0;
    if (hwDepth != key) {
        begin(M_DEPTH_FUNC, 1);
        data(st->depthTest ? st->depthFunc : GL_ALWAYS);
        begin(M_DEPTH_WRITE, 2);
        data(st->depthTest && st->depthWrite);
        data(st->depthTest != 0);
        hwDepth = key;
    }
    if (hwMask != (long)st->colorMask) {
        begin(M_COLOR_MASK, 1);
        data(st->colorMask);
        hwMask = st->colorMask;
    }
    tnl_state(st);
    set_scissor(t, st);
    return mode;
}

void hw_clear(HwTarget *t, int color, int depth, NVU32 argb, NVU32 zval, const HwState *st)
{
    NVU32 bits = 0;
    if (!hw_bind(t))
        return;
    set_scissor(t, st);
    if (color && st->colorMask) {
        if (t->bpp == 16) {
            NVU32 v = (((argb >> 19) & 31) << 10) | (((argb >> 11) & 31) << 5) | ((argb >> 3) & 31);
            argb = (v << 16) | v;
        }
        begin(M_CLEAR_COLOR_VALUE, 1);
        data(argb);
        bits |= 0xF0;
    }
    if (depth) {
        begin(M_DEPTH_WRITE, 1);
        data(1);
        begin(M_CLEAR_DEPTH_VALUE, 1);
        data(zval);
        bits |= 0x03;
        hwDepth = -1;
    }
    if (bits) {
        begin(M_CLEAR_BUFFERS, 1);
        data(bits);
    }
}

/* ---- vertices ------------------------------------------------------------- */

static inline NVU32 vcolor(const HwVtx *v)
{
    NVU32 c = v->c;
    return gColorBE ? c : (c >> 24) | ((c >> 8) & 0xff00) | ((c << 8) & 0xff0000) | (c << 24);
}

void hw_draw(int prim, const HwVtx *v, int n, int textured)
{
    NVU32 wpv, left = n, cnt, i;
    if (n <= 0)
        return;
    vtx_init();
    wpv = gVtx12 ? (textured ? 12 : 8) : gCompact ? (textured == 2 ? 8 : textured ? 6 : 4) :
          (textured == 2 ? 12 : textured ? 8 : 4);
    set_vtxfmt(textured);
    begin(M_VERTEX_BEGIN_END, 1);
    data(prim);
    while (left) {
        volatile float *p;
        NVU32 t0 = tb();
        cnt = 2047 / wpv;
        if (cnt > left)
            cnt = left;
        if (gHwDead)
            return;
        if (nva_begin(S->a, SUBC, 0x40000000 | M_VERTEX_DATA, cnt * wpv) != kNVAOK) {
            hw_fail("vertices");
            return;
        }
        gTWait += tb() - t0;                        /* waiting for room in the push buffer */
        /* opt-1: the space is reserved → write directly (no nva_push call per word) */
        p = (volatile float *)(S->a->dmaBase + S->a->dmaCurrent);
        if (gVtx12) {
            for (i = 0; i < cnt; i++, v++) {
                p[0] = v->x; p[1] = v->y; p[2] = v->z; p[3] = 1.0f;
                p[4] = (v->c >> 24) * (1.0f / 255); p[5] = ((v->c >> 16) & 255) * (1.0f / 255);
                p[6] = ((v->c >> 8) & 255) * (1.0f / 255); p[7] = (v->c & 255) * (1.0f / 255);
                if (textured) {
                    p[8] = v->sq; p[9] = v->tq; p[10] = 0.0f; p[11] = v->q;
                    p += 12;
                } else {
                    p += 8;
                }
            }
        } else if (gCompact && textured) {
            for (i = 0; i < cnt; i++, v++) {
                p[0] = v->x; p[1] = v->y; p[2] = v->z;
                ((volatile NVU32 *)p)[3] = vcolor(v);
                p[4] = v->sq; p[5] = v->tq;
                if (textured == 2) {
                    p[6] = v->sq1; p[7] = v->tq1;
                    p += 8;
                } else {
                    p += 6;
                }
            }
        } else if (textured == 2) {
            for (i = 0; i < cnt; i++, v++, p += 12) {
                p[0] = v->x; p[1] = v->y; p[2] = v->z;
                ((volatile NVU32 *)p)[3] = vcolor(v);
                p[4] = v->sq; p[5] = v->tq; p[6] = 0.0f; p[7] = v->q;
                p[8] = v->sq1; p[9] = v->tq1; p[10] = 0.0f; p[11] = v->q1;
            }
        } else if (textured) {
            for (i = 0; i < cnt; i++, v++, p += 8) {
                p[0] = v->x; p[1] = v->y; p[2] = v->z;
                ((volatile NVU32 *)p)[3] = vcolor(v);
                p[4] = v->sq; p[5] = v->tq; p[6] = 0.0f; p[7] = v->q;
            }
        } else {
            for (i = 0; i < cnt; i++, v++, p += 4) {
                p[0] = v->x; p[1] = v->y; p[2] = v->z;
                ((volatile NVU32 *)p)[3] = vcolor(v);
            }
        }
        S->a->dmaCurrent += cnt * wpv;
        left -= cnt;
    }
    begin(M_VERTEX_BEGIN_END, 1);
    data(0);
}

/* ---- q3-18: vertices in VRAM + indices ------------------------------------------
 * A 2 MB ring buffer in 16 segments; a segment carries the number of the frame that last used it, and it is
 * written again only once the GPU has finished it (REF_CNT = frame). The segment currently being written
 * is continued without checking; should the ring wrap within a single frame, we wait for the whole GPU.
 * A vertex has the same format as the shorter inline T&L vertex (q3-17). Indices go into the push buffer
 * (VB_ELEMENT_U16, the first index in the lower half of the word as in nouveau).
 * q3-19: the GPU rejected the first VB_ELEMENT_U16 (DATA_ERROR, INVALID_STATE) → q3-20 only with boot-args
 * nv34glvb; nv34glvbbatch = no indices (vertices in triangle order, VB_VERTEX_BATCH).
 * boot-args nv34glvbswap = vertex in LE byte order, nv34glidxhi = first
 * index in the upper half of the word, nv34glvbinv = VTX_CACHE_INV (0x1710) before drawing. */
#define VB_SIZE     (2u << 20)
#define VB_SEGS     16
#define VB_SEG      (VB_SIZE / VB_SEGS)
static NVU32 gVbOff, gVbPos, gVbStart, gVbGen, gVbSegFrame[VB_SEGS];
static int gVbSwap, gIdxHi, gVbInv, gVbBatch, gVbCurSeg = -1;
NVU32 gHwVbWaits;

static NVU32 bswap(NVU32 v)
{
    return (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24);
}

int hw_vb_batch(void) { return gVbBatch; }

int hw_vb_ready(void)
{
    if (gVbOn < 0) {
        gVbOn = gl_boot_arg("nv34glvb");             /* q3-20: the GPU rejected VB_ELEMENT in q3-18/19 (experiment) */
        gVbBatch = gl_boot_arg("nv34glvbbatch");
        gVbSwap = gl_boot_arg("nv34glvbswap");
        gIdxHi = gl_boot_arg("nv34glidxhi");
        gVbInv = gl_boot_arg("nv34glvbinv");
    }
    vtx_init();
    tnl_args();
    if (!gVbOn || gRefOK != 1 || gTnl4 || gVtx12 || gHwDead)   /* shorter T&L format only */
        return 0;
    if (gVbOff && gVbGen != S->generation)          /* 3D reinitialized: forget the buffer */
        gVbOff = 0;
    if (!gVbOff) {
        gVbOff = S->vramAlloc(VB_SIZE, 256);
        if (!gVbOff) {
            lg("vertices in VRAM: no room (%u KB)", VB_SIZE >> 10);
            gVbOn = 0;
            return 0;
        }
        memset(gVbSegFrame, 0, sizeof(gVbSegFrame));
        gVbPos = 0;
        gVbCurSeg = -1;
        gVbGen = S->generation;
        if (first_time(0x72000000))
            lg("vertices in VRAM: %u KB at %x", VB_SIZE >> 10, gVbOff);
    }
    return 1;
}

void hw_vb_free(void)
{
    if (gVbOff && S) {
        hw_sync();
        S->vramFree(gVbOff);
    }
    gVbOff = 0;
}

volatile NVU32 *hw_vb_begin(NVU32 bytes)
{
    NVU32 sg, last;
    if (bytes > VB_SIZE / 2)
        return 0;
    if (gVbPos + bytes > VB_SIZE) {
        gVbPos = 0;
        gVbCurSeg = -1;
    }
    last = (gVbPos + bytes - 1) / VB_SEG;
    for (sg = gVbPos / VB_SEG; sg <= last; sg++) {
        NVU32 f = gVbSegFrame[sg];
        if ((int)sg == gVbCurSeg)
            continue;
        if (f && f > gpu_done()) {
            gHwVbWaits++;
            if (f >= gFrame) {
                hw_sync();                          /* the ring wrapped within one frame */
            } else {
                NVU32 t0 = TickCount(), w0 = tb();
                nva_kickoff(S->a);
                while (gpu_done() < f)
                    if (TickCount() - t0 > 120) {
                        hw_sync();
                        break;
                    }
                gTWait += tb() - w0;
            }
        }
        gVbSegFrame[sg] = gFrame;
    }
    gVbCurSeg = (int)last;
    gVbStart = gVbPos;
    return (volatile NVU32 *)(S->g->a->vram + gVbOff + gVbPos);
}

NVU32 hw_vb_end(NVU32 used)
{
    gVbPos = gVbStart + ((used + 63) & ~63u);
    return gVbOff + gVbStart;
}

/* one vertex into VRAM: x, y, z, color, s, t (s1, t1) */
void hw_vb_put(volatile NVU32 *d, const HwVtx *v, int textured)
{
    union { float f; NVU32 u; } a[7];
    int i, n;
    a[0].f = v->x; a[1].f = v->y; a[2].f = v->z;
    a[3].f = v->sq; a[4].f = v->tq; a[5].f = v->sq1; a[6].f = v->tq1;
    n = textured == 2 ? 4 : textured ? 2 : 0;
    if (gVbSwap) {
        d[0] = bswap(a[0].u); d[1] = bswap(a[1].u); d[2] = bswap(a[2].u);
        d[3] = v->c;
        for (i = 0; i < n; i++)
            d[4 + i] = bswap(a[3 + i].u);
    } else {
        d[0] = a[0].u; d[1] = a[1].u; d[2] = a[2].u;
        d[3] = vcolor(v);
        for (i = 0; i < n; i++)
            d[4 + i] = a[3 + i].u;
    }
}

void hw_draw_indexed(int prim, NVU32 vbOff, int textured, const NVU16 *idx, int n)
{
    if (n <= 0 || gHwDead)
        return;
    set_vtxfmt(textured);
    begin(M_VTXBUF0, 1);
    data(vbOff);
    begin(M_VTXBUF0 + 3 * 4, 1);
    data(vbOff + 12);
    if (textured) {
        begin(M_VTXBUF0 + 8 * 4, textured == 2 ? 2 : 1);
        data(vbOff + 16);
        if (textured == 2)
            data(vbOff + 24);
    }
    if (gVbInv) {                                   /* q3-19: the GPU stalled in q3-18 — maybe NV40 only */
        begin(M_VTX_CACHE_INV, 1);
        data(0);
    }
    if (!gVbUsed)
        lg("first draw from VRAM: vertices at %x, textures %d, %s %d", vbOff, textured, idx ? "indices" : "vertices",
           n);
    gVbUsed = 1;
    begin(M_VERTEX_BEGIN_END, 1);
    data(prim);
    if (!idx) {                                     /* VB_VERTEX_BATCH: (count − 1) << 24 | start */
        NVU32 start = 0;
        while (n) {
            NVU32 c = n > 256 ? 256 : n;
            begin(M_VB_VERTEX_BATCH, 1);
            data(((c - 1) << 24) | start);
            start += c;
            n -= c;
        }
        begin(M_VERTEX_BEGIN_END, 1);
        data(0);
        return;
    }
    if (n & 1) {
        begin(M_VB_ELEMENT_U32, 1);
        data(*idx++);
        n--;
    }
    while (n) {
        volatile NVU32 *p;
        NVU32 cnt = n / 2, i;
        if (cnt > 2047)
            cnt = 2047;
        if (nva_begin(S->a, SUBC, 0x40000000 | M_VB_ELEMENT_U16, cnt) != kNVAOK) {
            hw_fail("indices");
            return;
        }
        p = S->a->dmaBase + S->a->dmaCurrent;
        if (gIdxHi)
            for (i = 0; i < cnt; i++, idx += 2)
                p[i] = ((NVU32)idx[0] << 16) | idx[1];
        else
            for (i = 0; i < cnt; i++, idx += 2)
                p[i] = ((NVU32)idx[1] << 16) | idx[0];
        S->a->dmaCurrent += cnt;
        n -= cnt * 2;
    }
    begin(M_VERTEX_BEGIN_END, 1);
    data(0);
}

/* q3-20: inline T&L vertices straight from the prepared table by index (no copy into a batch);
 * shorter format only (q3-17). 0 = not possible. */
int hw_gather_ok(void)
{
    vtx_init();
    tnl_args();
    return !gTnl4 && !gVtx12 && gCompact;
}

void hw_draw_gather(int prim, const HwVtx *base, const NVU16 *idx, int n, int textured)
{
    NVU32 wpv = textured == 2 ? 8 : textured ? 6 : 4, left = n, cnt, i;
    if (n <= 0)
        return;
    set_vtxfmt(textured);
    begin(M_VERTEX_BEGIN_END, 1);
    data(prim);
    while (left) {
        volatile float *p;
        NVU32 t0 = tb();
        cnt = 2047 / wpv;
        if (cnt > left)
            cnt = left;
        if (gHwDead)
            return;
        if (nva_begin(S->a, SUBC, 0x40000000 | M_VERTEX_DATA, cnt * wpv) != kNVAOK) {
            hw_fail("vertices");
            return;
        }
        gTWait += tb() - t0;
        p = (volatile float *)(S->a->dmaBase + S->a->dmaCurrent);
        for (i = 0; i < cnt; i++) {
            const HwVtx *v = &base[*idx++];
            p[0] = v->x; p[1] = v->y; p[2] = v->z;
            ((volatile NVU32 *)p)[3] = vcolor(v);
            if (textured) {
                p[4] = v->sq; p[5] = v->tq;
                if (textured == 2) {
                    p[6] = v->sq1; p[7] = v->tq1;
                }
            }
            p += wpv;
        }
        S->a->dmaCurrent += cnt * wpv;
        left -= cnt;
    }
    begin(M_VERTEX_BEGIN_END, 1);
    data(0);
}

/* ---- textures ------------------------------------------------------------- */

static NVU32 gSwX[2048], gSwY[2048];

static NVU32 swz(NVU32 x, NVU32 y, NVU32 lw, NVU32 lh)
{
    NVU32 o = 0, bit = 0, i;
    for (i = 0; i < lw || i < lh; i++) {
        if (i < lw)
            o |= ((x >> i) & 1) << bit++;
        if (i < lh)
            o |= ((y >> i) & 1) << bit++;
    }
    return o;
}

static int log2i(int v)
{
    int n = 0;
    while ((1 << n) < v)
        n++;
    return n;
}

HwTex *hw_tex_new(void)
{
    HwTex *t = (HwTex *)NewPtrClear(sizeof(HwTex));
    if (!t)
        t = (HwTex *)NewPtrSysClear(sizeof(HwTex));     /* q3-7: Quake III fills up the application heap */
    if (!t) {
        if (first_time(0x71100000))
            lg("hw_tex_new: out of memory");
        return 0;
    }
    t->magic = 'NVgt';
    t->next = gTexList;
    if (gTexList)
        gTexList->prev = t;
    gTexList = t;
    return t;
}

/* opt-5: VRAM of a texture the GPU may still be reading is released only after its last frame (fence) —
 * previously every glDeleteTextures waited for the GPU */
#define PEND_MAX 512
static struct { NVU32 off, frame; } gPend[PEND_MAX];
static int gPendN;

static void pend_process(int force)
{
    NVU32 done;
    int i, j = 0;
    if (!gPendN || !S)
        return;
    done = gpu_done();
    for (i = 0; i < gPendN; i++) {
        if (force || gPend[i].frame <= done)
            S->vramFree(gPend[i].off);
        else
            gPend[j++] = gPend[i];
    }
    gPendN = j;
}

static void vram_release(NVU32 off, NVU32 frame)
{
    if (!off)
        return;
    if (gRefOK == 1 && frame > gpu_done()) {
        if (gPendN >= PEND_MAX) {
            hw_sync();
            pend_process(1);
        }
        gPend[gPendN].off = off;
        gPend[gPendN].frame = frame;
        gPendN++;
        return;
    }
    if (gRefOK != 1 && frame >= gFrame - 1)
        hw_sync();                                  /* no fence, as before */
    S->vramFree(off);
}

/* on exit / context destruction: return everything deferred to the arena */
void hw_pend_flush(void)
{
    if (!S || !gPendN)
        return;
    hw_sync();
    pend_process(1);
}

/* glFinish (opt-5): wait only for the previous frame, not for all GPU work */
NVU32 gTFinish, gFinishN;
static int gNoFinish = -1;

void hw_finish(void)
{
    NVU32 t0, tb0 = tb();
    if (!S || gHwDead)
        return;
    gFinishN++;
    if (gNoFinish < 0)
        gNoFinish = gl_boot_arg("nv34glnofinish");
    if (gNoFinish) {                                /* q3-9: test — just submit, do not wait */
        nva_kickoff(S->a);
        return;
    }
    if (gRefOK != 1) {
        hw_sync();
        return;
    }
    nva_kickoff(S->a);
    t0 = TickCount();
    while (gpu_done() + 1 < gFrame)
        if (TickCount() - t0 > 120) {
            hw_sync();
            break;
        }
    gTFinish += tb() - tb0;
}

void hw_tex_free(HwTex *t)
{
    if (!t || t->magic != 'NVgt')
        return;
    if (S) {
        vram_release(t->off, t->frameUsed);
        vram_release(t->alt, t->altUsed);
        t->off = t->alt = 0;
    }
    if (t->prev) t->prev->next = t->next;
    else if (gTexList == t) gTexList = t->next;
    if (t->next) t->next->prev = t->prev;
    t->magic = 0;
    if ((NVU32)hwTex == (NVU32)t)
        hwTex = -1;
    if ((NVU32)hwTex1 == (NVU32)t)
        hwTex1 = -1;
    DisposePtr((Ptr)t);
}

/* VRAM for a texture; when short, evicts the least recently used ones (the data is in RAM in the GL layer) */
static NVU32 alloc_evict_(HwTex *t, NVU32 size);

static NVU32 alloc_evict(HwTex *t, NVU32 size)
{
    NVU32 t0 = tb(), r = alloc_evict_(t, size);
    gTEvict += tb() - t0;
    return r;
}

static NVU32 alloc_evict_(HwTex *t, NVU32 size)
{
    NVU32 off = S->vramAlloc(size, 256);
    if (!off && gPendN) {
        pend_process(0);
        off = S->vramAlloc(size, 256);
    }
    if (!off) {
        HwTex *v, *lru;
        hw_sync();
        pend_process(1);
        while (!off) {
            lru = 0;
            for (v = gTexList; v; v = v->next)
                if (v != t && v != gTexPin && (v->off || v->alt) && (!lru || v->lastUse < lru->lastUse))
                    lru = v;
            if (!lru)
                break;
            if (lru->off) S->vramFree(lru->off);
            if (lru->alt) S->vramFree(lru->alt);
            lru->off = lru->alt = 0;
            lru->levels = 0;                        /* the GL layer uploads it again */
            off = S->vramAlloc(size, 256);
        }
        gEpoch++;
    }
    return off;
}

static int tex_resident(HwTex *t)
{
    return t->off && t->levels > 0;
}

int hw_tex_resident(HwTex *t)
{
    return t && tex_resident(t);
}

/* fmt: 0x06 A8R8G8B8, 0x02 A1R5G5B5, 0x04 A4R4G4B4 */
int hw_tex_upload(HwTex *t, int w, int h, int levels, void *const *pix, int fmt, int srcfmt)
{
    NVU32 size = 0, lw = log2i(w), lh = log2i(h), x, y, bpp = fmt == 0x06 ? 4 : 2, t0;
    int lv, cw = w, ch = h;
    volatile NVU8 *dst;

    if (!S || w > 2048 || h > 2048)
        return 0;
    for (lv = 0; lv < levels; lv++) {
        size += (NVU32)cw * ch * bpp;
        if (cw > 1) cw >>= 1;
        if (ch > 1) ch >>= 1;
    }
    size = (size + 63) & ~63UL;
    if (t->size != size) {                          /* different size: new areas */
        vram_release(t->off, t->frameUsed);
        vram_release(t->alt, t->altUsed);
        t->off = t->alt = 0;
        t->size = size;
    }
    if (t->off && t->frameUsed > gpu_done()) {
        /* the GPU may still be reading it in this frame → write into the second area (rave-22);
         * if the second one was also read in this frame (Quake 2 dynamic lightmap), wait for the GPU */
        NVU32 tmp;
        if (!t->alt)
            t->alt = alloc_evict(t, size);
        if (!t->alt || t->altUsed > gpu_done()) {
            hw_sync();
        } else {
            tmp = t->off;
            t->off = t->alt;
            t->alt = tmp;
            tmp = t->frameUsed;                     /* the last-read frames swap places too */
            t->frameUsed = t->altUsed;
            t->altUsed = tmp;
        }
    }
    if (!t->off)
        t->off = alloc_evict(t, size);
    if (!t->off) {
        t->levels = 0;
        return 0;
    }
    t->w = w;
    t->h = h;
    t->log2w = lw;
    t->log2h = lh;
    t->levels = levels;
    t->fmt = fmt;
    dst = S->a->vram + t->off;
    cw = w;
    ch = h;
    t0 = tb();
    for (lv = 0; lv < levels; lv++) {
        const NVU32 *src = (const NVU32 *)pix[lv];
        for (x = 0; x < (NVU32)cw; x++)
            gSwX[x] = swz(x, 0, lw, lh);
        for (y = 0; y < (NVU32)ch; y++)
            gSwY[y] = swz(0, y, lw, lh);
        if (bpp == 4) {
            volatile NVU32 *d = (volatile NVU32 *)dst;
            for (y = 0; y < (NVU32)ch; y++, src += cw) {
                NVU32 oy = gSwY[y];
                for (x = 0; x < (NVU32)cw; x++)
                    d[oy | gSwX[x]] = src[x];
            }
        } else if (srcfmt == fmt) {                 /* wolf-2: the RAM copy is already 16-bit */
            volatile NVU16 *d = (volatile NVU16 *)dst;
            const NVU16 *s16 = (const NVU16 *)pix[lv];
            for (y = 0; y < (NVU32)ch; y++, s16 += cw) {
                NVU32 oy = gSwY[y];
                for (x = 0; x < (NVU32)cw; x++)
                    d[oy | gSwX[x]] = s16[x];
            }
        } else {
            volatile NVU16 *d = (volatile NVU16 *)dst;
            for (y = 0; y < (NVU32)ch; y++, src += cw) {
                NVU32 oy = gSwY[y];
                for (x = 0; x < (NVU32)cw; x++)
                    d[oy | gSwX[x]] = tx16(src[x], fmt);
            }
        }
        dst += (NVU32)cw * ch * bpp;
        if (cw > 1) { cw >>= 1; lw--; }
        if (ch > 1) { ch >>= 1; lh--; }
    }
    __asm__ volatile("sync" ::: "memory");
    gTUpl += tb() - t0;
    gFrUplN++;
    gFrUplB += size;
    gEpoch++;
    gHwUploads++;
    gHwUploadBytes += size;
    return 1;
}

/* opt-1: only the level 0 rectangle, directly in place (Quake 2 lightmaps uploaded the whole 64 KB after every
 * glTexSubImage2D). Only if the GPU has not read it in this frame yet; otherwise 0 → a full upload. */
int hw_tex_subupload(HwTex *t, int x0, int y0, int w, int h, const void *srcv, int stride, int srcfmt)
{
    int x, y;

    if (!S || !tex_resident(t) || t->levels != 1 || t->frameUsed > gpu_done() ||
        x0 < 0 || y0 < 0 || x0 + w > t->w || y0 + h > t->h || (srcfmt != 0x06 && srcfmt != t->fmt))
        return 0;
    for (x = 0; x < w; x++)
        gSwX[x] = swz(x0 + x, 0, t->log2w, t->log2h);
    if (t->fmt == 0x06) {
        volatile NVU32 *dst = (volatile NVU32 *)(S->a->vram + t->off);
        const NVU32 *src = (const NVU32 *)srcv;
        for (y = 0; y < h; y++, src += stride) {
            NVU32 oy = swz(0, y0 + y, t->log2w, t->log2h);
            for (x = 0; x < w; x++)
                dst[oy | gSwX[x]] = src[x];
        }
    } else if (srcfmt == t->fmt) {
        volatile NVU16 *dst = (volatile NVU16 *)(S->a->vram + t->off);
        const NVU16 *src = (const NVU16 *)srcv;
        for (y = 0; y < h; y++, src += stride) {
            NVU32 oy = swz(0, y0 + y, t->log2w, t->log2h);
            for (x = 0; x < w; x++)
                dst[oy | gSwX[x]] = src[x];
        }
    } else {
        volatile NVU16 *dst = (volatile NVU16 *)(S->a->vram + t->off);
        const NVU32 *src = (const NVU32 *)srcv;
        for (y = 0; y < h; y++, src += stride) {
            NVU32 oy = swz(0, y0 + y, t->log2w, t->log2h);
            for (x = 0; x < w; x++)
                dst[oy | gSwX[x]] = tx16(src[x], t->fmt);
        }
    }
    __asm__ volatile("sync" ::: "memory");
    gEpoch++;                                       /* TEX_OFFSET again (flushes the texture cache) */
    gHwSubUploads++;
    gHwUploadBytes += (NVU32)w * h * (t->fmt == 0x06 ? 4 : 2);
    return 1;
}

/* ---- transfer to the screen ---------------------------------------------------- */

#define MAX_XS 128

static int toggle_x(short *xs, int n, short x)
{
    int i, j;
    for (i = 0; i < n && xs[i] < x; i++)
        ;
    if (i < n && xs[i] == x) {
        for (j = i; j < n - 1; j++)
            xs[j] = xs[j + 1];
        return n - 1;
    }
    if (n >= MAX_XS)
        return n;
    for (j = n; j > i; j--)
        xs[j] = xs[j - 1];
    xs[i] = x;
    return n + 1;
}

static void blit(HwTarget *t, PixMapPtr pm, const Rect *dst, long l, long tp, long r, long b)
{
    if (l < dst->left) l = dst->left;
    if (tp < dst->top) tp = dst->top;
    if (r > dst->right) r = dst->right;
    if (b > dst->bottom) b = dst->bottom;
    /* hl-2: clip to the screen bounds too (after a resolution change the window may stick out) */
    if (l < pm->bounds.left) l = pm->bounds.left;
    if (tp < pm->bounds.top) tp = pm->bounds.top;
    if (r > pm->bounds.right) r = pm->bounds.right;
    if (b > pm->bounds.bottom) b = pm->bounds.bottom;
    if (l >= r || tp >= b)
        return;
    nva_copy_from(S->a, t->colorOff, t->colorPitch, (int)(l - dst->left), (int)(tp - dst->top),
                  (int)(l - pm->bounds.left), (int)(tp - pm->bounds.top), (int)(r - l), (int)(b - tp));
}

/* fps-1: laggy mouse above 60 fps — the CPU must not get more than one frame ahead of the GPU
 * (otherwise frames pile up in the push buffer and the picture lags behind input);
 * nv34glqueue = no limit (as before) */
static int gQueue = -1;

static void present_throttle(void)
{
    NVU32 t0;
    if (gQueue < 0)
        gQueue = gl_boot_arg("nv34glqueue");
    if (gQueue || gRefOK != 1)
        return;
    t0 = TickCount();
    while (gpu_done() + 2 < gFrame)                 /* at least the previous frame must be finished */
        if (TickCount() - t0 > 120) {
            hw_sync();
            break;
        }
}

void hw_present(HwTarget *t, GDHandle gd, const Rect *dst, RgnHandle clip)
{
    PixMapPtr pm = *(*gd)->gdPMap;

    gFrame++;
    if (gHwDead)
        return;
    if (gDbg)
        lg("  present: screen %dx%d %d bpp base %x, ensure3D", pm->bounds.right - pm->bounds.left,
           pm->bounds.bottom - pm->bounds.top, pm->pixelSize, (NVU32)pm->baseAddr);
    if (pm->pixelSize != t->bpp || S->ensure3D() != 0 || S->a->depth != (NVU32)t->bpp) {
        if (first_time(0x71000000 | (pm->pixelSize << 8) | (NVU32)t->bpp))
            lg("present skipped: screen %d bpp, buffer %d bpp, 2D surface %u bpp, 3D state %u",
               pm->pixelSize, t->bpp, S->a->depth, S->state3D);
        return;
    }
    if (gDbg)
        lg("  present: blit (2D surface %u bpp)", S->a->depth);
    if (!clip || !*clip || (*clip)->rgnSize == 10) {
        if (clip && *clip) {
            Rect *bb = &(*clip)->rgnBBox;
            blit(t, pm, dst, bb->left, bb->top, bb->right, bb->bottom);
        } else {
            blit(t, pm, dst, dst->left, dst->top, dst->right, dst->bottom);
        }
    } else {
        short xs[MAX_XS], y, prevY = 0, x;
        const short *p;
        int n = 0, i;
        HLock((Handle)clip);
        p = (const short *)((const char *)*clip + 10);
        for (;;) {
            y = *p++;
            if (y == 0x7FFF)
                break;
            if (n > 0 && y > prevY)
                for (i = 0; i + 1 < n; i += 2)
                    blit(t, pm, dst, xs[i], prevY, xs[i + 1], y);
            while ((x = *p++) != 0x7FFF)
                n = toggle_x(xs, n, x);
            prevY = y;
        }
        HUnlock((Handle)clip);
    }
    if (gRefOK == 1)
        ref_emit(gFrame - 1);                       /* frame gFrame − 1 fully submitted */
    pend_process(0);
    nva_kickoff(S->a);
    present_throttle();
}

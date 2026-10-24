/*
 * NV34 MiniGL — hardvérová vrstva (NV30 3D trieda 0x0697, metódy z nv30-40_3d.xml.h, MIT).
 * Vychádza z NV34 RAVE enginu (nv34rave/engine.c): rovnaké render targety (farba aj zeta
 * s rovnakým pitchom), inline vrcholy (VERTEX_DATA), combiners, swizzled textúry, 2D blit.
 */
#include <MacMemory.h>
#include <Events.h>
#include "minigl.h"

#define SUBC                7
#define M_RT_HORIZ          0x0200
#define M_TEX_UNITS_ENABLE  0x023c
#define M_TEX_MATRIX_ENABLE 0x0240
#define M_VIEWPORT_CLIP_H   0x02c0
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
#define M_VTXFMT            0x1740
#define M_VERTEX_BEGIN_END  0x1808
#define M_VERTEX_DATA       0x1818
#define M_CULL_FACE_ENABLE  0x183c
#define M_TEX_OFFSET0       0x1a00
#define M_DEPTH_CONTROL     0x1d78
#define M_FP_CONTROL        0x1d60
#define M_CLEAR_DEPTH_VALUE 0x1d8c
#define M_CLEAR_COLOR_VALUE 0x1d90
#define M_CLEAR_BUFFERS     0x1d94

/* TXPH H0, a[tex0], t[0]; END (kódovanie pre BE 3D objekt, gl-5) */
static const NVU32 kFpTex[4] = { 0x18009e81, 0x1c9dc801, 0x0001c800, 0x3fe1c800 };

static NVU32 gFrame = 1, gEpoch = 1;
static HwTex *gTexList;

/* posledný nastavený hardvérový stav (-1 = neznámy) */
static long hwMode = -1, hwBlend = -1, hwAlpha = -1, hwDepth = -1, hwMask = -1, hwScis = -1, hwFmt = -1;
static long hwTex = -1, hwTexKey = -1, hwTexEpoch = -1;

static void invalidate(void)
{
    hwMode = hwBlend = hwAlpha = hwDepth = hwMask = hwScis = hwFmt = -1;
    hwTex = hwTexKey = hwTexEpoch = -1;
}

/* hl-2: ak GPU neodpovedá (timeout pri čakaní na miesto v push buffri), ďalej nič nepísať —
 * zápis za rezervované miesto by prepísal príkazy, ktoré GPU ešte číta */
static int gHwDead;

static void hw_fail(const char *what)
{
    if (!gHwDead)
        lg("GPU neodpoveda (%s): GET %x PUT %x, kreslenie vypnute", what, S->a->errGet, S->a->errPut);
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
NVU32 gTEnd, gTFlush, gTWait, gTSwap;

int hw_sync(void)
{
    NVU32 t0 = tb();
    int ok;
    gHwSyncs++;
    ok = S && !gHwDead && nva_sync(S->a, 120) == kNVAOK;
    if (S && !ok && !gHwDead && first_time(0x70000000))
        lg("hw_sync: GPU nedobehol do 2 s");
    gTWait += tb() - t0;
    return ok;
}
NVU32 hw_frame(void) { return gFrame; }

/* hl-10: GPU fence cez REF_CNT kanála (metóda 0x50, čítanie USER + 0x48, ako nouveau nv10_fence).
 * Pri present sa pošle číslo dokončenej snímky; textúru použitú v snímke F možno prepísať na mieste,
 * až keď REF ≥ F (push buffer 512 KB → GPU môže zaostávať o celú snímku; baterka v Half-Life
 * kreslila staré snímky s novými svetelnými mapami = čierne štvorce). */
static int gRefOK = -1;

static NVU32 gpu_done(void)
{
    if (gRefOK != 1)
        return gFrame - 1;                          /* bez fence: ako predtým */
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
        lg("GPU fence (REF_CNT): %s", gRefOK ? "ok" : "nefunguje");
    }
    return 1;
}

int hw_target_alloc(HwTarget *t, int w, int h, int bpp)
{
    t->w = w;
    t->h = h;
    t->bpp = bpp;
    /* farba a zeta Z24S8 s rovnakým pitchom (rave-2, rave-16) */
    t->colorPitch = t->zPitch = (w * 4 + 63) & ~63L;
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
        t->generation = S->generation;
    }
    begin(M_RT_HORIZ, 5);
    data(t->w << 16);
    data(t->h << 16);
    data((t->bpp == 16 ? 0x102 : 0x108) | 0x40);    /* farba | Z24S8 | linear */
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
    begin(M_FP_CONTROL, 1);
    data(0);
    begin(M_FP_REG_CONTROL, 1);
    data(0x0001000f);
    begin(M_TEX_MATRIX_ENABLE, 1);
    data(0);
    begin(M_CULL_FACE_ENABLE, 1);                  /* orezanie odvrátených stien robí CPU */
    data(0);
    begin(M_DEPTH_CONTROL, 1);                      /* hĺbku obmedziť, neorezávať */
    data(0x10);
    begin(M_DITHER_ENABLE, 1);
    data(t->bpp == 16);
    if (fresh) {                                    /* prvý CLEAR hĺbky po inite nezaberie (rave-3) */
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

/* opt-5: pozícia 3×float (w = 1), farba 4×U8_UNORM v jednom slove, textúra 4×float →
 * 8 slov na vrchol s textúrou (predtým 12), 4 bez nej (predtým 8). boot-args nv34glvtx12 =
 * pôvodný formát. Farba R<<24|G<<16|B<<8|A (opt-6 s opačným poradím mala ružový nádych);
 * nv34glcolorle = opačné poradie. */
static int gVtx12 = -1, gColorBE;

static void set_vtxfmt(int tex)
{
    int i;
    if (hwFmt == tex)
        return;
    begin(M_VTXFMT, 16);
    if (gVtx12) {
        NVU32 f = 0x2 | (4u << 4) | ((tex ? 48u : 32u) << 8);
        for (i = 0; i < 16; i++)
            data(i == 0 || i == 3 || (i == 8 && tex) ? f : 0x2);
    } else {
        NVU32 st = (tex ? 32u : 16u) << 8;
        for (i = 0; i < 16; i++)
            data(i == 0 ? (0x2 | (3u << 4) | st) :           /* V32_FLOAT × 3 */
                 i == 3 ? (0x4 | (4u << 4) | st) :           /* U8_UNORM × 4 */
                 (i == 8 && tex) ? (0x2 | (4u << 4) | st) : 0x2);
    }
    hwFmt = tex;
}

static NVU32 tex_filter(const HwState *st, const HwTex *t)
{
    NVU32 mag = st->magFilter == GL_NEAREST ? 0x01000000 : 0x02000000, min;
    if (t->levels <= 1) {
        min = (st->minFilter == GL_NEAREST || st->minFilter == GL_NEAREST_MIPMAP_NEAREST ||
               st->minFilter == GL_NEAREST_MIPMAP_LINEAR) ? 0x00010000 : 0x00020000;
    } else {
        switch (st->minFilter) {
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
    long key = st->scissor ? (long)((st->sx << 20) ^ (st->sy << 10) ^ (st->sw << 5) ^ st->sh ^ 0x40000000) : 0;
    int x = 0, y = 0, w = t->w, h = t->h;
    if (hwScis == key)
        return;
    if (st->scissor) {
        x = st->sx; y = st->sy; w = st->sw; h = st->sh;
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > t->w) w = t->w - x;
        if (y + h > t->h) h = t->h - y;
        if (w < 0) w = 0;
        if (h < 0) h = 0;
    }
    begin(M_SCISSOR_HORIZ, 2);
    data(((NVU32)w << 16) | (NVU32)x);
    data(((NVU32)h << 16) | (NVU32)y);
    hwScis = key;
}

/* vráti 1, ak sa kreslí s textúrou (textúra je vo VRAM) */
int hw_state(HwTarget *t, const HwState *st)
{
    long key;
    int mode = st->tex && tex_resident(st->tex);

    /* textúra */
    if (mode) {
        HwTex *x = st->tex;
        NVU32 filt = tex_filter(st, x);
        NVU32 wrap = wrap_mode(st->wrapS) | (wrap_mode(st->wrapT) << 8) | (1u << 16);
        key = (long)(filt ^ (wrap << 4));
        if (hwTex != (long)x || hwTexKey != key || hwTexEpoch != (long)gEpoch) {
            begin(M_TEX_OFFSET0, 8);
            data(x->off);
            data(0x08 | 0x20 | ((NVU32)x->fmt << 8) | (1u << 16) | (x->levels > 1 ? 0x00080000 : 0) |
                 ((NVU32)x->log2w << 20) | ((NVU32)x->log2h << 24) | 0x01);
            data(wrap);
            data(0x40000000 | ((((NVU32)x->levels - 1) << 8) << 6));
            data(0xAAE4);
            data(filt);
            data(((NVU32)x->w << 16) | (NVU32)x->h);
            data(0);
            hwTex = (long)x;
            hwTexKey = key;
            hwTexEpoch = gEpoch;
        }
        x->frameUsed = gFrame;
        x->lastUse = gFrame;
    }
    if (hwMode != mode) {
        if (!mode) {
            begin(M_TEX_UNITS_ENABLE, 1);
            data(0);
            begin(M_RC_FINAL0, 3);
            data(0x00000004);                       /* farba = primárna */
            data(0x00001400);
            data(0x01000101);
        } else {
            begin(M_TEX_UNITS_ENABLE, 1);
            data(1);
            begin(M_RC_IN_ALPHA0, 6);               /* tex0 × primárna → spare0 */
            data((0x18u << 24) | (0x14u << 16));
            data((0x08u << 24) | (0x04u << 16));
            data(0);
            data(0);
            data(0x00000c00);
            data(0x00000c00);
            begin(M_RC_FINAL0, 3);
            data(0x0000000c);
            data(0x00001c00);
            data(0x01000101);
        }
        hwMode = mode;
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
    /* hĺbka: bez testu ani zápis (GL) */
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

/* ---- vrcholy ------------------------------------------------------------- */

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
    if (gVtx12 < 0) {
        gVtx12 = gl_boot_arg("nv34glvtx12");
        gColorBE = !gl_boot_arg("nv34glcolorle");   /* opt-7: GPU číta R v najvyššom bajte */
    }
    wpv = gVtx12 ? (textured ? 12 : 8) : (textured ? 8 : 4);
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
            hw_fail("vrcholy");
            return;
        }
        gTWait += tb() - t0;                        /* čakanie na miesto v push buffri */
        /* opt-1: miesto je rezervované → zapisovať priamo (bez volania nva_push na slovo) */
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

/* ---- textúry ------------------------------------------------------------- */

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
        t = (HwTex *)NewPtrSysClear(sizeof(HwTex));     /* q3-7: Quake III zaplní haldu aplikácie */
    if (!t) {
        if (first_time(0x71100000))
            lg("hw_tex_new: malo pamate");
        return 0;
    }
    t->magic = 'NVgt';
    t->next = gTexList;
    if (gTexList)
        gTexList->prev = t;
    gTexList = t;
    return t;
}

/* opt-5: VRAM textúry, ktorú GPU možno ešte číta, sa uvoľní až po jej poslednej snímke (fence) —
 * predtým každé glDeleteTextures čakalo na GPU */
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
        hw_sync();                                  /* bez fence ako predtým */
    S->vramFree(off);
}

/* pri ukončení / zrušení kontextu: všetko odložené vrátiť do arény */
void hw_pend_flush(void)
{
    if (!S || !gPendN)
        return;
    hw_sync();
    pend_process(1);
}

/* glFinish (opt-5): počkať len na predchádzajúcu snímku, nie na celú prácu GPU */
void hw_finish(void)
{
    NVU32 t0;
    if (!S || gHwDead)
        return;
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
    DisposePtr((Ptr)t);
}

/* VRAM pre textúru; pri nedostatku uvoľní najdlhšie nepoužité (dáta sú v RAM v GL vrstve) */
static NVU32 alloc_evict(HwTex *t, NVU32 size)
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
                if (v != t && (v->off || v->alt) && (!lru || v->lastUse < lru->lastUse))
                    lru = v;
            if (!lru)
                break;
            if (lru->off) S->vramFree(lru->off);
            if (lru->alt) S->vramFree(lru->alt);
            lru->off = lru->alt = 0;
            lru->levels = 0;                        /* GL vrstva ju nahrá znova */
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
    NVU32 size = 0, lw = log2i(w), lh = log2i(h), x, y, bpp = fmt == 0x06 ? 4 : 2;
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
    if (t->size != size) {                          /* iná veľkosť: nové oblasti */
        vram_release(t->off, t->frameUsed);
        vram_release(t->alt, t->altUsed);
        t->off = t->alt = 0;
        t->size = size;
    }
    if (t->off && t->frameUsed > gpu_done()) {
        /* GPU ju v tejto snímke možno ešte číta → zapisovať do druhej oblasti (rave-22);
         * ak sa v tejto snímke čítala aj druhá (Quake 2 dynamická lightmapa), počkať na GPU */
        NVU32 tmp;
        if (!t->alt)
            t->alt = alloc_evict(t, size);
        if (!t->alt || t->altUsed > gpu_done()) {
            hw_sync();
        } else {
            tmp = t->off;
            t->off = t->alt;
            t->alt = tmp;
            tmp = t->frameUsed;                     /* aj snímky posledného čítania si vymenia miesto */
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
        } else if (srcfmt == fmt) {                 /* wolf-2: kópia v RAM už 16-bit */
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
    gEpoch++;
    gHwUploads++;
    gHwUploadBytes += size;
    return 1;
}

/* opt-1: len obdĺžnik úrovne 0 priamo na mieste (lightmapy Quake 2 po každom glTexSubImage2D
 * nahrávali celých 64 KB). Iba ak ju GPU v tejto snímke ešte nečítal; inak 0 → celé nahratie. */
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
    gEpoch++;                                       /* znova TEX_OFFSET (vyprázdni cache textúr) */
    gHwSubUploads++;
    gHwUploadBytes += (NVU32)w * h * (t->fmt == 0x06 ? 4 : 2);
    return 1;
}

/* ---- prenos na obrazovku ---------------------------------------------------- */

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
    /* hl-2: aj na hranice obrazovky (po zmene rozlíšenia môže okno presahovať) */
    if (l < pm->bounds.left) l = pm->bounds.left;
    if (tp < pm->bounds.top) tp = pm->bounds.top;
    if (r > pm->bounds.right) r = pm->bounds.right;
    if (b > pm->bounds.bottom) b = pm->bounds.bottom;
    if (l >= r || tp >= b)
        return;
    nva_copy_from(S->a, t->colorOff, t->colorPitch, (int)(l - dst->left), (int)(tp - dst->top),
                  (int)(l - pm->bounds.left), (int)(tp - pm->bounds.top), (int)(r - l), (int)(b - tp));
}

void hw_present(HwTarget *t, GDHandle gd, const Rect *dst, RgnHandle clip)
{
    PixMapPtr pm = *(*gd)->gdPMap;

    gFrame++;
    if (gHwDead)
        return;
    if (gDbg)
        lg("  present: obrazovka %dx%d %d bpp base %x, ensure3D", pm->bounds.right - pm->bounds.left,
           pm->bounds.bottom - pm->bounds.top, pm->pixelSize, (NVU32)pm->baseAddr);
    if (pm->pixelSize != t->bpp || S->ensure3D() != 0 || S->a->depth != (NVU32)t->bpp) {
        if (first_time(0x71000000 | (pm->pixelSize << 8) | (NVU32)t->bpp))
            lg("present vynechany: obrazovka %d bpp, buffer %d bpp, 2D povrch %u bpp, 3D stav %u",
               pm->pixelSize, t->bpp, S->a->depth, S->state3D);
        return;
    }
    if (gDbg)
        lg("  present: blit (2D povrch %u bpp)", S->a->depth);
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
        ref_emit(gFrame - 1);                       /* snímka gFrame − 1 odoslaná celá */
    pend_process(0);
    nva_kickoff(S->a);
}

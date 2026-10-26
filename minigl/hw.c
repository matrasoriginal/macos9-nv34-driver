/*
 * NV34 MiniGL — hardvérová vrstva (NV30 3D trieda 0x0697, metódy z nv30-40_3d.xml.h, MIT).
 * Vychádza z NV34 RAVE enginu (nv34rave/engine.c): rovnaké render targety (farba aj zeta
 * s rovnakým pitchom), inline vrcholy (VERTEX_DATA), combiners, swizzled textúry, 2D blit.
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

/* TXPH H0, a[tex0], t[0]; END (kódovanie pre BE 3D objekt, gl-5) */
static const NVU32 kFpTex[4] = { 0x18009e81, 0x1c9dc801, 0x0001c800, 0x3fe1c800 };

/* q3-12: dve textúry (GL_MODULATE na jednotke 1), combiners potom × primárna farba ako pri jednej:
 *   TXP R1, a[tex1], t[1]
 *   TXPH H0, a[tex0], t[0]
 *   MULH H0, H0, R1; END
 * Program leží za prvým (fpOffset + FP2_OFF). */
#define FP2_OFF 0x100
static const NVU32 kFpTex2[12] = {
    0x1802be02, 0x1c9dc801, 0x0001c800, 0x3fe1c800,
    0x18009e80, 0x1c9dc801, 0x0001c800, 0x3fe1c800,
    0x02001e81, 0x1c9dc900, 0x0001c804, 0x3fe1c800,
};

/* ako nv3d_upload_fp (swapMode 1), na posunutie off */
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

/* posledný nastavený hardvérový stav (-1 = neznámy) */
static long hwMode = -1, hwBlend = -1, hwAlpha = -1, hwDepth = -1, hwMask = -1, hwScis = -1, hwFmt = -1;
static long hwTex = -1, hwTexKey = -1, hwTexEpoch = -1;
static long hwTex1 = -1, hwTex1Key = -1, hwTex1Epoch = -1;
static long hwTnl = -1, hwMatGen = -1, hwCull = -1, hwMv = -1;
static long hwFin = -1, hwFogOn = -1, hwProg = -1;
static NVU32 hwFogK[6];
static int gVtx12 = -1, gColorBE, gCompact;        /* formát vrcholu (opt-5, q3-17) */
static int gNoFog, gFogPlane;

static void invalidate(void)
{
    hwMode = hwBlend = hwAlpha = hwDepth = hwMask = hwScis = hwFmt = -1;
    hwTex = hwTexKey = hwTexEpoch = -1;
    hwTex1 = hwTex1Key = hwTex1Epoch = -1;
    hwTnl = hwMatGen = hwCull = hwMv = -1;
    hwFin = hwFogOn = hwProg = -1;
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

static int gVbOn = -1, gVbUsed;

int hw_sync(void)
{
    NVU32 t0 = tb();
    int ok;
    gHwSyncs++;
    ok = S && !gHwDead && nva_sync(S->a, 120) == kNVAOK;
    if (S && !ok && !gHwDead && first_time(0x70000000)) {
        NV3DTrap tr;
        nv3d_trap(S->g, &tr);                       /* q3-19: ktorá metóda GPU zastavila */
        lg("hw_sync: GPU nedobehol do 2 s; PGRAPH intr %x nsource %x nstatus %x addr %x data %x status %x, "
           "PFIFO intr %x get %x put %x%s", tr.intr, tr.nsource, tr.nstatus, tr.addr, tr.data, tr.status,
           tr.fifoIntr, tr.dmaGet, tr.dmaPut, gVbUsed ? " (po kresleni z VRAM, vypinam ho)" : "");
        if (gVbUsed)
            gVbOn = 0;
        if (tr.intr)
            ok = nva_sync(S->a, 120) == kNVAOK;     /* po potvrdení chyby skúsiť dobehnúť */
    }
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
        {
            /* q3-8: takt jadra a pamäte (PLL ako v nouveau nv04_pm / nv30: f = krystal × N / M >> P;
             * NV31+ môže mať druhý stupeň v *_COEFF_B) — či GPU nebeží v úspornom režime */
            NVU32 boot = nva_rd32(S->a, 0x101000);
            NVU32 xtal = (boot & 0x40) ? 14318 : 13500;    /* kHz */
            NVU32 n1 = nva_rd32(S->a, 0x680500), n2 = nva_rd32(S->a, 0x680570);
            NVU32 m1 = nva_rd32(S->a, 0x680504), m2 = nva_rd32(S->a, 0x680574);
            NVU32 nm = n1 & 0xff, nn = (n1 >> 8) & 0xff, np = (n1 >> 16) & 7;
            NVU32 mm = m1 & 0xff, mn = (m1 >> 8) & 0xff, mp = (m1 >> 16) & 7;
            lg("PLL: jadro %x/%x (~%u MHz), pamat %x/%x (~%u MHz), krystal %u kHz", n1, n2,
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
    /* farba a zeta s rovnakým pitchom (rave-2, rave-16). q3-10: pri 16 bpp a požadovanej hĺbke
     * ≤ 16 bitov Z16 — polovičný prenos pamäte pri teste hĺbky (Quake III je obmedzený GPU);
     * boot-args nv34glz24 = vždy Z24S8 */
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
    data((t->bpp == 16 ? 0x102 : 0x108) | (t->z16 ? 0x20 : 0x40));   /* farba | Z16/Z24S8 | linear */
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
    begin(M_DMA_VTXBUF0, 1);                        /* q3-18: vrcholy vo VRAM (VTXBUF bez bitu 31) */
    data(NV3D_HANDLE_VRAM);
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
static void vtx_init(void)
{
    if (gVtx12 < 0) {
        gVtx12 = gl_boot_arg("nv34glvtx12");
        gColorBE = !gl_boot_arg("nv34glcolorle");   /* opt-7: GPU číta R v najvyššom bajte */
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
        NVU32 st = (tex == 2 ? 48u : tex ? 32u : 16u) << 8;   /* q3-12: 2 textúry = 12 slov */
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

/* textúrová jednotka u (blok metód 0x1a00 + 0x20·u), stav v cTex, cKey, cEpoch */
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

/* q3-16: transformácia. Fixed-function NV34 násobí vrchol maticou PROJECTION (ako NV10/NV20
 * v nouveau: zložená matica), MODELVIEW ostáva jednotková; viewport (posun 0, mierka 1) je
 * v matici. Bez T&L jednotková matica (vrcholy v súradniciach okna, w = 1).
 * boot-args nv34gltnlt = matica transponovaná, nv34gltnlclamp = hĺbku obmedziť (bez orezania
 * blízkou/ďalekou rovinou), nv34gltnlcw = predná strana otočená (q3-16 predvolené, zlé).
 * q3-17: v T&L dávke textúrové súradnice len s, t (q = 1 doplní GPU) → 6 slov na vrchol
 * (s dvoma textúrami 8); boot-args nv34gltnl4 = 4 floaty ako bez T&L. */
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
        begin(M_DEPTH_CONTROL, 1);                  /* 0x01 = orezať hĺbkou (GL), 0x10 = obmedziť */
        data(st->tnl && !gTnlClamp ? 0x01 : 0x10);
        if (!st->tnl) {
            mat_upload(M_PROJECTION0, 0);
            hwMatGen = -1;
        }
        hwTnl = st->tnl;
        gCompact = st->tnl && !gTnl4 && !gVtx12;
    }
    /* PROJECTION = celá matica okno × MVP (poloha, ako nouveau nv20_emit_projection); MODELVIEW len pre
     * vzdialenosť hmly v priestore oka, inak identita */
    if (st->tnl && (hwMatGen != (long)st->matGen || hwMv != (st->mv != 0))) {
        mat_upload(M_PROJECTION0, st->mat);
        if (st->mv || hwMv)
            mat_upload(M_MODELVIEW0, st->mv);
        hwMatGen = st->matGen;
        hwMv = st->mv != 0;
    }
    /* odvrátené steny: bez T&L ich odstráni CPU. GPU berie FRONT_FACE podľa GL (q3-16: s otočením
     * kvôli y dole zmizlo všetko) */
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

/* ---- max-4: fragment programy pre glTexEnv (GL_COMBINE, BLEND, ADD, DECAL na 2 jednotkách) ----------
 * Kódovanie NV30 podľa nouveau nvfx_shader.h (overené na kFpTex/kFpTex2). Program:
 *   TXP R1, a[tex0], t[0]; (TXP R2, a[tex1], t[1]);
 *   pre jednotku u: R3.xyz = op(argumenty RGB), R3.w = op(argumenty alfa)  (predošlá = a[col0] / R3)
 *   MOVH H0, R3; END
 * Argumenty s OPERAND 1−x a konštanty (TEXTURE_ENV_COLOR) idú najprv do R4–R6 (jedna inline konštanta
 * a jeden vstup na inštrukciu). Výsledok H0 vidia combiners ako TEXTURE0, final combiner ho berie priamo.
 * Programy v VRAM od fpOffset + FPG_BASE, 64 miest po 512 B (kľúč = stav prostredia). */
#define FPG_BASE    0x400
#define FPG_SLOT    0x200
#define FPG_N       64
#define FPG_KEYW    48

#define FO_MOV  0x01
#define FO_MUL  0x02
#define FO_ADD  0x03
#define FO_DP3  0x05
#define FO_TXP  0x18
#define FO_LRP  0x1F

typedef struct FSrc { int type, reg, neg; NVU32 swz; float c[4]; } FSrc;   /* type 0 temp, 1 vstup, 2 konšt. */

static NVU32 gFpg[160];
static int gFpgN, gFpgMaxR, gFpgLast, gFpgOver;
static NVU32 gFpgKey[FPG_N][FPG_KEYW], gFpgCtl[FPG_N];
static int gFpgUsed, gFpgNext, gFpgLastSlot = -1;
NVU32 gFpgEvict;

/* vstup (a[col0], a[texN]) sa udáva v slove 0 inštrukcie, v zdroji len typ a swizzle (kFpTex 0x1c9dc801) */
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

/* argument i jednotky u (alpha = 0 RGB, 1 alfa) → FSrc, prípadne cez R4+i */
static FSrc fpg_arg(const HwEnvUnit *e, int u, int units, int alpha, int i)
{
    GLenum src = e->src[alpha][i], op = e->op[alpha][i];
    int am = op == GL_SRC_ALPHA || op == 0x0303, inv = op == 0x0301 || op == 0x0303;
    FSrc x;
    memset(&x, 0, sizeof(x));
    x.swz = am ? 0xFF : 0xE4;
    if (src == 0x8576) {                            /* CONSTANT: hodnota na CPU */
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
    if (src == 0x8577 || (src == 0x8578 && u == 0)) {   /* PRIMARY_COLOR, PREVIOUS na jednotke 0 */
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

/* jedna časť (RGB alebo alfa) jednotky u do R3 s maskou m */
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

/* pevné režimy → COMBINE */
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
    case GL_DECAL:                                  /* RGB = lerp(predošlá, tex, tex.a), alfa = predošlá */
        e->comb[0] = 0x8575;
        e->src[0][2] = GL_TEXTURE;
        e->op[0][2] = GL_SRC_ALPHA;
        e->comb[1] = GL_REPLACE;
        e->src[1][0] = 0x8578;
        break;
    case GL_BLEND:                                  /* RGB = lerp(farba prostredia, predošlá, tex) */
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

/* max-5: počet argumentov operácie */
static int fpg_nargs(GLenum comb)
{
    return comb == GL_REPLACE ? 1 : comb == 0x8575 ? 3 : 2;
}

/* max-5: v kľúči len to, čo program naozaj použije (max-4 generoval kvôli zmene nepoužitej
 * TEXTURE_ENV_COLOR stále nové programy → 64 miest sa prepisovalo počas kreslenia, textúry blikali) */
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
    if (e->comb[0] == 0x86AF) {                     /* DOT3_RGBA: alfa sa nepočíta */
        e->comb[1] = 0;
        for (i = 0; i < 3; i++)
            e->src[1][i] = e->op[1][i] = 0;
    }
    if (!usesConst)
        e->color[0] = e->color[1] = e->color[2] = e->color[3] = 0;
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

/* program pre prostredie env (units jednotiek) → posun v VRAM a FP_CONTROL */
static NVU32 fpg_get(const HwEnvUnit *env, int units, NVU32 *ctl)
{
    NVU32 key[FPG_KEYW];
    FSrc r3;
    int i, slot, u;
    fpg_key(env, units, key);
    if (gFpgLastSlot >= 0 && !memcmp_w(gFpgKey[gFpgLastSlot], key, FPG_KEYW)) {
        *ctl = gFpgCtl[gFpgLastSlot];
        return FPG_BASE + (NVU32)gFpgLastSlot * FPG_SLOT;
    }
    for (i = 0; i < gFpgUsed; i++)
        if (!memcmp_w(gFpgKey[i], key, FPG_KEYW)) {
            gFpgLastSlot = i;
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
            lg("texenv program: prilis dlhy (%d slov)", gFpgN);
        *ctl = 0;
        return 0;
    }
    if (gFpgUsed < FPG_N) {
        slot = gFpgUsed++;
    } else {
        slot = gFpgNext;
        gFpgNext = (gFpgNext + 1) % FPG_N;
        hw_sync();                                  /* max-5: GPU môže čítať starý program aj z tejto snímky */
        gFpgEvict++;
    }
    for (i = 0; i < FPG_KEYW; i++)
        gFpgKey[slot][i] = key[i];
    gFpgCtl[slot] = (NVU32)gFpgMaxR / 2;            /* (počet R − 1) / 2 */
    fp_upload(FPG_BASE + (NVU32)slot * FPG_SLOT, gFpg, gFpgN);
    if (gFpgUsed <= 12) {
        lg("texenv program %d: %d jednotky, %d slov, R0-R%d; j0 %x %x/%x z %x %x %x / %x %x %x", slot, units, gFpgN,
           gFpgMaxR, env[0].mode, env[0].comb[0], env[0].comb[1], env[0].src[0][0], env[0].src[0][1],
           env[0].src[0][2], env[0].op[0][0], env[0].op[0][1], env[0].op[0][2]);
        if (units == 2)
            lg("  j1 %x %x/%x z %x %x %x / %x %x %x", env[1].mode, env[1].comb[0], env[1].comb[1], env[1].src[0][0],
               env[1].src[0][1], env[1].src[0][2], env[1].op[0][0], env[1].op[0][1], env[1].op[0][2]);
    }
    gFpgLastSlot = slot;
    *ctl = gFpgCtl[slot];
    return FPG_BASE + (NVU32)slot * FPG_SLOT;
}

/* vráti 0 bez textúry, 1 s textúrou, 2 s dvoma (textúry sú vo VRAM) */
int hw_state(HwTarget *t, const HwState *st)
{
    long key;
    int mode = st->tex && tex_resident(st->tex), fog, gen;

    /* q3-12: druhá jednotka (nie s pôvodným 12-slovným formátom nv34glvtx12) */
    vtx_init();
    if (mode && st->tex1 && tex_resident(st->tex1) && !gVtx12)
        mode = 2;
    /* textúra */
    if (mode) {
        tex_unit(0, st->tex, st->minFilter, st->magFilter, st->wrapS, st->wrapT, &hwTex, &hwTexKey, &hwTexEpoch);
        if (mode == 2)
            tex_unit(1, st->tex1, st->minFilter1, st->magFilter1, st->wrapS1, st->wrapT1, &hwTex1, &hwTex1Key,
                     &hwTex1Epoch);
    }
    if (hwMode != mode) {
        if (hwMode == 2 || hwMode < 0) {            /* jednotku 1 vypnúť, späť program s jednou textúrou */
            begin(M_TEX_OFFSET0 + 0x20 + 0x0c, 1);
            data(0);
            hwTex1 = -1;
        }
        if (!mode) {
            begin(M_TEX_UNITS_ENABLE, 1);
            data(0);
        } else {
            begin(M_TEX_UNITS_ENABLE, 1);
            data(mode == 2 ? 3 : 1);                /* interpolované súradnice tex0 (a tex1) */
            begin(M_RC_IN_ALPHA0, 6);               /* tex0 (pri dvoch tex0 × tex1 z programu) × primárna → spare0 */
            data((0x18u << 24) | (0x14u << 16));
            data((0x08u << 24) | (0x04u << 16));
            data(0);
            data(0);
            data(0x00000c00);
            data(0x00000c00);
        }
        hwMode = mode;
    }
    /* program: kFpTex / kFpTex2, max-4: pri inom prostredí vygenerovaný (FP_CONTROL = počet registrov) */
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
    /* final combiner: farba = D (primárna / spare0); s hmlou A·B + (1−A)·C, A = FOG.a (faktor hmly),
     * B = farba, C = FOG.rgb (farba hmly) ako v nouveau nv10_state_frag.c */
    /* wolf-8: len pri T&L — zdroj 6 (súradnica z vrcholu, NV20) NV30 odmietol (DATA_ERROR na 0x8c8) */
    fog = st->fog && st->tnl && !gNoFog && !gVtx12;
    key = (mode != 0) | (fog << 1) | (gen << 2);
    if (hwFin != key) {
        NVU32 col = gen ? 0x08 : mode ? 0x0c : 0x04;   /* max-4: výsledok programu (TEXTURE0) priamo */
        begin(M_RC_FINAL0, 3);
        data(fog ? (0x13u << 24) | (col << 16) | (0x03u << 8) : col);
        data((col | 0x10) << 8);
        data(0x01000101);
        hwFin = key;
    }
    if (fog) {
        /* zdroj: radiálna vzdialenosť (RADIAL, režimy SIGNED). wolf-9: ORTHOGONAL_ABS s UNSIGNED (NV20,
         * predvolené v Mesa) na NV30 hmlu nedal → len boot-args nv34glfogplane (pokus) */
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
                lg("hmla: zdroj %u, rezim %x, k %f %f/1e6, farba %x (T&L %d)", k[0], k[1], (double)a.f, (double)b.f * 1e6,
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

/* ---- q3-18: vrcholy vo VRAM + indexy ------------------------------------------
 * Kruhový buffer 2 MB v 16 úsekoch; úsek nesie číslo snímky, ktorá ho naposledy použila, a znova
 * sa doň píše, až keď ho GPU dokončil (REF_CNT = snímka). Do úseku, v ktorom sa práve píše, sa
 * pokračuje bez kontroly; keby sa kruh pretočil v rámci jednej snímky, počká sa na celé GPU.
 * Vrchol má rovnaký formát ako kratší inline vrchol T&L (q3-17). Indexy idú do push buffra
 * (VB_ELEMENT_U16, prvý index v dolnej polovici slova ako v nouveau).
 * q3-19: GPU odmietol prvé VB_ELEMENT_U16 (DATA_ERROR, INVALID_STATE) → q3-20 len na boot-args
 * nv34glvb; nv34glvbbatch = bez indexov (vrcholy v poradí trojuholníkov, VB_VERTEX_BATCH).
 * boot-args nv34glvbswap = vrchol v poradí bajtov LE, nv34glidxhi = prvý
 * index v hornej polovici slova, nv34glvbinv = VTX_CACHE_INV (0x1710) pred kreslením. */
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
        gVbOn = gl_boot_arg("nv34glvb");             /* q3-20: GPU v q3-18/19 odmietol VB_ELEMENT (pokus) */
        gVbBatch = gl_boot_arg("nv34glvbbatch");
        gVbSwap = gl_boot_arg("nv34glvbswap");
        gIdxHi = gl_boot_arg("nv34glidxhi");
        gVbInv = gl_boot_arg("nv34glvbinv");
    }
    vtx_init();
    tnl_args();
    if (!gVbOn || gRefOK != 1 || gTnl4 || gVtx12 || gHwDead)   /* len kratší formát T&L */
        return 0;
    if (gVbOff && gVbGen != S->generation)          /* 3D znova inicializované: buffer zabudnúť */
        gVbOff = 0;
    if (!gVbOff) {
        gVbOff = S->vramAlloc(VB_SIZE, 256);
        if (!gVbOff) {
            lg("vrcholy vo VRAM: nie je miesto (%u KB)", VB_SIZE >> 10);
            gVbOn = 0;
            return 0;
        }
        memset(gVbSegFrame, 0, sizeof(gVbSegFrame));
        gVbPos = 0;
        gVbCurSeg = -1;
        gVbGen = S->generation;
        if (first_time(0x72000000))
            lg("vrcholy vo VRAM: %u KB na %x", VB_SIZE >> 10, gVbOff);
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
                hw_sync();                          /* kruh pretočený v jednej snímke */
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

/* jeden vrchol do VRAM: x, y, z, farba, s, t (s1, t1) */
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
    if (gVbInv) {                                   /* q3-19: v q3-18 GPU zastal — možno len NV40 */
        begin(M_VTX_CACHE_INV, 1);
        data(0);
    }
    if (!gVbUsed)
        lg("prve kreslenie z VRAM: vrcholy na %x, textury %d, %s %d", vbOff, textured, idx ? "indexov" : "vrcholov",
           n);
    gVbUsed = 1;
    begin(M_VERTEX_BEGIN_END, 1);
    data(prim);
    if (!idx) {                                     /* VB_VERTEX_BATCH: (počet − 1) << 24 | začiatok */
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
            hw_fail("indexy");
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

/* q3-20: inline vrcholy T&L priamo z pripravenej tabuľky podľa indexov (bez kópie do dávky);
 * len kratší formát (q3-17). 0 = nedá sa. */
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
            hw_fail("vrcholy");
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
    if (gNoFinish) {                                /* q3-9: test — len odoslať, nečakať */
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
                if (v != t && v != gTexPin && (v->off || v->alt) && (!lru || v->lastUse < lru->lastUse))
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

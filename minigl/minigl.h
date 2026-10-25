/*
 * NV34 MiniGL — vlastná OpenGLLibrary (OpenGL 1.1 + AGL) pre GeForce FX Go5200 v Mac OS 9.
 *
 * Knižnica sa volá rovnako ako Apple „OpenGLLibrary“ a exportuje všetkých jej 474 symbolov.
 * Položená do priečinka hry ju CFM nájde skôr ako systémovú (len pre túto hru).
 * Transformácia, orezanie blízkou rovinou a projekcia bežia na CPU; GPU dostane vrcholy
 * v súradniciach okna s projektívnou textúrovou súradnicou (rovnaká cesta ako NV34 RAVE),
 * rasterizácia, textúry, z-buffer a blending sú hardvérové (accel/nv34_shared.h).
 */
#ifndef NV34_MINIGL_H
#define NV34_MINIGL_H

#include <Quickdraw.h>
#include "gldefs.h"
#include "../accel/nv34_shared.h"

/* ---- log (minigl.c) ---- */
void   *memset(void *d, int c, unsigned long n);
void   *memcpy(void *d, const void *s, unsigned long n);
void    lg(const char *fmt, ...);
int     first_time(NVU32 key);
extern NV34Shared *S;
NV34Shared *shared(void);

/* ---- hardvér (hw.c) ---- */

typedef unsigned short NVU16;

typedef struct HwTarget {
    NVU32   id, generation;
    int     bpp;                    /* 16 / 32 */
    int     w, h;
    NVU32   colorOff, colorPitch;   /* zadný buffer */
    NVU32   zOff, zPitch;
    int     z16;                    /* q3-10: Z16 namiesto Z24S8 (16 bpp a hra pýta ≤ 16 bitov) */
} HwTarget;

typedef struct HwTex {
    NVU32   magic;                  /* 'NVgt' */
    int     w, h, log2w, log2h, levels;
    NVU32   off, alt, size;         /* VRAM, druhá oblasť pre časté zmeny */
    NVU32   frameUsed;              /* posledná snímka, v ktorej GPU textúru čítal */
    NVU32   lastUse;
    NVU32   altUsed;                /* snímka, v ktorej GPU čítal oblasť alt */
    int     fmt;                    /* NV formát: 0x06 A8R8G8B8, 0x02 A1R5G5B5, 0x04 A4R4G4B4 */
    struct HwTex *next, *prev;
} HwTex;

/* stav, ktorý GL nastaví pred kreslením */
typedef struct HwState {
    int     blend;  GLenum blendSrc, blendDst;
    int     alphaTest; GLenum alphaFunc; float alphaRef;
    int     depthTest; GLenum depthFunc; int depthWrite;
    NVU32   colorMask;              /* 0x01010101 = všetko */
    int     scissor; int sx, sy, sw, sh;   /* v súradniciach RT (y zhora) */
    HwTex  *tex;                    /* NULL = bez textúry */
    int     texEnv;                 /* GL_MODULATE / GL_REPLACE / … */
    GLenum  minFilter, magFilter, wrapS, wrapT;
    HwTex  *tex1;                   /* q3-12: jednotka 1 (GL_MODULATE), NULL = len jedna textúra */
    GLenum  minFilter1, magFilter1, wrapS1, wrapT1;
    /* q3-16: HW T&L — vrcholy v súradniciach objektu, GPU ich násobí maticou mat (po riadkoch,
     * MVP × okno), oreže a odstráni odvrátené steny */
    int     tnl;
    const float *mat; NVU32 matGen;
    int     cull; GLenum cullFace, frontFace;
} HwState;

int     hw_init(void);                                  /* 1 = GPU pripravené */
int     hw_target_alloc(HwTarget *t, int w, int h, int bpp, int zbits);
void    hw_target_free(HwTarget *t);
int     hw_bind(HwTarget *t);
int     hw_state(HwTarget *t, const HwState *st);    /* 0 bez textúry, 1 s textúrou, 2 s dvoma */
void    hw_clear(HwTarget *t, int color, int depth, NVU32 argb, NVU32 zval, const HwState *st);  /* zval Z24S8 */
void    hw_kick(void);
int     hw_sync(void);
void    hw_finish(void);
void    hw_pend_flush(void);                                /* glFinish: počkať na predošlú snímku */
int     gl_boot_arg(const char *word);
NVU32   hw_frame(void);

/* vrcholy v súradniciach RT: x, y, z (0..1), farba 0..1, s/q, t/q, q */
/* c = farba R<<24|G<<16|B<<8|A (wolf-3: zabalená raz, hw_draw ju zapisuje priamo) */
/* sq1, tq1, q1 = jednotka 1 (q3-12) */
typedef struct HwVtx { float x, y, z, r, g, b, a, sq, tq, q; NVU32 c; float sq1, tq1, q1; } HwVtx;

static inline NVU32 ubyte_f(float f)
{
    if (!(f > 0.0f))
        return 0;
    if (f >= 1.0f)
        return 255;
    return (NVU32)(f * 255.0f + 0.5f);
}

static inline NVU32 pack_rgba(float r, float g, float b, float a)
{
    return (ubyte_f(r) << 24) | (ubyte_f(g) << 16) | (ubyte_f(b) << 8) | ubyte_f(a);
}
void    hw_draw(int prim, const HwVtx *v, int n, int textured);  /* prim: 1 body, 2 čiary, 5 trojuholníky; textured 0/1/2 */

/* textúra: celý reťazec mipmáp v ARGB32 (úroveň 0 prvá), voliteľne iba úroveň 0 */
HwTex  *hw_tex_new(void);
void    hw_tex_free(HwTex *t);
int     hw_tex_resident(HwTex *t);                      /* 0 = treba nahrať (nová / vyhodená) */
extern HwTex *gTexPin;                                  /* q3-12: túto textúru nevyhadzovať (druhá jednotka sa práve nahráva) */
int     hw_tex_subupload(HwTex *t, int x, int y, int w, int h, const void *src, int stride, int srcfmt);
extern NVU32 gHwUploads, gHwUploadBytes, gHwSubUploads, gHwSyncs;

/* časovanie (opt-2): časová základňa PPC */
static inline NVU32 tb(void) { NVU32 v; __asm__ volatile("mftb %0" : "=r"(v)); return v; }
extern NVU32 gTEnd, gTFlush, gTWait, gTSwap, gTFinish, gFinishN;
int     hw_tex_upload(HwTex *t, int w, int h, int levels, void *const *pix, int fmt, int srcfmt);

/* ARGB32 → 16-bit texel: A1R5G5B5 (0x02) alebo A4R4G4B4 (0x04), natívne BE ako NV34 RAVE */
static inline NVU16 tx16(NVU32 c, int fmt)
{
    if (fmt == 0x04)
        return (NVU16)(((c >> 16) & 0xF000) | ((c >> 12) & 0x0F00) | ((c >> 8) & 0x00F0) | ((c >> 4) & 0x000F));
    return (NVU16)(((c >> 16) & 0x8000) | ((c >> 9) & 0x7C00) | ((c >> 6) & 0x03E0) | ((c >> 3) & 0x001F));
}

/* prenos zadného buffra na obrazovku cez klip (globálne súradnice) */
void    hw_present(HwTarget *t, GDHandle gd, const Rect *dst, RgnHandle clip);

/* ---- GL kontext (minigl.c, agl.c) ---- */

#define MAX_LEVELS  12
#define STACK_DEPTH 32
#define VB_MAX      1024

typedef struct TexObj {
    GLuint  name;
    int     w[MAX_LEVELS], h[MAX_LEVELS];
    void   *lv[MAX_LEVELS];                 /* ARGB32 (0xAARRGGBB) alebo 16-bit (sfmt) */
    int     sfmt[MAX_LEVELS];               /* wolf-2: formát kópie v RAM: 0x06 (4 B), 0x02 / 0x04 (2 B) */
    int     hasAlpha;
    int     dirty;
    HwTex  *hw;
    GLenum  minFilter, magFilter, wrapS, wrapT;
    struct TexObj *next;                    /* hash */
} TexObj;

/* vrchol v clip súradniciach */
typedef struct V { float x, y, z, w, r, g, b, a, s, t, q, s1, t1, q1; } V;   /* s1… jednotka 1 */
#define V_FLOATS 14

typedef struct Arr { int on, size; GLenum type; int stride; const void *ptr; } Arr;

/* stav, ktorý ukladá glPushAttrib */
typedef struct GLS {
    int     blend, alphaTest, depthTest, cull, scissorTest, tex2D, fog, dither;
    GLenum  blendSrc, blendDst, alphaFunc, depthFunc, cullFace, frontFace, shade, texEnv;
    float   alphaRef;
    int     depthMask;
    GLboolean cmask[4];
    int     scissor[4];
    int     viewport[4];
    double  depthNear, depthFar;
    float   clearColor[4];
    double  clearDepth;
    GLenum  polyMode;
    int     polyOffsetFill;
    float   polyFactor, polyUnits;
} GLS;

typedef struct GLCtx {
    NVU32   magic;                          /* 'NVgc' */
    /* AGL */
    int     dbl, depthBits;
    CGrafPtr port;
    HwTarget rt;
    int     hasRT, viewportSet;
    int     full;                           /* okno pokrýva celú obrazovku (hl-11) */
    GDHandle gd;
    Rect    global;                         /* drawable v globálnych súradniciach */
    RgnHandle rgn;
    long    swapRect[4]; int swapRectOn;
    long    frames;
    /* GL */
    GLenum  err;
    GLS     s;
    GLS     attrStack[8]; int attrSp;
    float   mstack[3][STACK_DEPTH][16];
    int     msp[3];
    GLenum  matrixMode;
    float   mvp[16]; int mvpDirty, texIdentity;
    float   cur[4], curTex[4];              /* farba, s t r q */
    TexObj *bound, def;
    /* q3-4: textúrová jednotka 1; q3-12: kreslí sa s ňou (GL_ARB_multitexture, len GL_MODULATE) */
    int     unit, cunit, tex2D1;
    TexObj *bound1;
    GLenum  texEnv1;
    Arr     ta1;
    float   curTex1[4];
    int     unpackAlign, unpackRowLength, packAlign;
    Arr     va, ca, ta, na;
    /* glBegin/glEnd */
    int     inBegin; GLenum prim;
    V       vb[VB_MAX]; int nv; int stripOdd; int loopFirstValid; V loopFirst;
} GLCtx;

extern GLCtx *C;

/* minigl.c pre agl.c */
void    gl_ctx_init(GLCtx *c);
void    gl_flush_batch(void);
NVU32   vram_total(void);
void    gl_frame_stats(void);
void    gl_stats_init(void);
extern int gDbg;                            /* hl-3: podrobný log (Half-Life) */
void    gl_release_textures(void);
NVU32   gl_cpu_mhz(void);                   /* q3-15: zmeraný takt CPU */
void    gl_mem_report(void);           /* pri ukončení: VRAM späť do zdieľanej arény */

#endif

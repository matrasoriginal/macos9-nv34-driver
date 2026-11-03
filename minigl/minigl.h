/*
 * NV34 MiniGL — custom OpenGLLibrary (OpenGL 1.1 + AGL) for the GeForce FX Go5200 on Mac OS 9.
 *
 * The library has the same name as Apple's "OpenGLLibrary" and exports all 474 of its symbols.
 * Placed in a game folder, CFM finds it before the system one (for that game only).
 * Transformation, near-plane clipping and projection run on the CPU (or on the GPU with HW T&L); the GPU gets
 * vertices in window coordinates with a projective texture coordinate (same path as NV34 RAVE);
 * rasterization, textures, z-buffer and blending are done in hardware (accel/nv34_shared.h).
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

/* ---- hardware (hw.c) ---- */

typedef unsigned short NVU16;

typedef struct HwTarget {
    NVU32   id, generation;
    int     bpp;                    /* 16 / 32 */
    int     w, h;
    NVU32   colorOff, colorPitch;   /* back buffer */
    NVU32   zOff, zPitch;
    int     z16;                    /* q3-10: Z16 instead of Z24S8 (16 bpp and the game asks for ≤ 16 bits) */
} HwTarget;

typedef struct HwTex {
    NVU32   magic;                  /* 'NVgt' */
    int     w, h, log2w, log2h, levels;
    NVU32   off, alt, size;         /* VRAM, second area for frequent changes */
    NVU32   frameUsed;              /* last frame in which the GPU read the texture */
    NVU32   lastUse;
    NVU32   altUsed;                /* frame in which the GPU read the alt area */
    int     fmt;                    /* NV format: 0x06 A8R8G8B8, 0x02 A1R5G5B5, 0x04 A4R4G4B4 */
    struct HwTex *next, *prev;
} HwTex;

/* max-4: texture unit environment (glTexEnv, including GL_COMBINE from ARB_texture_env_combine) */
typedef struct HwEnvUnit {
    GLenum  mode;                   /* GL_MODULATE, GL_REPLACE, GL_DECAL, GL_BLEND, GL_ADD, GL_COMBINE */
    GLenum  comb[2];                /* COMBINE_RGB, COMBINE_ALPHA */
    GLenum  src[2][3], op[2][3];    /* [0] RGB, [1] alpha: SOURCEn, OPERANDn */
    float   scale[2];               /* RGB_SCALE, ALPHA_SCALE */
    float   color[4];               /* GL_TEXTURE_ENV_COLOR */
} HwEnvUnit;

/* state GL sets up before drawing */
typedef struct HwState {
    int     blend;  GLenum blendSrc, blendDst;
    int     alphaTest; GLenum alphaFunc; float alphaRef;
    int     depthTest; GLenum depthFunc; int depthWrite;
    NVU32   colorMask;              /* 0x01010101 = everything */
    int     scissor; int sx, sy, sw, sh;   /* in RT coordinates (y from the top) */
    HwTex  *tex;                    /* NULL = no texture */
    int     texEnv;                 /* GL_MODULATE / GL_REPLACE / … */
    GLenum  minFilter, magFilter, wrapS, wrapT;
    HwTex  *tex1;                   /* q3-12: unit 1 (GL_MODULATE), NULL = single texture only */
    GLenum  minFilter1, magFilter1, wrapS1, wrapT1;
    /* q3-16: HW T&L — vertices in object coordinates, the GPU multiplies them by the matrix mat (row-major,
     * MVP × window), clips them and removes back faces */
    int     tnl;
    const float *mat; NVU32 matGen;
    int     cull; GLenum cullFace, frontFace;
    /* fog (GL_FOG): fogMode GL_LINEAR/GL_EXP/GL_EXP2, fogK = GPU equation coefficients (like nouveau
     * nv10_get_fog_coeff), fogColor R | G<<8 | B<<16 | A<<24. With T&L and fog the GPU also gets mv =
     * MODELVIEW (row-major) — only for the eye-space distance, the position still comes from mat (wolf-8) */
    int     fog; GLenum fogMode; float fogK[2]; NVU32 fogColor;
    const float *mv;
    /* max-4: NULL = fixed combiners (textures × primary color), otherwise a fragment program from env[0..1] */
    const HwEnvUnit *env;
} HwState;

int     hw_init(void);                                  /* 1 = GPU ready */
int     hw_target_alloc(HwTarget *t, int w, int h, int bpp, int zbits);
void    hw_target_free(HwTarget *t);
int     hw_bind(HwTarget *t);
int     hw_state(HwTarget *t, const HwState *st);    /* 0 no texture, 1 with a texture, 2 with two */
void    hw_clear(HwTarget *t, int color, int depth, NVU32 argb, NVU32 zval, const HwState *st);  /* zval Z24S8 */
void    hw_kick(void);
int     hw_sync(void);
extern NVU32 gFpgEvict, gFpgNew;                        /* max-6: texenv programs — waits, new ones */
void    hw_finish(void);
void    hw_pend_flush(void);                                /* glFinish: wait for the previous frame */
int     gl_boot_arg(const char *word);
void    gl_cfg_load(const FSSpec *lib);                 /* cfg-1: "NV34 MiniGL config" next to the library */
NVU32   hw_frame(void);

/* vertices in RT coordinates: x, y, z (0..1), color 0..1, s/q, t/q, q */
/* c = color R<<24|G<<16|B<<8|A (wolf-3: packed once, hw_draw writes it directly) */
/* sq1, tq1, q1 = unit 1 (q3-12) */
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
void    hw_draw(int prim, const HwVtx *v, int n, int textured);  /* prim: 1 points, 2 lines, 5 triangles; textured 0/1/2 */

/* q3-18: vertices in VRAM (T&L only, shorter format) + indices */
int     hw_vb_ready(void);
volatile NVU32 *hw_vb_begin(NVU32 bytes);       /* space to write into, 0 = not possible */
NVU32   hw_vb_end(NVU32 used);                  /* returns the offset in VRAM */
void    hw_vb_put(volatile NVU32 *d, const HwVtx *v, int textured);
void    hw_vb_free(void);
void    hw_draw_indexed(int prim, NVU32 vbOff, int textured, const NVU16 *idx, int n);
extern NVU32 gHwVbWaits;
int     hw_vb_batch(void);
int     hw_gather_ok(void);
void    hw_draw_gather(int prim, const HwVtx *base, const NVU16 *idx, int n, int textured);

/* texture: the whole mipmap chain in ARGB32 (level 0 first), optionally only level 0 */
HwTex  *hw_tex_new(void);
void    hw_tex_free(HwTex *t);
int     hw_tex_resident(HwTex *t);                      /* 0 = needs uploading (new / evicted) */
extern HwTex *gTexPin;                                  /* q3-12: do not evict this texture (the second unit is being uploaded) */
int     hw_tex_subupload(HwTex *t, int x, int y, int w, int h, const void *src, int stride, int srcfmt);
extern NVU32 gHwUploads, gHwUploadBytes, gHwSubUploads, gHwSyncs;
extern NVU32 gTUpl, gTEvict, gFrUplN, gFrUplB;

/* timing (opt-2): PPC time base */
static inline NVU32 tb(void) { NVU32 v; __asm__ volatile("mftb %0" : "=r"(v)); return v; }
extern NVU32 gTEnd, gTFlush, gTWait, gTSwap, gTFinish, gFinishN;
extern NVU32 gTFps, gFpsLate, gFpsReset;      /* fps limit (agl.c) */
int     hw_tex_upload(HwTex *t, int w, int h, int levels, void *const *pix, int fmt, int srcfmt);

/* ARGB32 → 16-bit texel: A1R5G5B5 (0x02) or A4R4G4B4 (0x04), native BE like NV34 RAVE */
static inline NVU16 tx16(NVU32 c, int fmt)
{
    if (fmt == 0x04)
        return (NVU16)(((c >> 16) & 0xF000) | ((c >> 12) & 0x0F00) | ((c >> 8) & 0x00F0) | ((c >> 4) & 0x000F));
    return (NVU16)(((c >> 16) & 0x8000) | ((c >> 9) & 0x7C00) | ((c >> 6) & 0x03E0) | ((c >> 3) & 0x001F));
}

/* back buffer transfer to the screen through the clip (global coordinates) */
void    hw_present(HwTarget *t, GDHandle gd, const Rect *dst, RgnHandle clip);

/* ---- GL context (minigl.c, agl.c) ---- */

#define MAX_LEVELS  12
#define STACK_DEPTH 32
#define VB_MAX      1024

typedef struct TexObj {
    GLuint  name;
    int     w[MAX_LEVELS], h[MAX_LEVELS];
    void   *lv[MAX_LEVELS];                 /* ARGB32 (0xAARRGGBB) or 16-bit (sfmt) */
    int     sfmt[MAX_LEVELS];               /* wolf-2: format of the RAM copy: 0x06 (4 B), 0x02 / 0x04 (2 B) */
    int     hasAlpha;
    int     dirty;
    HwTex  *hw;
    GLenum  minFilter, magFilter, wrapS, wrapT;
    struct TexObj *next;                    /* hash */
} TexObj;

/* vertex in clip coordinates */
typedef struct V { float x, y, z, w, r, g, b, a, s, t, q, s1, t1, q1; } V;   /* s1… unit 1 */
#define V_FLOATS 14

typedef struct Arr { int on, size; GLenum type; int stride; const void *ptr; } Arr;

/* state saved by glPushAttrib */
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
    GLenum  fogMode;
    float   fogDensity, fogStart, fogEnd, fogColor[4];
} GLS;

typedef struct GLCtx {
    NVU32   magic;                          /* 'NVgc' */
    /* AGL */
    int     dbl, depthBits;
    CGrafPtr port;
    HwTarget rt;
    int     hasRT, viewportSet;
    int     full;                           /* the window covers the whole screen (hl-11) */
    GDHandle gd;
    Rect    global;                         /* drawable in global coordinates */
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
    float   cur[4], curTex[4];              /* color, s t r q */
    TexObj *bound, def;
    /* q3-4: texture unit 1; q3-12: drawn with it (GL_ARB_multitexture, GL_MODULATE only) */
    int     unit, cunit, tex2D1;
    TexObj *bound1;
    HwEnvUnit env[2];               /* max-4: glTexEnv of units 0 and 1 */
    Arr     ta1;
    float   curTex1[4];
    int     unpackAlign, unpackRowLength, packAlign;
    Arr     va, ca, ta, na;
    /* glBegin/glEnd */
    int     inBegin; GLenum prim;
    V       vb[VB_MAX]; int nv; int stripOdd; int loopFirstValid; V loopFirst;
} GLCtx;

extern GLCtx *C;

/* minigl.c for agl.c */
void    gl_ctx_init(GLCtx *c);
void    gl_flush_batch(void);
NVU32   vram_total(void);
void    gl_frame_stats(void);
void    gl_draw_fps(void);                          /* fps-4: counter in the corner (nv34glshowfps) */
void    gl_stats_init(void);
extern int gDbg;                            /* hl-3: verbose log (Half-Life) */
void    gl_release_textures(void);
NVU32   gl_cpu_mhz(void);                   /* q3-15: measured CPU clock */
void    gl_mem_report(void);           /* at exit: VRAM back to the shared arena */

#endif

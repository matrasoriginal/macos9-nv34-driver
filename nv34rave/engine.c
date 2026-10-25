/*
 * NV34 RAVE — QuickDraw 3D RAVE 1.6 drawing engine pre GeForce FX Go5200 (NV34).
 *
 * Zdieľaná knižnica (typ 'shlb', creator 'tnsl' ako ATI enginy) v Extensions. RAVE manager ju
 * načíta a CFM init rutina TtRegister zaregistruje engine (QARegisterEngine).
 * GPU vlastní NV34 QD Accel (INIT): engine si od neho cez Name Registry property
 * "NV34ACCEL,shared" vezme push buffer, 3D stav, alokátor VRAM a log (accel/nv34_shared.h).
 *
 * Kreslenie: fixed-function vrcholy v pixeloch (matice identita), farba cez register combiners,
 * textúry cez fragment program TXPH s projektívnou súradnicou (u/w, v/w, 0, 1/w) — perspektívne
 * správne. Kontext s DoubleBuffer kreslí do zadného buffra vo VRAM pod viditeľnou plochou
 * (rovnaký pitch ako obrazovka), RenderEnd ho skopíruje 2D blitom cez klip región.
 *
 * Etapa R1: Gouraud + textúry (ARGB32/RGB32/ARGB16/RGB16/CL8 → A8R8G8B8 swizzled, mipmapy),
 * z-buffer, blending, bitmapy, body/čiary, trimesh, notice metódy, log neznámych tagov.
 */
#include <Quickdraw.h>
#include <MacMemory.h>
#include <Processes.h>
#include <Files.h>
#include <stdarg.h>

#include "rave.h"
#include "../accel/nv34_shared.h"

typedef SInt32 OSStatus;
typedef struct RegEntryID { UInt32 contents[4]; } RegEntryID;
extern OSStatus RegistryCStrEntryLookup(const RegEntryID *start, const char *path, RegEntryID *found);
extern OSStatus RegistryPropertyGet(const RegEntryID *e, const char *name, void *value, UInt32 *size);
extern OSStatus RegistryEntryIDDispose(RegEntryID *id);

#define ENGINE_NAME     "NV34 RAVE Engine"
#define VENDOR_ID       0x4E563334          /* 'NV34' */
#define ENGINE_ID       1
#define REVISION        1

/* metódy NV30_3D (nv30-40_3d.xml.h, MIT) */
#define SUBC                7
#define M_RT_HORIZ          0x0200
#define M_RT_ENABLE         0x0220
#define M_TEX_UNITS_ENABLE  0x023c
#define M_TEX_MATRIX_ENABLE 0x0240
#define M_VIEWPORT_CLIP_H   0x02c0
#define M_ALPHA_FUNC_ENABLE 0x0304
#define M_BLEND_FUNC_ENABLE 0x0310
#define M_COLOR_MASK        0x0324
#define M_LINE_WIDTH        0x03b8
#define M_SCISSOR_HORIZ     0x08c0
#define M_FP_ACTIVE_PROGRAM 0x08e4
#define M_RC_FINAL0         0x08f4
#define M_RC_IN_ALPHA0      0x0900
#define M_VIEWPORT_HORIZ    0x0a00
#define M_VIEWPORT_TRANSLATE_X 0x0a20
#define M_DEPTH_FUNC        0x0a6c
#define M_FP_REG_CONTROL    0x1450
#define M_VERTEX_BEGIN_END  0x1808
#define M_CULL_FACE_ENABLE  0x183c
#define M_TEX_OFFSET0       0x1a00
#define M_VTX_ATTR_4F(i)    (0x1c00 + (i) * 16)
#define M_FP_CONTROL        0x1d60
#define M_CLEAR_DEPTH_VALUE 0x1d8c
#define M_CLEAR_COLOR_VALUE 0x1d90
#define M_CLEAR_BUFFERS     0x1d94

#define PRIM_POINTS     1
#define PRIM_LINES      2
#define PRIM_LINE_STRIP 4
#define PRIM_TRIANGLES  5
#define PRIM_TRI_STRIP  6
#define PRIM_TRI_FAN    7

/* fragment program TXPH H0, a[tex0], t[0]; END (xf86-video-nouveau), kódovanie pre BE objekt */
static const NVU32 kFpTex[4] = { 0x18009e81, 0x1c9dc801, 0x0001c800, 0x3fe1c800 };

static const float kOne = 1.0f;

/* ---- bez libc ------------------------------------------------------------ */

void *memset(void *d, int c, unsigned long n)
{
    unsigned char *p = d;
    while (n--)
        *p++ = (unsigned char)c;
    return d;
}

/* opt-4: po slovách, ak sú zarovnané (kompilátor ním kopíruje štruktúry) */
void *memcpy(void *d, const void *s, unsigned long n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    if (!(((unsigned long)p | (unsigned long)q) & 3)) {
        unsigned long *pw = (unsigned long *)p;
        const unsigned long *qw = (const unsigned long *)q;
        while (n >= 4) {
            *pw++ = *qw++;
            n -= 4;
        }
        p = (unsigned char *)pw;
        q = (const unsigned char *)qw;
    }
    while (n--)
        *p++ = *q++;
    return d;
}

static unsigned long slen(const char *s)
{
    unsigned long n = 0;
    while (s[n])
        n++;
    return n;
}

static NVU32 fbits(float f)
{
    union { float f; NVU32 u; } c;
    c.f = f;
    return c.u;
}

static NVU32 ubyte(float f)
{
    if (!(f > 0.0f))
        return 0;
    if (f >= 1.0f)
        return 255;
    return (NVU32)(f * 255.0f + 0.5f);
}

/* ---- zdieľaný stav a log ---------------------------------------------------- */

static NV34Shared *S;
static int gZ16;                    /* boot-args nv34z16: zeta Z16 pri 16 bpp (ako rave-15) */
static int gSpoofATI;               /* hlásiť vendor ATI (UT, alebo boot-args nv34raveati) */
static int gAtiGL;                  /* gl-q2: navyše ATI gestalty a BoundToDevice pre Apple GL (Quake) */
static int gNo2x;                   /* boot-args nv34no2x: tag 1020 ignorovať */

static NV34Shared *shared(void)
{
    RegEntryID e;
    UInt32 addr = 0, size = 4;

    if (S)
        return S;
    if (RegistryCStrEntryLookup(0, "Devices:device-tree:pci:NVDA,Parent:NVDA,Display-A", &e) != 0)
        return 0;
    if (RegistryPropertyGet(&e, "NV34ACCEL,shared", &addr, &size) == 0 && size == 4 && addr) {
        NV34Shared *s = (NV34Shared *)addr;
        if (s->magic == NV34_SHARED_MAGIC && s->version == NV34_SHARED_VERSION)
            S = s;
    }
    RegistryEntryIDDispose(&e);
    return S;
}

static void log_put(const char *s, unsigned long n)
{
    while (n--) {
        S->log[S->logPos % S->logSize] = *s++;
        S->logPos++;
    }
}

static void fmt_num(char **o, NVU32 v, int base, int width, int neg)
{
    char pad = base == 16 || width < 0 ? '0' : ' ';
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    if (width < 0)
        width = -width;
    if (neg)
        *(*o)++ = '-';
    while (width-- > n)
        *(*o)++ = pad;
    while (n)
        *(*o)++ = tmp[--n];
}

/* gl-q2-5: pri ATI procese aj priebežne do súboru (Systémová zložka:Preferences:NV34 RAVE live.txt),
 * aby log prežil zamrznutie a reštart. Každý riadok: otvoriť, pripísať, zavrieť, FlushVol. */
static int gFileLog, gFileLines, gFileFresh = 1;

static void file_put(const char *s, long n)
{
    short vref, ref;
    long dir;
    FSSpec spec;
    OSErr e;

    if (!gFileLog || gFileLines >= 3000)
        return;
    gFileLines++;
    if (FindFolder(-32768, kPreferencesFolderType, true, &vref, &dir) != noErr)
        return;
    e = FSMakeFSSpec(vref, dir, "\pNV34 RAVE live.txt", &spec);
    if (e == fnfErr)
        FSpCreate(&spec, 'ttxt', 'TEXT', 0);
    if (FSpOpenDF(&spec, fsRdWrPerm, &ref) != noErr)
        return;
    if (gFileFresh) {                               /* minigl-11: každý proces začína prázdnym logom */
        SetEOF(ref, 0);
        gFileFresh = 0;
    }
    SetFPos(ref, fsFromLEOF, 0);
    FSWrite(ref, &n, s);
    FSClose(ref);
    FlushVol(0, vref);
}

static void lg(const char *fmt, ...);
static int gCallLogs;
/* priebeh volaní v ATI procese (prvých 600), na hľadanie zamrznutia */
#define CL(...) do { if (gFileLog && gCallLogs < 600) { gCallLogs++; lg(__VA_ARGS__); } } while (0)

/* %s %d %u %x %c, šírka (napr. %08x), %f = float s 3 desatinnými miestami */
static void lg(const char *fmt, ...)
{
    char buf[256], *o = buf, *end = buf + sizeof(buf) - 24;
    va_list ap;

    if (!shared() || !S->log)
        return;
    va_start(ap, fmt);
    while (*fmt && o < end) {
        int width = 0;
        if (*fmt != '%') {
            *o++ = *fmt++;
            continue;
        }
        fmt++;
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        switch (*fmt++) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            while (*s && o < end)
                *o++ = *s++;
            break;
        }
        case 'd': {
            long v = va_arg(ap, long);
            fmt_num(&o, (NVU32)(v < 0 ? -v : v), 10, width, v < 0);
            break;
        }
        case 'u':
            fmt_num(&o, va_arg(ap, unsigned long), 10, width, 0);
            break;
        case 'x':
            fmt_num(&o, va_arg(ap, unsigned long), 16, width, 0);
            break;
        case 'c':
            *o++ = (char)va_arg(ap, int);
            break;
        case '%':
            *o++ = '%';
            break;
        case 'f': {
            double d = va_arg(ap, double);
            long m = (long)(d * 1000.0 + (d < 0 ? -0.5 : 0.5));
            NVU32 a = (NVU32)(m < 0 ? -m : m);
            fmt_num(&o, a / 1000, 10, 0, m < 0);
            *o++ = '.';
            fmt_num(&o, a % 1000, 10, -3, 0);
            break;
        }
        default:
            *o++ = '?';
        }
    }
    va_end(ap);
    *o++ = '\r';
    log_put(buf, o - buf);
    file_put(buf, o - buf);
}

/* prvý výskyt každého neznámeho tagu / gestaltu (aby log nezahltil) */
#define SEEN_MAX 256
static NVU32 gSeen[SEEN_MAX];
static int gSeenCount;

static int first_time(NVU32 key)
{
    int i;
    for (i = 0; i < gSeenCount; i++)
        if (gSeen[i] == key)
            return 0;
    if (gSeenCount < SEEN_MAX)
        gSeen[gSeenCount++] = key;
    return 1;
}

/* ---- push buffer ---------------------------------------------------------- */

static void begin(NVU32 mthd, NVU32 n)
{
    nva_begin(S->a, SUBC, mthd, n);
}

static void data(NVU32 v)
{
    nva_push(S->a, v);
}

static void dataf(float f)
{
    nva_push(S->a, fbits(f));
}

static void kick(void)
{
    nva_kickoff(S->a);
}

static int gpu_sync(void)
{
    return nva_sync(S->a, 120) == kNVAOK;
}

/* ---- objekty enginu ------------------------------------------------------- */

#define STATE_MAX 160

struct TQATexture {
    NVU32   magic;                  /* 'NVtx' */
    NVU32   off, size;              /* VRAM */
    NVU32   w, h, log2w, log2h, levels;
    NVU32   rect;                   /* 1 = lineárna RECT (bitmapa), pitch */
    NVU32   fmt, bpp;               /* NV formát (0x06 A8R8G8B8, 0x02 A1R5G5B5, 0x04 A4R4G4B4), bajty/texel */
    NVU32   pitch;
    long    pixelType;
    unsigned long flags;
    TQAColorTable *clut;
    TQAImage src[12];               /* pre CL4/CL8: zdroj na nové nahratie po bind CLUT */
    /* rave-5: UT minul VRAM → kópia v RAM (už v NV formáte), VRAM len pre používané (LRU) */
    void   *sys;
    NVU32   lastUse;
    NVU32   alpha0;                 /* % texelov úrovne 0 s alfou 0 (diagnostika) */
    NVU32   alt;                    /* druhá oblasť VRAM pre často menené textúry (ATI[2]) */
    struct TQATexture *next, *prev;
};

static struct TQATexture *gTexList;
static NVU32 gFrame = 1, gTexEpoch = 1, gEvictions, gEvictLogged;

struct TQAColorTable {
    NVU32   argb[256];
    long    transparent;
};

typedef struct Ctx {
    TQADrawContext *dc;
    NVU32   id;
    unsigned long flags;
    GDHandle gd;
    TQARect rect;                   /* zariadenie (globálne súradnice obrazovky) */
    RgnHandle clip;
    long    width, height;
    int     dbl, hasZ;
    NVU32   backOff, backPitch;     /* zadný buffer: offset vo VRAM, pitch = šírka kontextu */
    int     bpp;                    /* 16 (X1R5G5B5) alebo 32 (A8R8G8B8) */
    int     z24;                    /* zeta Z24S8 (aj pri 16 bpp zadnom buffri, rave-16) */
    NVU32   zOff, zPitch;
    NVU32   fbOff, pitch;           /* obrazovka pri poslednom bind */
    NVU32   generation;
    unsigned long si[STATE_MAX];
    float   sf[STATE_MAX];
    TQATexture *tex;
    /* platný stav v GPU (-1 = neznámy) */
    long    hwMode, hwBlend, hwZ, hwTex, hwFilter, hwWrap, hwFmt;
    int     flat2D;                 /* práve kreslený primitív je 2D (z = 0, invW = 1) */
    int     atiZWrite;              /* súkromný ATI tag 1022: zápis hĺbky (UT: 0 pre oblohu, text) */
    int     ati2x;                  /* súkromný ATI tag 1020: modulácia 2× (UT nastaví 1) */
    NVU32   hwEpoch;
    /* trimesh */
    const TQAVGouraud *vg;
    unsigned long nvg;
    const TQAVTexture *vt;
    unsigned long nvt;
    TQANoticeMethod notice[5];
    void   *noticeRef[5];
    /* štatistika */
    NVU32   tris, frames;
} Ctx;

#define CTX(dc) ((Ctx *)(dc)->drawPrivate)

/* ---- väzba kontextu na GPU ---------------------------------------------------- */

static void screen_geom(Ctx *c)
{
    PixMapPtr pm = *(*c->gd)->gdPMap;
    c->pitch = pm->rowBytes & 0x3fff;
    c->fbOff = (NVU32)((volatile NVU8 *)pm->baseAddr - S->a->vram);
}

static void invalidate(Ctx *c)
{
    c->hwMode = c->hwBlend = c->hwZ = c->hwTex = c->hwFilter = c->hwWrap = c->hwFmt = -1;
}

/* Render target + viewport pre kontext; volať pred každým kreslením. */
static int bind(Ctx *c)
{
    NVU32 w, h, off, zoff, zpitch, cpitch, tx, ty;
    int fresh;

    if (!S || S->state3D != 1)
        return 0;
    if (S->boundCtxId == c->id && S->generation == c->generation)
        return 1;
    screen_geom(c);
    if (c->dbl) {
        w = c->width;
        h = c->height;
        off = c->backOff;
        tx = ty = 0;
    } else {
        PixMapPtr pm = *(*c->gd)->gdPMap;
        w = pm->bounds.right - pm->bounds.left;
        h = pm->bounds.bottom - pm->bounds.top;
        off = c->fbOff;
        tx = c->rect.left - pm->bounds.left;
        ty = c->rect.top - pm->bounds.top;
    }
    zoff = c->hasZ ? c->zOff : off;                 /* bez z: zeta sa nepoužije */
    zpitch = c->hasZ ? c->zPitch : c->pitch;
    cpitch = c->dbl ? c->backPitch : c->pitch;

    fresh = S->generation != c->generation;
    if (fresh) {                                    /* nový 3D init alebo nový kontext: FP znova */
        nv3d_upload_fp(S->g, kFpTex, 4, 1);
        c->generation = S->generation;
    }
    begin(M_RT_HORIZ, 5);
    data(w << 16);
    data(h << 16);
    /* rave-2: farba aj zeta musia mať rovnaký pitch (iný pitch farby → rozsekané riadky) */
    data((c->bpp == 16 ? 0x102 : 0x108) | (c->z24 ? 0x40 : 0x20));   /* farba | Z24S8/Z16 | linear */
    data((zpitch << 16) | cpitch);
    data(off);
    begin(0x0214, 1);                               /* ZETA_OFFSET */
    data(zoff);
    begin(M_VIEWPORT_HORIZ, 2);
    data(w << 16);
    data(h << 16);
    begin(M_VIEWPORT_CLIP_H, 2);
    data((w - 1) << 16);
    data((h - 1) << 16);
    begin(M_SCISSOR_HORIZ, 2);                      /* kontext (pri kreslení na obrazovku) */
    data((c->width << 16) | tx);
    data((c->height << 16) | ty);
    begin(M_VIEWPORT_TRANSLATE_X, 8);
    dataf((float)tx);
    dataf((float)ty);
    dataf(0.0f);
    dataf(0.0f);
    dataf(1.0f);
    dataf(1.0f);
    dataf(1.0f);                                    /* hĺbka 0..1 → hardvér (gl-7) */
    dataf(0.0f);
    begin(M_FP_ACTIVE_PROGRAM, 1);
    data(S->g->fpOffset | 1);
    begin(M_FP_CONTROL, 1);
    data(0);
    begin(M_FP_REG_CONTROL, 1);
    data(0x0001000f);
    begin(M_TEX_MATRIX_ENABLE, 1);
    data(0);
    begin(M_CULL_FACE_ENABLE, 1);
    data(0);
    begin(M_COLOR_MASK, 1);
    data(0x01010101);
    begin(0x1d78, 1);                               /* DEPTH_CONTROL: clamp namiesto orezania */
    data(0x00000010);
    begin(0x0300, 1);                               /* DITHER_ENABLE */
    data(c->bpp == 16 && !(c->flags & kQAContext_NoDither));
    if (fresh && c->hasZ) {
        /* gl-7, gl-8, rave-3: prvý CLEAR hĺbky po inite/novom RT nezaberie → naprázdno */
        begin(0x0a70, 1);
        data(1);
        begin(M_CLEAR_DEPTH_VALUE, 1);
        data(0xffffffff);
        begin(M_CLEAR_BUFFERS, 1);
        data(0x03);
    }
    S->boundCtxId = c->id;
    invalidate(c);
    return 1;
}

/* ---- stav kreslenia ----------------------------------------------------------- */

enum { MODE_GOURAUD = 0, MODE_TEXTURE = 1 };

static NVU32 gl_blend(unsigned long v)
{
    /* RAVE kQAGL_ hodnoty = GL enumy (GL_ZERO 0, GL_ONE 1, 0x300…) */
    return v;
}

static void set_mode(Ctx *c, long mode)
{
    if (c->hwMode == mode)
        return;
    if (mode == MODE_GOURAUD) {
        begin(M_TEX_UNITS_ENABLE, 1);
        data(0);
        begin(M_RC_FINAL0, 3);
        data(0x00000004);                           /* D = primárna farba */
        data(0x00001400);                           /* G = primárna alfa */
        data(0x01000101);
    } else {
        begin(M_TEX_UNITS_ENABLE, 1);
        data(1);
        /* stupeň 0: tex0 (H0, zdroj 0x08) × primárna farba → spare0 */
        begin(M_RC_IN_ALPHA0, 6);
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
    c->hwMode = mode;
}

static void set_blend(Ctx *c)
{
    long b = (long)c->si[kQATag_Blend];
    NVU32 src, dst;
    long key;

    if (b == kQABlend_OpenGL) {
        src = gl_blend(c->si[kQATagGL_BlendSrc]);
        dst = gl_blend(c->si[kQATagGL_BlendDst]);
        /* rave-22: UT (ATI tag 1020 = 1) svetelné mapy a „modulated“ povrchy sú v polovičnom jase,
         * DST_COLOR × ZERO má byť 2× (ako OpenGLDrv: DST_COLOR × SRC_COLOR) — inak tmavé steny */
        if (c->ati2x && src == 0x0306 && dst == 0)
            dst = 0x0300;
    } else if (b == kQABlend_Interpolate) {
        src = 0x0302;                               /* SRC_ALPHA */
        dst = 0x0303;                               /* ONE_MINUS_SRC_ALPHA */
    } else {
        src = 1;                                    /* PreMultiply: ONE */
        dst = 0x0303;
    }
    key = (long)(((NVU32)(b == kQABlend_OpenGL) << 28) | (src << 16) | dst);
    if (c->hwBlend == key)
        return;
    if (first_time(0xF0000000 | (NVU32)key))
        lg("blend %d: src %x dst %x", b, src, dst);
    /* rave-8: plne priehľadné texely (maskované textúry UT, alfa 0) nesmú zapísať hĺbku — inak
     * 2D panel menu s priehľadným oknom zakryje model hráča v z-bufferi. ATI zrejme tieto
     * pixely zahadzuje; pri RAVE miešaní (nie OpenGL) je to bez vplyvu na farbu. */
    begin(M_ALPHA_FUNC_ENABLE, 3);
    /* rave-21: aj OpenGL miešanie so SRC_ALPHA (UT maskované textúry: mreže, rastliny) —
     * inak diery zapíšu hĺbku a svetelná mapa (ZFunction EQ) ich stmaví / zakryje, čo je za nimi */
    data(b != kQABlend_OpenGL || src == 0x0302);
    data(0x0204);                                   /* GREATER */
    data(0);
    if (src == 1 && dst == 0) {
        begin(M_BLEND_FUNC_ENABLE, 1);
        data(0);
    } else {
        begin(M_BLEND_FUNC_ENABLE, 5);
        data(1);
        data((src << 16) | src);
        data((dst << 16) | dst);
        data(0);
        data(0x80068006);                           /* FUNC_ADD */
    }
    c->hwBlend = key;
}

static void set_depth(Ctx *c)
{
    static const NVU32 func[9] = { 0x207, 0x201, 0x202, 0x203, 0x204, 0x205, 0x206, 0x207, 0x200 };
    unsigned long zf = c->si[kQATag_ZFunction];
    long key;
    int test, write;

    if (!c->hasZ)
        zf = kQAZFunction_None;
    if (zf > kQAZFunction_False)
        zf = kQAZFunction_LT;
    test = zf != kQAZFunction_None;
    /* rave-11: 2D prvky UT (z = 0, invW = 1: pozadie menu, HUD) hĺbku nezapisujú — inak pozadie
     * menu zakryje model hráča (UT medzi nimi z-buffer nemaže) */
    write = test && !c->si[kQATag_ZBufferMask] && !c->flat2D && c->atiZWrite;
    key = (long)((zf << 8) | (write << 1) | test);
    if (c->hwZ == key)
        return;
    begin(M_DEPTH_FUNC, 1);
    data(func[zf]);
    begin(0x0a70, 2);                               /* DEPTH_WRITE_ENABLE, DEPTH_TEST_ENABLE */
    data(write);
    data(test);
    c->hwZ = key;
}

static NVU32 tex_filter(Ctx *c, TQATexture *t)
{
    unsigned long f = c->si[kQATag_TextureFilter];
    NVU32 mag, min;

    if (t->rect || f == kQATextureFilter_Fast) {
        mag = 0x01000000;                           /* NEAREST */
        min = t->levels > 1 && !t->rect ? 0x00030000 : 0x00010000;
    } else {
        mag = 0x02000000;                           /* LINEAR */
        if (t->levels <= 1)
            min = 0x00020000;
        else
            min = f == kQATextureFilter_Best ? 0x00060000 : 0x00040000;
    }
    return mag | min | 0x2000;
}

static int tex_resident(TQATexture *t);

static int set_texture(Ctx *c, TQATexture *t)
{
    NVU32 fmt, wrap, en, filt;

    if (!t || t->magic != 'NVtx' || !tex_resident(t))
        return 0;
    t->lastUse = gFrame;
    filt = tex_filter(c, t);
    wrap = t->rect ? 0x030303 :
           ((c->si[kQATagGL_TextureWrapU] == kQAGL_Clamp ? 3u : 1u) |
            ((c->si[kQATagGL_TextureWrapV] == kQAGL_Clamp ? 3u : 1u) << 8) | (1u << 16));
    if (c->hwTex == (long)t && c->hwFilter == (long)filt && c->hwWrap == (long)wrap && c->hwEpoch == gTexEpoch)
        return 1;
    if (t->rect)
        fmt = 0x08 | 0x20 | (0x12u << 8) | (1u << 16);
    else
        fmt = 0x08 | 0x20 | (t->fmt << 8) | (1u << 16) | (t->levels > 1 ? 0x00080000 : 0);
    fmt |= (t->log2w << 20) | (t->log2h << 24) | 0x01;
    en = 0x40000000 | (((t->levels - 1) << 8) << 6);   /* max LOD (8.8) << 6, min LOD 0 */
    begin(M_TEX_OFFSET0, 8);
    data(t->off);
    data(fmt);
    data(wrap);
    data(en);
    data(t->rect ? (t->pitch << 16) | 0xAAE4 : 0xAAE4);
    data(filt);
    data((t->w << 16) | t->h);
    data(0);
    c->hwTex = (long)t;
    c->hwFilter = filt;
    c->hwWrap = wrap;
    c->hwEpoch = gTexEpoch;
    return 1;
}

static void set_vtxfmt(Ctx *c, int tex);

static int prepare(Ctx *c, long mode)
{
    if (!bind(c))
        return 0;
    set_vtxfmt(c, mode == MODE_TEXTURE);           /* podľa typu vrcholov, nie podľa combinerov */
    if (mode == MODE_TEXTURE && (!c->tex || !set_texture(c, c->tex)))
        mode = MODE_GOURAUD;
    set_mode(c, mode);
    set_blend(c);
    set_depth(c);
    return 1;
}

/* ---- vrcholy ------------------------------------------------------------------ */

/* rave-7: UT má z = 1 − 1/w, vzdialené vrcholy z ≥ 1 → hardvér ich orezal (model hráča v menu).
 * Hĺbka sa obmedzí na [0, 1) aj v hardvéri (DEPTH_CONTROL clamp). */
static float zclamp(float z)
{
    /* rave-16: horná hranica len v hardvéri (DEPTH_CONTROL clamp) — softvérové 0,99998 zlialo
     * vzdialené hĺbky UT do jednej hodnoty */
    return z > 0.0f ? z : 0.0f;
}

/* rave-23: vrcholy ako inline pole (VERTEX_DATA, non-increment) — jedna hlavička na dávku
 * namiesto 2–3 hlavičiek na vrchol. Poradie atribútov podľa indexu: pozícia (0), farba (3),
 * textúra (8). boot-args nv34immediate = pôvodné VTX_ATTR metódy.
 * rave-25 (ako MiniGL opt-1 a q3-17): vrchol sa zapisuje priamo do miesta rezervovaného v push
 * buffri (bez volania nva_push na slovo) a je kratší — pozícia x, y, z (w = 1 doplní GPU), farba
 * U8 × 4 v jednom slove (R v najvyššom bajte) → 8 slov s textúrou (predtým 12), 4 bez nej (8).
 * boot-args nv34ravelong = pôvodný 12/8-slovný vrchol. */
static int gImmediate, gLong, gStats;
static NVU32 gVwLeft, gVwHdr, gVwWords, gVwBad;
static NVU32 gVwDummy[12];

#define WPV_G   (gLong ? 8u : 4u)
#define WPV_T   (gLong ? 12u : 8u)

static void vw_start(NVU32 n, NVU32 wpv)
{
    gVwLeft = n;
    gVwHdr = 0;
    gVwWords = wpv;
}

/* miesto pre ďalší vrchol (gVwWords slov) */
static volatile NVU32 *vw_next(void)
{
    volatile NVU32 *p;
    if (!gVwHdr) {
        NVU32 cnt = 2047 / gVwWords;
        if (cnt > gVwLeft)
            cnt = gVwLeft;
        gVwBad = nva_begin(S->a, SUBC, 0x40000000 | 0x1818, cnt * gVwWords) != kNVAOK;
        gVwHdr = cnt;
    }
    gVwHdr--;
    gVwLeft--;
    if (gVwBad)                                     /* GPU neodpovedá: miesto nie je rezervované */
        return gVwDummy;
    p = S->a->dmaBase + S->a->dmaCurrent;
    S->a->dmaCurrent += gVwWords;
    return p;
}

static void set_vtxfmt(Ctx *c, int tex)
{
    NVU32 st;
    int i;
    if (gImmediate || c->hwFmt == tex)
        return;
    begin(0x1740, 16);                              /* VTXFMT(0..15) */
    if (gLong) {
        st = (tex ? 48u : 32u) << 8;
        for (i = 0; i < 16; i++)
            data(i == 0 || i == 3 || (i == 8 && tex) ? (0x2 | (4u << 4) | st) : 0x2);
    } else {
        st = (tex ? 32u : 16u) << 8;
        for (i = 0; i < 16; i++)
            data(i == 0 ? (0x2 | (3u << 4) | st) :           /* V32_FLOAT × 3 */
                 i == 3 ? (0x4 | (4u << 4) | st) :           /* U8_UNORM × 4 */
                 (i == 8 && tex) ? (0x2 | (4u << 4) | st) : 0x2);
    }
    c->hwFmt = tex;
}

static inline NVU32 pack_rgba(float r, float g, float b, float a)
{
    return (ubyte(r) << 24) | (ubyte(g) << 16) | (ubyte(b) << 8) | ubyte(a);
}

static void vtx_g(const TQAVGouraud *v)
{
    if (!gImmediate) {
        volatile NVU32 *p = vw_next();
        p[0] = fbits(v->x);
        p[1] = fbits(v->y);
        p[2] = fbits(zclamp(v->z));
        if (gLong) {
            p[3] = fbits(kOne);
            p[4] = fbits(v->r);
            p[5] = fbits(v->g);
            p[6] = fbits(v->b);
            p[7] = fbits(v->a);
        } else {
            p[3] = pack_rgba(v->r, v->g, v->b, v->a);
        }
        return;
    }
    begin(M_VTX_ATTR_4F(3), 4);
    dataf(v->r);
    dataf(v->g);
    dataf(v->b);
    dataf(v->a);
    begin(M_VTX_ATTR_4F(0), 4);
    dataf(v->x);
    dataf(v->y);
    dataf(zclamp(v->z));
    dataf(kOne);
}

static NVU32 gVtxCount, gVtxLogs;

static void vtx_t(Ctx *c, const TQAVTexture *v)
{
    if (0 && (++gVtxCount & 4095) == 1 && gVtxLogs < 16) {     /* vzorky vrcholov (rave-7), vypnuté */
        gVtxLogs++;
        lg("vrchol: x %f y %f z %f invW %f u/w %f v/w %f a %f zf %u pz %u", (double)v->x, (double)v->y,
           (double)v->z, (double)v->invW, (double)v->uOverW, (double)v->vOverW, (double)v->a,
           c->si[kQATag_ZFunction], c->si[kQATag_PerspectiveZ]);
    }
    unsigned long op = c->si[kQATag_TextureOp];
    if (op & ~kQATextureOp_Modulate && first_time(0xF1000000 | op))
        lg("textureOp %x (Highlight/Decal zatial nie)", op);
    if (!gImmediate) {
        volatile NVU32 *p = vw_next();
        int mod = (op & kQATextureOp_Modulate) != 0;
        p[0] = fbits(v->x);
        p[1] = fbits(v->y);
        p[2] = fbits(zclamp(v->z));
        if (gLong) {
            p[3] = fbits(kOne);
            p[4] = fbits(mod ? v->kd_r : kOne);
            p[5] = fbits(mod ? v->kd_g : kOne);
            p[6] = fbits(mod ? v->kd_b : kOne);
            p[7] = fbits(v->a);
            p += 4;
        } else {
            p[3] = mod ? pack_rgba(v->kd_r, v->kd_g, v->kd_b, v->a) : (0xFFFFFF00u | ubyte(v->a));
        }
        p[4] = fbits(v->uOverW);                    /* projektívna súradnica: TXP delí q */
        p[5] = fbits(v->vOverW);
        p[6] = 0;
        p[7] = fbits(v->invW);
        return;
    }
    begin(M_VTX_ATTR_4F(3), 4);
    if (op & kQATextureOp_Modulate) {
        dataf(v->kd_r);
        dataf(v->kd_g);
        dataf(v->kd_b);
    } else {
        dataf(kOne);
        dataf(kOne);
        dataf(kOne);
    }
    dataf(v->a);
    begin(M_VTX_ATTR_4F(8), 4);                     /* projektívna súradnica: TXP delí q */
    dataf(v->uOverW);
    dataf(v->vOverW);
    dataf(0.0f);
    dataf(v->invW);
    begin(M_VTX_ATTR_4F(0), 4);
    dataf(v->x);
    dataf(v->y);
    dataf(zclamp(v->z));
    dataf(kOne);
}

static void prim_begin(NVU32 prim)
{
    begin(M_VERTEX_BEGIN_END, 1);
    data(prim);
}

static void prim_end(void)
{
    begin(M_VERTEX_BEGIN_END, 1);
    data(0);
}

static NVU32 vmode_prim(long mode)
{
    switch (mode) {
    case kQAVertexMode_Point:    return PRIM_POINTS;
    case kQAVertexMode_Line:     return PRIM_LINES;
    case kQAVertexMode_Polyline: return PRIM_LINE_STRIP;
    case kQAVertexMode_Strip:    return PRIM_TRI_STRIP;
    case kQAVertexMode_Fan:      return PRIM_TRI_FAN;
    default:                     return PRIM_TRIANGLES;
    }
}

/* ---- metódy draw contextu --------------------------------------------------- */

/* rave-10: záznam jednej snímky na požiadanie — pri zapnutom Caps Lock sa pri najbližšom
 * RenderStart zapíše celá snímka (max. 300 riadkov), raz za beh aplikácie. */
static int gTrace, gTraceDone, gTraceLines, gCapsWas;
static int gTraceOn;                /* boot-args nv34trace: záznam snímky cez Caps Lock */

#define TR(...) do { if (gTrace && gTraceLines < 250) { gTraceLines++; lg(__VA_ARGS__); } } while (0)

/* rave-13: zhrnutie — rovnaké po sebe idúce kreslenia (rovnaký stav) sa spočítajú */
static NVU32 gTrSig[8], gTrRun;

static void tr_flush(void)
{
    if (gTrRun > 1)
        TR("  x%u", gTrRun);
    gTrRun = 0;
}

static void tr_draw(const char *what, const Ctx *c, unsigned long n, float x, float y, float z, float w, float a)
{
    NVU32 sig[8];
    const TQATexture *t = c->tex;
    int i, same = 1;
    if (!gTrace)
        return;
    sig[0] = (NVU32)what[0] << 8 | (NVU32)what[1];
    sig[1] = (NVU32)t;
    sig[2] = c->si[kQATag_ZFunction];
    sig[3] = c->si[kQATag_Blend];
    sig[4] = c->si[kQATagGL_BlendSrc] << 16 | c->si[kQATagGL_BlendDst];
    sig[5] = c->si[kQATag_TextureOp];
    sig[6] = c->flat2D;
    sig[7] = c->si[kQATag_ZBufferMask] | (c->atiZWrite << 8);
    for (i = 0; i < 8; i++)
        if (sig[i] != gTrSig[i])
            same = 0;
    if (same && gTrRun) {
        gTrRun++;
        return;
    }
    tr_flush();
    for (i = 0; i < 8; i++)
        gTrSig[i] = sig[i];
    gTrRun = 1;
    if (t && t->magic == 'NVtx')
        TR("%s%u t%x(typ %d %ux%u a0 %u%%) zf%u bl%u %x op%u 2d%u zw%u | %d,%d z%f w%f a%f", what, n, (NVU32)t,
           t->pixelType, t->w, t->h, t->alpha0, sig[2], sig[3], sig[4], sig[5], sig[6], c->atiZWrite,
           (long)x, (long)y, (double)z, (double)w, (double)a);
    else
        TR("%s%u bez tex zf%u bl%u %x 2d%u | %d,%d z%f a%f", what, n, sig[2], sig[3], sig[4], sig[6],
           (long)x, (long)y, (double)z, (double)a);
}

static void tr_vt(const char *what, const Ctx *c, unsigned long n, long mode, const TQAVTexture *v)
{
    (void)mode;
    tr_draw(what, c, n, v->x, v->y, v->z, v->invW, v->a);
}

static void tr_vg(const char *what, const Ctx *c, unsigned long n, long mode, const TQAVGouraud *v)
{
    (void)mode;
    tr_draw(what, c, n, v->x, v->y, v->z, v->invW, v->a);
}

static int gStateLogs;

static void SetFloat(TQADrawContext *dc, long tag, float v)
{
    if (tag > kQATag_ColorBG_b)
        TR("f%d=%f", tag, (double)v);
    Ctx *c = CTX(dc);
    if (tag >= 0 && tag < STATE_MAX) {
        if (c->sf[tag] != v && gStateLogs < 60 && tag > kQATag_ColorBG_b) {
            gStateLogs++;
            lg("setFloat %d = %f", tag, (double)v);
        }
        c->sf[tag] = v;
        return;
    }
    if (first_time(0x10000000 | tag))
        lg("setFloat %d = %f (neznamy)", tag, (double)v);
}

static void SetInt(TQADrawContext *dc, long tag, unsigned long v)
{
    if (tag >= 1000 && tag != 1022)
        TR("i%d=%x", tag, v);
    Ctx *c = CTX(dc);
    if (tag >= 0 && tag < STATE_MAX) {
        if (c->si[tag] != v && gStateLogs < 60 && first_time(0x12000000 | (tag << 8) | (v & 0xff))) {
            gStateLogs++;
            lg("setInt %d = %x", tag, v);
        }
        c->si[tag] = v;
        if (tag == kQATagGL_LinePattern || tag == kQATag_FogMode || tag == kQATag_AlphaTestFunc ||
            tag == kQATag_ChannelMask || tag == kQATag_MultiTextureEnable) {
            if (v && first_time(0x20000000 | tag))
                lg("setInt %d = %x (zatial ignorovane)", tag, v);
        }
        return;
    }
    if (tag == 1022) {                              /* rave-17: ATI zápis hĺbky */
        c->atiZWrite = v != 0;
        return;
    }
    if (tag == 1020) {                              /* rave-22: ATI modulácia 2× */
        c->ati2x = v != 0 && !gNo2x;
        c->hwBlend = -1;
        return;
    }
    if (first_time(0x30000000 | ((tag & 0xfff) << 12) | (v & 0xfff)))
        lg("setInt %d = %x (neznamy)", tag, v);   /* rave-15: každá nová hodnota */
}

static void SetPtr(TQADrawContext *dc, long tag, const void *v)
{
    (void)0;
    Ctx *c = CTX(dc);
    if (tag == kQATag_Texture) {
        c->tex = (TQATexture *)v;
        return;
    }
    if (first_time(0x40000000 | tag))
        lg("setPtr %d = %x (neznamy)", tag, (NVU32)v);
}

static float GetFloat(const TQADrawContext *dc, long tag)
{
    CL("getFloat %d", tag);
    if (gTrace) { tr_flush(); TR("getFloat %d", tag); }
    if (tag >= 0 && tag < STATE_MAX)
        return CTX(dc)->sf[tag];
    if (first_time(0x50000000 | tag))
        lg("getFloat %d (neznamy)", tag);
    return 0.0f;
}

static unsigned long GetInt(const TQADrawContext *dc, long tag)
{
    CL("getInt %d", tag);
    if (gTrace) { tr_flush(); TR("getInt %d", tag); }
    if (tag >= 0 && tag < STATE_MAX)
        return CTX(dc)->si[tag];
    if (tag == 1011 && gSpoofATI) {
        /* rave-24: ATI typ čipu. UT IsRageProAvail (štartovací dialóg): ≥ 772 = RAVE dostupné,
         * ≥ 1280 = novší čip; OpenGLRendererATI: 777 = Rage Mobility */
        /* minigl-12: URaveRenderDevice pri > 772 kreslí svetelné mapy ako druhú textúru cez súkromné
         * ATI tagy 1012–1039/1018 (engine ich nevie) → UT bez tieňov. 772 = dialóg RAVE ponúkne,
         * hra kreslí svetelné mapy druhým prechodom. */
        NVU32 v = gAtiGL ? 1280 : 772;
        if (first_time(0x60000000 | tag))
            lg("getInt 1011 -> %u (ATI cip)", v);
        return v;
    }
    if (first_time(0x60000000 | tag))
        lg("getInt %d (neznamy)", tag);
    return 0;
}

/* ---- súkromná ATI tabuľka (getPtr 1021) — rave-19 ------------------------------------------
 * UT RaveDrv (URaveRenderDevice) pri vendore ATI: Lock → [0](ctx, 0) zmaže farbu, [1](ctx, 0)
 * z-buffer; ClearZ (po oblohe) → [1](ctx, 0); DownloadTexture → [2](4, ?, &TQAImage, textúra)
 * nahradí obsah existujúcej textúry. Bez ATI UT z-buffer po oblohe nemaže → obloha presvitá.
 * Ostatné položky (OpenGLRendererATI ich používa viac) len zalogujú volanie. */
static void clear_buffers(Ctx *c, int color, int depth);
static void reupload_level0(TQATexture *t, const TQAImage *img);

static long AtiClearColor(TQADrawContext *dc, long x)
{
    (void)x;
    if (gTrace) { tr_flush(); TR("ATI[0] ClearColor"); }
    if (dc && dc->drawPrivate)
        clear_buffers(CTX(dc), 1, 0);
    return 0;
}

static long AtiClearZ(TQADrawContext *dc, long x)
{
    (void)x;
    if (gTrace) { tr_flush(); TR("ATI[1] ClearZ"); }
    if (dc && dc->drawPrivate)
        clear_buffers(CTX(dc), 0, 1);
    return 0;
}

static int gAtiMismatch;

static long AtiTexUpdate(long a, long b, const TQAImage *img, TQATexture *t)
{
    if (first_time(0x15000002))
        lg("ATI[2] (%x, %x, img %dx%d rb %d, tex %x)", a, b, img ? img->width : 0, img ? img->height : 0,
           img ? img->rowBytes : 0, (NVU32)t);
    if (t && t->magic == 'NVtx' && img && img->pixmap && img->width == (long)t->w && img->height == (long)t->h) {
        reupload_level0(t, img);
    } else if (t && t->magic == 'NVtx' && img && gAtiMismatch < 8) {
        gAtiMismatch++;
        lg("ATI[2] nesedi: img %dx%d rb %d, textura %ux%u typ %d levels %u (a %x b %x)", img->width, img->height,
           img->rowBytes, t->w, t->h, t->pixelType, t->levels, a, b);
    }
    return 0;
}

#define ATI_STUB(n) static long AtiStub##n(long a, long b, long c, long d) { \
    if (first_time(0x15000000 | n)) { lg("ATI[%d] (%x, %x, %x, %x) nepodporovane", n, a, b, c, d); } \
    else CL("ATI[%d]", n); \
    return 0; }
ATI_STUB(3) ATI_STUB(4) ATI_STUB(5) ATI_STUB(6) ATI_STUB(7) ATI_STUB(8) ATI_STUB(9) ATI_STUB(10)
ATI_STUB(11) ATI_STUB(12) ATI_STUB(13) ATI_STUB(14) ATI_STUB(15) ATI_STUB(16) ATI_STUB(17) ATI_STUB(18)
ATI_STUB(19) ATI_STUB(20) ATI_STUB(21) ATI_STUB(22) ATI_STUB(23) ATI_STUB(24) ATI_STUB(25) ATI_STUB(26)
ATI_STUB(27) ATI_STUB(28) ATI_STUB(29) ATI_STUB(30) ATI_STUB(31) ATI_STUB(32) ATI_STUB(33) ATI_STUB(34)
ATI_STUB(35) ATI_STUB(36) ATI_STUB(37) ATI_STUB(38) ATI_STUB(39) ATI_STUB(40) ATI_STUB(41) ATI_STUB(42)
ATI_STUB(43) ATI_STUB(44) ATI_STUB(45) ATI_STUB(46) ATI_STUB(47) ATI_STUB(48) ATI_STUB(49) ATI_STUB(50)
ATI_STUB(51) ATI_STUB(52) ATI_STUB(53) ATI_STUB(54) ATI_STUB(55) ATI_STUB(56) ATI_STUB(57) ATI_STUB(58)
ATI_STUB(59) ATI_STUB(60) ATI_STUB(61) ATI_STUB(62) ATI_STUB(63) ATI_STUB(64) ATI_STUB(65) ATI_STUB(66)
ATI_STUB(67) ATI_STUB(68) ATI_STUB(69) ATI_STUB(70) ATI_STUB(71) ATI_STUB(72) ATI_STUB(73) ATI_STUB(74)
ATI_STUB(75) ATI_STUB(76) ATI_STUB(77) ATI_STUB(78) ATI_STUB(79)

/* gl-q2-7: glrVendorGetDispatchTable číta aj pozície 31–60 (bajty 124–240) ako funkcie kreslenia */
static void *gAtiTable[80] = {
    (void *)AtiClearColor, (void *)AtiClearZ, (void *)AtiTexUpdate, (void *)AtiStub3, (void *)AtiStub4,
    (void *)AtiStub5, (void *)AtiStub6, (void *)AtiStub7, (void *)AtiStub8, (void *)AtiStub9,
    (void *)AtiStub10, (void *)AtiStub11, (void *)AtiStub12, (void *)AtiStub13, (void *)AtiStub14,
    (void *)AtiStub15, (void *)AtiStub16, (void *)AtiStub17, (void *)AtiStub18, (void *)AtiStub19,
    (void *)AtiStub20, (void *)AtiStub21, (void *)AtiStub22, (void *)AtiStub23, (void *)AtiStub24,
    (void *)AtiStub25, (void *)AtiStub26, (void *)AtiStub27, (void *)AtiStub28, (void *)AtiStub29,
    (void *)AtiStub30, (void *)AtiStub31, (void *)AtiStub32, (void *)AtiStub33, (void *)AtiStub34,
    (void *)AtiStub35, (void *)AtiStub36, (void *)AtiStub37, (void *)AtiStub38, (void *)AtiStub39,
    (void *)AtiStub40, (void *)AtiStub41, (void *)AtiStub42, (void *)AtiStub43, (void *)AtiStub44,
    (void *)AtiStub45, (void *)AtiStub46, (void *)AtiStub47, (void *)AtiStub48, (void *)AtiStub49,
    (void *)AtiStub50, (void *)AtiStub51, (void *)AtiStub52, (void *)AtiStub53, (void *)AtiStub54,
    (void *)AtiStub55, (void *)AtiStub56, (void *)AtiStub57, (void *)AtiStub58, (void *)AtiStub59,
    (void *)AtiStub60, (void *)AtiStub61, (void *)AtiStub62, (void *)AtiStub63, (void *)AtiStub64,
    (void *)AtiStub65, (void *)AtiStub66, (void *)AtiStub67, (void *)AtiStub68, (void *)AtiStub69,
    (void *)AtiStub70, (void *)AtiStub71, (void *)AtiStub72, (void *)AtiStub73, (void *)AtiStub74,
    (void *)AtiStub75, (void *)AtiStub76, (void *)AtiStub77, (void *)AtiStub78, (void *)AtiStub79
};

static void *GetPtr(const TQADrawContext *dc, long tag)
{
    CL("getPtr %d", tag);
    if (tag == 1021 && gSpoofATI) {
        if (first_time(0x70000000 | tag))
            lg("getPtr 1021 -> ATI tabulka");
        return gAtiTable;
    }
    if (gTrace) { tr_flush(); TR("getPtr %d", tag); }
    if (tag == kQATag_Texture)
        return CTX(dc)->tex;
    if (first_time(0x70000000 | tag))
        lg("getPtr %d (neznamy)", tag);
    return 0;
}

static int flat_t(const TQAVTexture *v, unsigned long n)
{
    unsigned long i;
    for (i = 0; i < n; i++)
        if (v[i].z > 0.0f || v[i].invW != 1.0f)
            return 0;
    return 1;
}

static int flat_g(const TQAVGouraud *v, unsigned long n)
{
    unsigned long i;
    for (i = 0; i < n; i++)
        if (v[i].z > 0.0f)
            return 0;
    return 1;
}

static void DrawPoint(const TQADrawContext *dc, const TQAVGouraud *v)
{
    tr_vg("Point", CTX(dc), 1, 0, v);
    if (first_time(0x13000000 | 1))
        lg("prvy DrawPoint");
    Ctx *c = CTX(dc);
    if (!prepare(c, MODE_GOURAUD))
        return;
    prim_begin(PRIM_POINTS);
    vw_start(1, WPV_G);
    vtx_g(v);
    prim_end();
}

static void DrawLine(const TQADrawContext *dc, const TQAVGouraud *v0, const TQAVGouraud *v1)
{
    tr_vg("Line", CTX(dc), 2, 0, v0);
    if (first_time(0x13000000 | 2))
        lg("prvy DrawLine");
    Ctx *c = CTX(dc);
    if (!prepare(c, MODE_GOURAUD))
        return;
    prim_begin(PRIM_LINES);
    vw_start(2, WPV_G);
    vtx_g(v0);
    vtx_g(v1);
    prim_end();
}

static void DrawTriGouraud(const TQADrawContext *dc, const TQAVGouraud *v0, const TQAVGouraud *v1,
                           const TQAVGouraud *v2, unsigned long flags)
{
    CTX(dc)->flat2D = v0->z <= 0.0f && v1->z <= 0.0f && v2->z <= 0.0f;
    tr_vg("TriG", CTX(dc), 1, 3, v0);
    if (first_time(0x13000000 | 3))
        lg("prvy DrawTriGouraud");
    Ctx *c = CTX(dc);
    (void)flags;
    if (!prepare(c, MODE_GOURAUD))
        return;
    prim_begin(PRIM_TRIANGLES);
    vw_start(3, WPV_G);
    vtx_g(v0);
    vtx_g(v1);
    vtx_g(v2);
    prim_end();
    c->tris++;
}

static void DrawTriTexture(const TQADrawContext *dc, const TQAVTexture *v0, const TQAVTexture *v1,
                           const TQAVTexture *v2, unsigned long flags)
{
    CTX(dc)->flat2D = v0->z <= 0.0f && v1->z <= 0.0f && v2->z <= 0.0f &&
                      v0->invW == 1.0f && v1->invW == 1.0f && v2->invW == 1.0f;
    tr_vt("TriT", CTX(dc), 1, 3, v0);
    if (first_time(0x13000000 | 4))
        lg("prvy DrawTriTexture");
    Ctx *c = CTX(dc);
    (void)flags;
    if (!prepare(c, MODE_TEXTURE))
        return;
    prim_begin(PRIM_TRIANGLES);
    vw_start(3, WPV_T);
    vtx_t(c, v0);
    vtx_t(c, v1);
    vtx_t(c, v2);
    prim_end();
    c->tris++;
}

static void DrawVGouraud(const TQADrawContext *dc, unsigned long n, long mode, const TQAVGouraud v[],
                         const unsigned long flags[])
{
    CTX(dc)->flat2D = flat_g(v, n);
    if (n) tr_vg("VG", CTX(dc), n, mode, &v[0]);
    if (first_time(0x13000000 | 5))
        lg("prvy DrawVGouraud");
    Ctx *c = CTX(dc);
    unsigned long i;
    (void)flags;
    if (!n || !prepare(c, MODE_GOURAUD))
        return;
    prim_begin(vmode_prim(mode));
    vw_start(n, WPV_G);
    for (i = 0; i < n; i++)
        vtx_g(&v[i]);
    prim_end();
    c->tris += n;
}

static void DrawVTexture(const TQADrawContext *dc, unsigned long n, long mode, const TQAVTexture v[],
                         const unsigned long flags[])
{
    CTX(dc)->flat2D = flat_t(v, n);
    if (n) tr_vt("VT", CTX(dc), n, mode, &v[0]);
    if (first_time(0x13000000 | 6))
        lg("prvy DrawVTexture");
    Ctx *c = CTX(dc);
    unsigned long i;
    (void)flags;
    if (!n || !prepare(c, MODE_TEXTURE))
        return;
    prim_begin(vmode_prim(mode));
    vw_start(n, WPV_T);
    for (i = 0; i < n; i++)
        vtx_t(c, &v[i]);
    prim_end();
    c->tris += n;
}

static void SubmitVerticesGouraud(const TQADrawContext *dc, unsigned long n, const TQAVGouraud *v)
{
    CTX(dc)->vg = v;
    CTX(dc)->nvg = n;
}

static void SubmitVerticesTexture(const TQADrawContext *dc, unsigned long n, const TQAVTexture *v)
{
    CTX(dc)->vt = v;
    CTX(dc)->nvt = n;
}

static void DrawTriMeshGouraud(const TQADrawContext *dc, unsigned long n, const TQAIndexedTriangle *t)
{
    CTX(dc)->flat2D = 0;
    TR("MeshG n %u nv %u", n, CTX(dc)->nvg);
    if (first_time(0x13000000 | 7))
        lg("prvy DrawTriMeshGouraud");
    Ctx *c = CTX(dc);
    unsigned long i, k;
    if (!n || !c->vg || !prepare(c, MODE_GOURAUD))
        return;
    prim_begin(PRIM_TRIANGLES);
    vw_start(3 * n, WPV_G);
    for (i = 0; i < n; i++)
        for (k = 0; k < 3; k++)
            vtx_g(&c->vg[t[i].vertices[k] < c->nvg ? t[i].vertices[k] : 0]);
    prim_end();
    c->tris += n;
}

static void DrawTriMeshTexture(const TQADrawContext *dc, unsigned long n, const TQAIndexedTriangle *t)
{
    CTX(dc)->flat2D = 0;
    if (CTX(dc)->vt && CTX(dc)->nvt) tr_vt("MeshT", CTX(dc), n, 0, &CTX(dc)->vt[0]);
    if (first_time(0x13000000 | 8))
        lg("prvy DrawTriMeshTexture");
    Ctx *c = CTX(dc);
    unsigned long i, k;
    if (!n || !c->vt || !prepare(c, MODE_TEXTURE))
        return;
    prim_begin(PRIM_TRIANGLES);
    vw_start(3 * n, WPV_T);
    for (i = 0; i < n; i++)
        for (k = 0; k < 3; k++)
            vtx_t(c, &c->vt[t[i].vertices[k] < c->nvt ? t[i].vertices[k] : 0]);
    prim_end();
    c->tris += n;
}

/* bitmapa = obdĺžnik s RECT textúrou, ľavý horný roh vo v->x, v->y */
static void DrawBitmap(const TQADrawContext *dc, const TQAVGouraud *v, TQABitmap *bitmap)
{
    tr_vg("Bitmap", CTX(dc), 1, 0, v);
    if (first_time(0x13000000 | 9))
        lg("prvy DrawBitmap");
    Ctx *c = CTX(dc);
    TQATexture *t = (TQATexture *)bitmap;
    TQATexture *save = c->tex;
    unsigned long saveOp = c->si[kQATag_TextureOp];
    float sx = c->sf[kQATag_BitmapScale_x], sy = c->sf[kQATag_BitmapScale_y];
    float x0 = v->x, y0 = v->y, x1, y1, tw, th;
    TQAVTexture q[4];
    int i;

    if (!t || t->magic != 'NVtx')
        return;
    if (!(sx > 0.0f))
        sx = 1.0f;
    if (!(sy > 0.0f))
        sy = 1.0f;
    x1 = x0 + t->w * sx;
    y1 = y0 + t->h * sy;
    tw = (float)t->w;
    th = (float)t->h;
    memset(q, 0, sizeof(q));
    for (i = 0; i < 4; i++) {
        q[i].z = v->z;
        q[i].invW = 1.0f;
        q[i].a = v->a;
        q[i].kd_r = v->r;
        q[i].kd_g = v->g;
        q[i].kd_b = v->b;
    }
    q[0].x = x0; q[0].y = y0; q[0].uOverW = 0; q[0].vOverW = 0;
    q[1].x = x1; q[1].y = y0; q[1].uOverW = tw; q[1].vOverW = 0;
    q[2].x = x1; q[2].y = y1; q[2].uOverW = tw; q[2].vOverW = th;
    q[3].x = x0; q[3].y = y1; q[3].uOverW = 0; q[3].vOverW = th;
    c->tex = t;
    /* Alpha1 bitmapa nesie farbu vrcholu, ostatné vlastnú */
    c->si[kQATag_TextureOp] = t->pixelType == kQAPixel_Alpha1 ? kQATextureOp_Modulate : kQATextureOp_None;
    if (prepare(c, MODE_TEXTURE)) {
        prim_begin(PRIM_TRI_FAN);
        vw_start(4, WPV_T);
        for (i = 0; i < 4; i++)
            vtx_t(c, &q[i]);
        prim_end();
    }
    c->tex = save;
    c->si[kQATag_TextureOp] = saveOp;
}

/* ---- buffre ---------------------------------------------------------------- */

static void clear_buffers(Ctx *c, int color, int depth)
{
    NVU32 bits = 0;
    if (!bind(c))
        return;
    if (color) {
        begin(M_CLEAR_COLOR_VALUE, 1);
        if (c->bpp == 16) {
            NVU32 v = ((ubyte(c->sf[kQATag_ColorBG_r]) >> 3) << 10) | ((ubyte(c->sf[kQATag_ColorBG_g]) >> 3) << 5) |
                      (ubyte(c->sf[kQATag_ColorBG_b]) >> 3);
            data((v << 16) | v);
        } else {
            data((ubyte(c->sf[kQATag_ColorBG_a]) << 24) | (ubyte(c->sf[kQATag_ColorBG_r]) << 16) |
                 (ubyte(c->sf[kQATag_ColorBG_g]) << 8) | ubyte(c->sf[kQATag_ColorBG_b]));
        }
        bits |= 0xF0;
    }
    if (depth && c->hasZ) {
        begin(0x0a70, 1);                           /* DEPTH_WRITE_ENABLE (gl-8) */
        data(1);
        begin(M_CLEAR_DEPTH_VALUE, 1);
        data(0xffffffff);
        bits |= 0x03;
        c->hwZ = -1;
    }
    if (!bits)
        return;
    if (c->dbl) {
        begin(M_CLEAR_BUFFERS, 1);
        data(bits);
    } else {
        /* na obrazovke: CLEAR by zmazal celý RT → obdĺžnik v rozsahu kontextu */
        TQAVGouraud q[4];
        int i;
        memset(q, 0, sizeof(q));
        for (i = 0; i < 4; i++) {
            q[i].z = 1.0f;
            q[i].r = c->sf[kQATag_ColorBG_r];
            q[i].g = c->sf[kQATag_ColorBG_g];
            q[i].b = c->sf[kQATag_ColorBG_b];
            q[i].a = c->sf[kQATag_ColorBG_a];
        }
        q[1].x = q[2].x = (float)c->width;
        q[2].y = q[3].y = (float)c->height;
        set_mode(c, MODE_GOURAUD);
        set_vtxfmt(c, 0);
        begin(M_BLEND_FUNC_ENABLE, 1);
        data(0);
        c->hwBlend = -1;
        begin(M_DEPTH_FUNC, 1);
        data(0x207);                                /* ALWAYS */
        begin(0x0a70, 2);
        data((bits & 3) ? 1 : 0);
        data((bits & 3) ? 1 : 0);
        c->hwZ = -1;
        begin(M_COLOR_MASK, 1);
        data(color ? 0x01010101 : 0);
        prim_begin(PRIM_TRI_FAN);
        vw_start(4, WPV_G);
        for (i = 0; i < 4; i++)
            vtx_g(&q[i]);
        prim_end();
        begin(M_COLOR_MASK, 1);
        data(0x01010101);
    }
    kick();
}

static TQADevice back_device(Ctx *c)
{
    TQADevice d;
    d.deviceType = kQADeviceMemory;
    d.device.memoryDevice.rowBytes = c->backPitch;
    d.device.memoryDevice.pixelType = c->bpp == 16 ? kQAPixel_RGB16 : kQAPixel_ARGB32;
    d.device.memoryDevice.width = c->width;
    d.device.memoryDevice.height = c->height;
    d.device.memoryDevice.baseAddr = (void *)(S->a->vram + c->backOff);
    return d;
}

static void RenderStart(const TQADrawContext *dc, const TQARect *dirty, const TQADrawContext *initial)
{
    CL("RenderStart %x", (NVU32)dc);
    {
        UInt8 km[16];
        km[7] = 0;
        if (gTraceOn)
            GetKeys(km);
        int caps = (km[7] & 0x02) != 0;         /* Caps Lock (kód 0x39) */
        if (caps && !gCapsWas && gTraceDone < 2) {  /* každé zapnutie = 1 snímka, max. 2 */
            gTrace = 1;
            gTraceDone++;
            gTraceLines = 0;
            gTrRun = 0;
            gTrSig[1] = 0xffffffff;
            lg("---- zaznam snimky %d ----", gTraceDone);
        }
        gCapsWas = caps;
    }
    TR("RenderStart dirty %x", (NVU32)dirty);
    Ctx *c = CTX(dc);
    TQANoticeMethod init = c->notice[kQAMethod_BufferInitialize];
    (void)initial;

    if (!bind(c))
        return;
    if (init.bufferNoticeMethod && c->dbl) {
        TQADevice d = back_device(c);
        TQARect r;
        r.left = r.top = 0;
        r.right = c->width;
        r.bottom = c->height;
        clear_buffers(c, 0, 1);
        gpu_sync();
        init.bufferNoticeMethod(dc, &d, dirty ? dirty : &r, c->noticeRef[kQAMethod_BufferInitialize]);
    } else {
        clear_buffers(c, 1, 1);
    }
}

/* QuickDraw región → obdĺžniky (pásy), prienik s obdĺžnikom r; volá blit pre každý */
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

static void blit_rect(Ctx *c, long l, long t, long r, long b, const TQARect *lim)
{
    PixMapPtr pm = *(*c->gd)->gdPMap;

    if (l < lim->left) l = lim->left;
    if (t < lim->top) t = lim->top;
    if (r > lim->right) r = lim->right;
    if (b > lim->bottom) b = lim->bottom;
    if (l >= r || t >= b)
        return;
    nva_copy_from(S->a, c->backOff, c->backPitch, (int)(l - c->rect.left), (int)(t - c->rect.top),
                  (int)(l - pm->bounds.left), (int)(t - pm->bounds.top), (int)(r - l), (int)(b - t));
}

static void present(Ctx *c, const TQARect *mod)
{
    TQARect lim = c->rect;
    Region *rg;

    if (mod) {                                      /* modifiedRect je relatívny ku kontextu */
        if (c->rect.left + mod->left > lim.left) lim.left = c->rect.left + mod->left;
        if (c->rect.top + mod->top > lim.top) lim.top = c->rect.top + mod->top;
        if (c->rect.left + mod->right < lim.right) lim.right = c->rect.left + mod->right;
        if (c->rect.top + mod->bottom < lim.bottom) lim.bottom = c->rect.top + mod->bottom;
    }
    screen_geom(c);
    if (((*(*c->gd)->gdPMap)->pixelSize) != c->bpp)
        return;                                     /* hĺbka obrazovky sa zmenila */
    if (S->ensure3D() != 0 || S->a->depth != (NVU32)c->bpp)
        return;                                     /* 2D povrch QD Accel = aktuálna obrazovka */
    /* 2D a 3D sú v tom istom PGRAPH → blit prebehne po kreslení, sync netreba */
    if (!c->clip || !*c->clip || (*c->clip)->rgnSize == 10) {
        if (c->clip && *c->clip) {
            Rect *bb = &(*c->clip)->rgnBBox;
            blit_rect(c, bb->left, bb->top, bb->right, bb->bottom, &lim);
        } else {
            blit_rect(c, lim.left, lim.top, lim.right, lim.bottom, &lim);
        }
    } else {
        short xs[MAX_XS];
        const short *p;
        int n = 0, i;
        short y, prevY = 0;
        HLock((Handle)c->clip);
        rg = *c->clip;
        p = (const short *)((const char *)rg + 10);
        for (;;) {
            short x;
            y = *p++;
            if (y == 0x7FFF)
                break;
            if (n > 0 && y > prevY)
                for (i = 0; i + 1 < n; i += 2)
                    blit_rect(c, xs[i], prevY, xs[i + 1], y, &lim);
            while ((x = *p++) != 0x7FFF)
                n = toggle_x(xs, n, x);
            prevY = y;
        }
        HUnlock((Handle)c->clip);
    }
    kick();
}

static TQAError RenderEnd(const TQADrawContext *dc, const TQARect *modified)
{
    CL("RenderEnd %x", (NVU32)dc);
    if (gTrace) {
        tr_flush();
        TR("RenderEnd");
        gTrace = 0;
        lg("---- koniec zaznamu (%u riadkov) ----", gTraceLines);
    }
    Ctx *c = CTX(dc);
    TQANoticeMethod done = c->notice[kQAMethod_RenderCompletion];

    if (!S || S->state3D != 1)
        return kQAError;
    if (c->dbl && !c->si[kQATag_DontSwap])
        present(c, modified);
    else
        kick();
    c->frames++;
    gFrame++;
    if (gStats) {                                   /* rave-25: boot-args nv34ravestats */
        static unsigned long t0;
        static NVU32 f0, tr0;
        unsigned long t = TickCount();
        if (!t0) {
            t0 = t;
            f0 = c->frames;
            tr0 = c->tris;
        } else if (c->frames - f0 >= 250 && t > t0) {
            unsigned long fps10 = (unsigned long)((c->frames - f0) * 600UL / (t - t0));
            lg("snimky %u: %lu.%lu fps, %u troj./snimku (vrchol %u slov)", c->frames, fps10 / 10, fps10 % 10,
               (c->tris - tr0) / (c->frames - f0), WPV_T);
            t0 = t;
            f0 = c->frames;
            tr0 = c->tris;
        }
    }
    if (done.standardNoticeMethod) {
        gpu_sync();
        done.standardNoticeMethod(dc, c->noticeRef[kQAMethod_RenderCompletion]);
    }
    return kQANoErr;
}

static TQAError RenderAbort(const TQADrawContext *dc)
{
    CL("RenderAbort");
    (void)dc;
    if (S)
        gpu_sync();
    return kQANoErr;
}

static TQAError Flush(const TQADrawContext *dc)
{
    CL("Flush");
    if (gTrace) { tr_flush(); TR("Flush"); }
    (void)dc;
    if (S)
        kick();
    return kQANoErr;
}

static TQAError Sync(const TQADrawContext *dc)
{
    CL("Sync");
    if (gTrace) { tr_flush(); TR("Sync"); }
    (void)dc;
    return S && gpu_sync() ? kQANoErr : kQAError;
}

static TQAError SwapBuffers(const TQADrawContext *dc, const TQARect *dirty)
{
    CL("SwapBuffers");
    if (gTrace) { tr_flush(); TR("SwapBuffers"); }
    Ctx *c = CTX(dc);
    if (c->dbl)
        present(c, dirty);
    return kQANoErr;
}

static TQABoolean Busy(const TQADrawContext *dc)
{
    CL("Busy");
    if (gTrace) { tr_flush(); TR("Busy"); }
    (void)dc;
    if (!S)
        return 0;
    return nva_rd32(S->a, 0x400700) != 0;
}

static TQAError SetNoticeMethod(const TQADrawContext *dc, long method, TQANoticeMethod cb, void *ref)
{
    CL("SetNoticeMethod %d %x", method, (NVU32)cb.standardNoticeMethod);
    Ctx *c = CTX(dc);
    if (method < 0 || method > 4)
        return kQAParamErr;
    c->notice[method] = cb;
    c->noticeRef[method] = ref;
    if (cb.standardNoticeMethod && method != kQAMethod_RenderCompletion &&
        method != kQAMethod_BufferInitialize && first_time(0x80000000 | method))
        lg("setNoticeMethod %d (nevolane)", method);
    return kQANoErr;
}

static TQAError GetNoticeMethod(const TQADrawContext *dc, long method, TQANoticeMethod *cb, void **ref)
{
    Ctx *c = CTX(dc);
    if (method < 0 || method > 4)
        return kQAParamErr;
    *cb = c->notice[method];
    *ref = c->noticeRef[method];
    return kQANoErr;
}

static TQAError AccessDrawBuffer(const TQADrawContext *dc, TQAPixelBuffer *buf)
{
    CL("AccessDrawBuffer");
    if (gTrace) { tr_flush(); TR("AccessDrawBuffer"); }
    Ctx *c = CTX(dc);
    if (!c->dbl || !gpu_sync())
        return kQANotSupported;
    screen_geom(c);
    *buf = back_device(c).device.memoryDevice;
    return kQANoErr;
}

static TQAError AccessDrawBufferEnd(const TQADrawContext *dc, const TQARect *dirty)
{
    CL("AccessDrawBufferEnd");
    (void)dc;
    (void)dirty;
    return kQANoErr;
}

static TQAError AccessZBuffer(const TQADrawContext *dc, TQAZBuffer *zb)
{
    CL("AccessZBuffer");
    if (gTrace) { tr_flush(); TR("AccessZBuffer"); }
    (void)dc;
    (void)zb;
    return kQANotSupported;
}

static TQAError AccessZBufferEnd(const TQADrawContext *dc, const TQARect *dirty)
{
    if (gTrace) { tr_flush(); TR("AccessZBufferEnd"); }
    (void)dc;
    (void)dirty;
    return kQANotSupported;
}

static TQAError ClearDrawBuffer(const TQADrawContext *dc, const TQARect *r, const TQADrawContext *init)
{
    CL("ClearDrawBuffer");
    if (gTrace) { tr_flush(); TR("ClearDrawBuffer"); }
    if (first_time(0x14000001))
        lg("prvy ClearDrawBuffer");
    (void)r;
    (void)init;
    clear_buffers(CTX(dc), 1, 0);
    return kQANoErr;
}

static TQAError ClearZBuffer(const TQADrawContext *dc, const TQARect *r, const TQADrawContext *init)
{
    CL("ClearZBuffer");
    if (gTrace) { tr_flush(); TR("ClearZBuffer"); }
    if (first_time(0x14000002))
        lg("prvy ClearZBuffer");
    (void)r;
    (void)init;
    clear_buffers(CTX(dc), 0, 1);
    return kQANoErr;
}

static void SubmitMultiTextureParams(const TQADrawContext *dc, unsigned long n, const TQAVMultiTexture *p)
{
    (void)dc;
    (void)n;
    (void)p;
}

static TQAError TextureFromContext(const TQADrawContext *dc, unsigned long flags, TQATexture **t)
{
    (void)dc;
    (void)flags;
    *t = 0;
    return kQANotSupported;
}

static TQAError BitmapFromContext(const TQADrawContext *dc, unsigned long flags, TQABitmap **b)
{
    (void)dc;
    (void)flags;
    *b = 0;
    return kQANotSupported;
}

/* ---- metódy enginu --------------------------------------------------------- */

static int device_ok(const TQADevice *d, PixMapPtr *out)
{
    PixMapPtr pm;
    NVU32 off;

    if (!d || d->deviceType != kQADeviceGDevice || !d->device.gDevice || !shared())
        return 0;
    if (S->state3D == 2)
        return 0;
    pm = *(*d->device.gDevice)->gdPMap;
    off = (NVU32)((volatile NVU8 *)pm->baseAddr - S->a->vram);
    if (off >= S->a->vramSize || (pm->pixelSize != 32 && pm->pixelSize != 16))
        return 0;
    if (out)
        *out = pm;
    return 1;
}

static TQAError EngineCheckDevice(const TQADevice *d)
{
    CL("CheckDevice typ %d gd %x", d ? d->deviceType : -1, d && d->deviceType == 1 ? (NVU32)d->device.gDevice : 0);
    if (device_ok(d, 0))
        return kQANoErr;
    /* gl-q2-2: OpenGLRendererATI (glrGetFullScreenRendererInfo) hľadá enginy so zariadením
     * {GDevice, NULL} = „ktorákoľvek obrazovka“ */
    if (d && d->deviceType == kQADeviceGDevice && !d->device.gDevice && shared() && S->state3D != 2) {
        if (first_time(0xD2000000))
            lg("CheckDevice: GDevice NULL prijaty");
        return kQANoErr;
    }
    if (d && d->deviceType == kQADeviceGDevice && d->device.gDevice) {
        PixMapPtr pm = *(*d->device.gDevice)->gdPMap;
        if (first_time(0xD0000000 | pm->pixelSize))
            lg("CheckDevice: GDevice %d bpp odmietnuty", pm->pixelSize);
    } else if (d && first_time(0xD1000000 | d->deviceType)) {
        lg("CheckDevice: typ zariadenia %d odmietnuty", d->deviceType);
    }
    return kQAError;
}

/* gestalt 1002: tabuľka funkcií ATI (glrSetConfigData volá [7](blk, &flagy) a [8](blk, &hodnota);
 * blk = {1, zariadenie}). Význam odhadnutý z použitia: [8] ~ veľkosť VRAM, [7] bity 0–2. */
static long AtiInfoFlags(const long *blk, unsigned long *out)
{
    if (first_time(0x16000007))
        lg("ATIinfo[7] (blk %x %x) -> 7", blk ? blk[0] : 0, blk ? blk[1] : 0);
    if (out)
        *out = 7;
    return 0;
}

static long AtiInfoMem(const long *blk, unsigned long *out)
{
    if (first_time(0x16000008))
        lg("ATIinfo[8] (blk %x %x) -> VRAM", blk ? blk[0] : 0, blk ? blk[1] : 0);
    if (out)
        *out = S ? S->a->vramSize : 0x02000000;
    return 0;
}

#define INFO_STUB(n) static long AtiInfoStub##n(long a, long b, long c, long d) { \
    if (first_time(0x16000000 | n)) { lg("ATIinfo[%d] (%x, %x, %x, %x) nepodporovane", n, a, b, c, d); } \
    else CL("ATIinfo[%d]", n); \
    return 0; }
INFO_STUB(0) INFO_STUB(1) INFO_STUB(2) INFO_STUB(3) INFO_STUB(4) INFO_STUB(5) INFO_STUB(6)
INFO_STUB(9) INFO_STUB(10) INFO_STUB(11) INFO_STUB(12) INFO_STUB(13) INFO_STUB(14) INFO_STUB(15)
INFO_STUB(16) INFO_STUB(17) INFO_STUB(18) INFO_STUB(19)

static void *gAtiInfoTable[20] = {
    (void *)AtiInfoStub0, (void *)AtiInfoStub1, (void *)AtiInfoStub2, (void *)AtiInfoStub3,
    (void *)AtiInfoStub4, (void *)AtiInfoStub5, (void *)AtiInfoStub6, (void *)AtiInfoFlags,
    (void *)AtiInfoMem, (void *)AtiInfoStub9, (void *)AtiInfoStub10, (void *)AtiInfoStub11,
    (void *)AtiInfoStub12, (void *)AtiInfoStub13, (void *)AtiInfoStub14, (void *)AtiInfoStub15,
    (void *)AtiInfoStub16, (void *)AtiInfoStub17, (void *)AtiInfoStub18, (void *)AtiInfoStub19
};

static int gGestaltLogs;

static TQAError EngineGestalt(long sel, void *resp)
{
    long *r = (long *)resp;
    TQAError err = kQANoErr;
    switch (sel) {
    case kQAGestalt_OptionalFeatures:
        *r = kQAOptional_DeepZ | kQAOptional_Texture | kQAOptional_TextureHQ | kQAOptional_TextureColor |
             kQAOptional_Blend | kQAOptional_BlendAlpha | kQAOptional_OpenGL | kQAOptional_CL8 | kQAOptional_NoDither |
             kQAOptional_AccessDrawBuffer | kQAOptional_ClearDrawBuffer | kQAOptional_ClearZBuffer;
        /* gl-q2-3: OpenGLRendererATI berie engine pre obrazovku iba s BoundToDevice (ako ATI) */
        if (gAtiGL)
            *r |= kQAOptional_BoundToDevice;
        break;
    case kQAGestalt_FastFeatures:
        *r = kQAFast_Line | kQAFast_Gouraud | kQAFast_Texture | kQAFast_TextureHQ | kQAFast_Blend;
        break;
    case kQAGestalt_VendorID:
        *r = gSpoofATI ? 1 : VENDOR_ID;            /* boot-args nv34raveati: test, či hra chce ATI */
        break;
    case kQAGestalt_EngineID:
        *r = gSpoofATI ? 5 : ENGINE_ID;
        break;
    case kQAGestalt_Revision:
        *r = REVISION;
        break;
    case kQAGestalt_ASCIINameLength:
        *r = slen(ENGINE_NAME);
        break;
    case kQAGestalt_ASCIIName:
        memcpy(resp, ENGINE_NAME, slen(ENGINE_NAME) + 1);
        break;
    case kQAGestalt_TextureMemory:
    case kQAGestalt_FastTextureMemory:
        *r = shared() ? S->vramFreeBytes() : 0;
        break;
    case kQAGestalt_DrawContextPixelTypesAllowed:
    case kQAGestalt_DrawContextPixelTypesPreferred:
        *r = (1 << kQAPixel_ARGB32) | (1 << kQAPixel_RGB16);
        break;
    case kQAGestalt_TexturePixelTypesAllowed:
    case kQAGestalt_BitmapPixelTypesAllowed:
        *r = (1 << kQAPixel_RGB16) | (1 << kQAPixel_ARGB16) | (1 << kQAPixel_RGB32) | (1 << kQAPixel_ARGB32) |
             (1 << kQAPixel_CL8) | (1 << kQAPixel_ARGB16_4444) | (1 << kQAPixel_I8) | (1 << kQAPixel_AI16_88) |
             (sel == kQAGestalt_BitmapPixelTypesAllowed ? 1 << kQAPixel_Alpha1 : 0);
        break;
    case kQAGestalt_TexturePixelTypesPreferred:
    case kQAGestalt_BitmapPixelTypesPreferred:
        *r = (1 << kQAPixel_ARGB32) | (1 << kQAPixel_RGB32);
        break;
    case kQAGestalt_OptionalFeatures2:
        *r = kQAOptional2_Busy | kQAOptional2_SwapBuffers;
        break;
    case kQAGestalt_MultiTextureMax:
        *r = 1;
        break;
    default:
        if (gAtiGL && sel >= 1000 && sel <= 1010) {
            /* gl-q2-4: súkromné ATI gestalty (OpenGLRendererATI) — nezapísaná odpoveď = smeti */
            if (sel == 1001)
                *r = S ? S->a->vramSize : 0x02000000;       /* glrVendorBoardMemory */
            else if (sel == 1002)
                *(void **)resp = gAtiInfoTable;             /* glrSetConfigData: tabuľka funkcií */
            else
                *r = 0;
        } else {
            err = kQAGestaltUnknown;
        }
    }
    /* každý dotaz (max. 48 na proces) s odpoveďou — z toho vidno, čo hra/RAVE kontroluje */
    if (gGestaltLogs < 100) {
        gGestaltLogs++;
        if (err)
            lg("gestalt %d -> neznamy", sel);
        else if (sel >= 1000)
            lg("gestalt %d -> %x (ATI)", sel, *r);
        else if (sel == kQAGestalt_ASCIIName)
            lg("gestalt %d -> %s", sel, (char *)resp);
        else
            lg("gestalt %d -> %x", sel, *r);
    }
    return err;
}

static void free_ctx_mem(Ctx *c)
{
    if (c->backOff)
        S->vramFree(c->backOff);
    if (c->zOff)
        S->vramFree(c->zOff);
    c->backOff = c->zOff = 0;
}

static TQAError DrawPrivateNew(TQADrawContext *dc, const TQADevice *d, const TQARect *rect,
                               const TQAClip *clip, unsigned long flags)
{
    CL("DrawPrivateNew typ %d gd %x rect %d,%d-%d,%d fl %x", d ? d->deviceType : -1, d ? (NVU32)d->device.gDevice : 0, rect ? rect->left : 0, rect ? rect->top : 0, rect ? rect->right : 0, rect ? rect->bottom : 0, flags);
    PixMapPtr pm;
    Ctx *c;

    if (!device_ok(d, &pm)) {
        lg("DrawPrivateNew: zariadenie nepodporovane");
        return kQANotSupported;
    }
    if (S->ensure3D() != 0) {
        lg("DrawPrivateNew: 3D init zlyhal");
        return kQAError;
    }
    c = (Ctx *)NewPtrSysClear(sizeof(Ctx));
    if (!c)
        return kQAOutOfMemory;
    c->dc = dc;
    c->id = S->nextCtxId++;
    c->flags = flags;
    c->gd = d->device.gDevice;
    c->rect = *rect;
    c->clip = clip && clip->clipType == kQAClipRgn ? clip->clip.clipRgn : 0;
    c->width = rect->right - rect->left;
    c->height = rect->bottom - rect->top;
    c->dbl = (flags & kQAContext_DoubleBuffer) != 0;
    c->hasZ = !(flags & kQAContext_NoZBuffer);
    screen_geom(c);
    if (c->width <= 0 || c->height <= 0 || c->width > 2048 || c->height > 2048) {
        DisposePtr((Ptr)c);
        return kQAParamErr;
    }
    c->bpp = pm->pixelSize;
    c->z24 = c->bpp == 32;                          /* na obrazovku (bez double buffra) pri 16 bpp Z16 */
    if (c->dbl) {
        /* rave-16: UT (z = 1 − 1/w) potrebuje Z24 aj v Thousands — Z16 nerozlíši vzdialené steny
         * od oblohy (obloha presvitá). Zeta má 4 B/pixel a pitch farby musí byť rovnaký. */
        c->z24 = c->bpp == 32 || !gZ16;
        c->backPitch = (c->width * (c->z24 ? 4 : c->bpp / 8) + 63) & ~63L;
        c->backOff = S->vramAlloc(c->backPitch * c->height, 4096);
        if (!c->backOff) {
            lg("DrawPrivateNew: malo VRAM pre zadny buffer");
            DisposePtr((Ptr)c);
            return kQAOutOfVideoMemory;
        }
    }
    if (c->hasZ) {
        long zw = c->dbl ? c->width : pm->bounds.right - pm->bounds.left;
        long zh = c->dbl ? c->height : pm->bounds.bottom - pm->bounds.top;
        c->zPitch = c->dbl ? c->backPitch : c->pitch;   /* rovnaký ako farba */
        (void)zw;
        c->zOff = S->vramAlloc(c->zPitch * zh, 4096);
        if (!c->zOff) {
            lg("DrawPrivateNew: malo VRAM pre z-buffer");
            free_ctx_mem(c);
            DisposePtr((Ptr)c);
            return kQAOutOfVideoMemory;
        }
    }
    c->si[kQATag_ZFunction] = c->hasZ ? kQAZFunction_LT : kQAZFunction_None;
    c->atiZWrite = 1;
    c->si[kQATag_Antialias] = kQAAntiAlias_Fast;
    c->si[kQATag_Blend] = kQABlend_PreMultiply;
    c->si[kQATag_TextureFilter] = kQATextureFilter_Fast;
    c->si[kQATag_TextureOp] = kQATextureOp_None;
    c->si[kQATagGL_BlendSrc] = 1;
    c->si[kQATagGL_BlendDst] = 0;
    c->sf[kQATag_Width] = 1.0f;
    c->sf[kQATag_ZMinScale] = 1.0f / 65535.0f;
    c->sf[kQATag_BitmapScale_x] = c->sf[kQATag_BitmapScale_y] = 1.0f;
    c->generation = (NVU32)-1;
    invalidate(c);

    dc->drawPrivate = (TQADrawPrivate *)c;
    dc->version = kQAVersion_1_6;
    dc->setFloat = SetFloat;
    dc->setInt = SetInt;
    dc->setPtr = SetPtr;
    dc->getFloat = GetFloat;
    dc->getInt = GetInt;
    dc->getPtr = GetPtr;
    dc->drawPoint = DrawPoint;
    dc->drawLine = DrawLine;
    dc->drawTriGouraud = DrawTriGouraud;
    dc->drawTriTexture = DrawTriTexture;
    dc->drawVGouraud = DrawVGouraud;
    dc->drawVTexture = DrawVTexture;
    dc->drawBitmap = DrawBitmap;
    dc->renderStart = RenderStart;
    dc->renderEnd = RenderEnd;
    dc->renderAbort = RenderAbort;
    dc->flush = Flush;
    dc->sync = Sync;
    dc->submitVerticesGouraud = SubmitVerticesGouraud;
    dc->submitVerticesTexture = SubmitVerticesTexture;
    dc->drawTriMeshGouraud = DrawTriMeshGouraud;
    dc->drawTriMeshTexture = DrawTriMeshTexture;
    dc->setNoticeMethod = SetNoticeMethod;
    dc->getNoticeMethod = GetNoticeMethod;
    dc->submitMultiTextureParams = SubmitMultiTextureParams;
    dc->accessDrawBuffer = AccessDrawBuffer;
    dc->accessDrawBufferEnd = AccessDrawBufferEnd;
    dc->accessZBuffer = AccessZBuffer;
    dc->accessZBufferEnd = AccessZBufferEnd;
    dc->clearDrawBuffer = ClearDrawBuffer;
    dc->clearZBuffer = ClearZBuffer;
    dc->textureFromContext = TextureFromContext;
    dc->bitmapFromContext = BitmapFromContext;
    dc->busy = Busy;
    dc->swapBuffers = SwapBuffers;

    lg("kontext %u: %dx%d %d bpp z%d na (%d,%d) flags %x %s%s back %x pitch %u z %x pitch %u", c->id, c->width, c->height, c->bpp, c->z24 ? 24 : 16,
       rect->left, rect->top, flags, c->dbl ? "double " : "", c->hasZ ? "z" : "", c->backOff, c->backPitch,
       c->zOff, c->zPitch);
    return kQANoErr;
}

static void DrawPrivateDelete(TQADrawPrivate *p)
{
    CL("DrawPrivateDelete");
    Ctx *c = (Ctx *)p;
    if (!c)
        return;
    if (S) {
        gpu_sync();
        lg("kontext %u koniec: %u snimok, %u trojuholnikov, %u textur uvolnenych z VRAM, volne %u B",
           c->id, c->frames, c->tris, gEvictions, S->vramFreeBytes());
        free_ctx_mem(c);
        if (S->boundCtxId == c->id)
            S->boundCtxId = 0;
    }
    DisposePtr((Ptr)c);
}

/* ---- textúry a bitmapy ------------------------------------------------------- */

static NVU32 log2u(NVU32 v)
{
    NVU32 n = 0;
    while ((1UL << n) < v)
        n++;
    return n;
}

static NVU32 texel(const TQATexture *t, const TQAImage *im, long x, long y)
{
    const UInt8 *row = (const UInt8 *)im->pixmap + y * im->rowBytes;
    NVU32 v, a, r, g, b;

    switch (t->pixelType) {
    case kQAPixel_ARGB32:
        return ((const NVU32 *)row)[x];
    case kQAPixel_RGB32:
        return ((const NVU32 *)row)[x] | 0xFF000000;
    case kQAPixel_RGB24:                            /* OpenGLRendererATI: R, G, B bajty */
        return 0xFF000000 | ((NVU32)row[x * 3] << 16) | ((NVU32)row[x * 3 + 1] << 8) | row[x * 3 + 2];
    case kQAPixel_ARGB16:
    case kQAPixel_RGB16:
        v = ((const UInt16 *)row)[x];
        r = (v >> 10) & 31;
        g = (v >> 5) & 31;
        b = v & 31;
        a = t->pixelType == kQAPixel_RGB16 || (v & 0x8000) ? 0xFF : 0;
        return (a << 24) | (((r << 3) | (r >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((b << 3) | (b >> 2));
    case kQAPixel_ARGB16_4444:
        v = ((const UInt16 *)row)[x];
        return (((v >> 12) & 15) * 0x11u << 24) | (((v >> 8) & 15) * 0x11u << 16) |
               (((v >> 4) & 15) * 0x11u << 8) | ((v & 15) * 0x11u);
    case kQAPixel_I8:
        v = row[x];
        return 0xFF000000 | (v << 16) | (v << 8) | v;
    case kQAPixel_AI16_88:
        v = ((const UInt16 *)row)[x];
        return ((v >> 8) << 24) | ((v & 0xff) * 0x010101u);
    case kQAPixel_CL8:
        v = row[x];
        if (!t->clut)
            return 0xFF000000 | (v * 0x010101u);
        if (t->clut->transparent && v == 0)
            return 0;
        return t->clut->argb[v];
    case kQAPixel_CL4:
        v = (row[x >> 1] >> ((x & 1) ? 0 : 4)) & 15;
        if (!t->clut)
            return 0xFF000000 | (v * 0x111111u);
        if (t->clut->transparent && v == 0)
            return 0;
        return t->clut->argb[v];
    case kQAPixel_Alpha1:
        return (row[x >> 3] & (0x80 >> (x & 7))) ? 0xFFFFFFFF : 0;
    }
    return 0xFFFF00FF;
}

/* NV swizzle: bity x a y sa striedajú (x prvý), zvyšok väčšieho rozmeru nasleduje */
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


static int gTexSwap16;      /* boot-args nv34tex16swap: prehodiť dvojice 16-bit texelov (test) */

/* kópia z RAM do VRAM (GPU textúru práve nečíta: nová alebo po sync) */
static void vram_put(TQATexture *t)
{
    volatile NVU32 *d = (volatile NVU32 *)(S->a->vram + t->off);
    const NVU32 *src = (const NVU32 *)t->sys;
    NVU32 i, n = (t->size + 3) >> 2;
    for (i = 0; i < n; i++)
        d[i] = src[i];
    __asm__ volatile("sync" ::: "memory");
}

/* VRAM pre textúru; pri nedostatku po sync uvoľní najdlhšie nepoužité textúry */
static int tex_resident(TQATexture *t)
{
    if (t->off)
        return 1;
    if (!t->sys)
        return 0;
    t->off = S->vramAlloc(t->size, 256);
    if (!t->off) {
        TQATexture *v, *lru;
        gpu_sync();
        while (!t->off) {
            lru = 0;
            for (v = gTexList; v; v = v->next)
                if (v != t && v->off && (!lru || v->lastUse < lru->lastUse))
                    lru = v;
            if (!lru)
                break;
            S->vramFree(lru->off);
            lru->off = 0;
            if (lru->alt) {
                S->vramFree(lru->alt);
                lru->alt = 0;
            }
            gEvictions++;
            t->off = S->vramAlloc(t->size, 256);
        }
        gTexEpoch++;
        if (gEvictLogged < 4) {
            gEvictLogged++;
            lg("VRAM plna: %u textur uvolnenych, volne %u B, %s", gEvictions, S->vramFreeBytes(),
               t->off ? "ok" : "stale malo");
        }
        if (!t->off)
            return 0;
    }
    vram_put(t);
    return 1;
}

/* rave-23: swizzle cez tabuľky — bity x a y idú na pevné pozície, offset = sx[x] | sy[y] */
static NVU32 gSwX[2048], gSwY[2048];

static void swz_tables(NVU32 w, NVU32 h, NVU32 lw, NVU32 lh)
{
    NVU32 i;
    for (i = 0; i < w; i++)
        gSwX[i] = swz(i, 0, lw, lh);
    for (i = 0; i < h; i++)
        gSwY[i] = swz(0, i, lw, lh);
}

static void upload(TQATexture *t, const TQAImage *images)
{
    NVU32 *base = (NVU32 *)t->sys;
    UInt16 *base16 = (UInt16 *)base;
    NVU32 lvl, x, y, w = t->w, h = t->h, lw = t->log2w, lh = t->log2h;
    /* RAVE/QD3D: textúra má počiatok (u,v) = (0,0) vľavo DOLE (rave-4: UT menu a text hore
     * nohami); kQATexture_FlipOrigin = vľavo hore. Bitmapy sa kreslia zhora nadol. */
    int flip = t->rect ? (t->flags & kQATexture_FlipOrigin) != 0 : !(t->flags & kQATexture_FlipOrigin);

    if (t->bpp == 2) {
        for (lvl = 0; lvl < t->levels; lvl++) {
            const TQAImage *im = &images[lvl];
            swz_tables(w, h, lw, lh);
            for (y = 0; y < h; y++) {
                long sy = flip ? (long)(h - 1 - y) : (long)y;
                const UInt16 *row = (const UInt16 *)((const UInt8 *)im->pixmap + sy * im->rowBytes);
                NVU32 oy = gSwY[y], set = t->pixelType == kQAPixel_RGB16 ? 0x8000 : 0;
                for (x = 0; x < w; x++)
                    base16[(oy | gSwX[x]) ^ gTexSwap16] = row[x] | set;
            }
            base16 += w * h;
            if (w > 1) { w >>= 1; lw--; }
            if (h > 1) { h >>= 1; lh--; }
        }
        if (t->off)
            vram_put(t);
        return;
    }

    for (lvl = 0; lvl < t->levels; lvl++) {
        const TQAImage *im = &images[lvl];
        for (y = 0; y < h; y++) {
            long sy = flip ? (long)(h - 1 - y) : (long)y;
            if (t->rect) {
                NVU32 *row = (NVU32 *)((NVU8 *)base + y * t->pitch);
                for (x = 0; x < w; x++)
                    row[x] = texel(t, im, x, sy);
            } else {
                NVU32 oy;
                if (y == 0)
                    swz_tables(w, h, lw, lh);
                oy = gSwY[y];
                for (x = 0; x < w; x++)
                    base[oy | gSwX[x]] = texel(t, im, x, sy);
            }
        }
        base += w * h;
        if (w > 1) { w >>= 1; lw--; }
        if (h > 1) { h >>= 1; lh--; }
    }
    if (t->off)
        vram_put(t);
}

/* nový obsah úrovne 0 (ATI[2]); nižšie mipmapy ostanú */
/* rave-22: bez čakania na GPU (voda v UT sa mení každú snímku → sync spomaľoval) — druhá
 * oblasť VRAM sa strieda s prvou; push buffer (32 KB) je kratší ako snímka, takže GPU staršiu
 * oblasť už dávno dočítal, keď na ňu príde rad */
static void reupload_level0(TQATexture *t, const TQAImage *img)
{
    NVU32 levels = t->levels, off = t->off, tmp;
    t->levels = 1;
    t->off = 0;
    upload(t, img);                                 /* iba do RAM */
    t->levels = levels;
    t->off = off;
    if (!t->off)
        return;                                     /* do VRAM pri ďalšom použití */
    if (!t->alt)
        t->alt = S->vramAlloc(t->size, 256);
    if (!t->alt) {
        gpu_sync();
        vram_put(t);
        return;
    }
    tmp = t->off;
    t->off = t->alt;
    t->alt = tmp;
    vram_put(t);
    gTexEpoch++;                                    /* kontexty znova nastavia TEX_OFFSET */
}

static TQAError new_image(unsigned long flags, long pixelType, const TQAImage images[], int rect,
                          TQATexture **out)
{
    TQATexture *t;
    NVU32 size, w, h, i;

    *out = 0;
    if (!shared() || S->state3D == 2 || !images || images[0].width <= 0 || images[0].height <= 0)
        return kQAParamErr;
    t = (TQATexture *)NewPtrSysClear(sizeof(TQATexture));
    if (!t)
        return kQAOutOfMemory;
    t->magic = 'NVtx';
    t->w = images[0].width;
    t->h = images[0].height;
    t->pixelType = pixelType;
    t->flags = flags;
    t->rect = rect;
    t->log2w = log2u(t->w);
    t->log2h = log2u(t->h);
    t->levels = 1;
    if (t->w > 2048 || t->h > 2048) {
        lg("textura %ux%u prilis velka", t->w, t->h);
        DisposePtr((Ptr)t);
        return kQANotSupported;
    }
    if (!rect && ((1UL << t->log2w) != t->w || (1UL << t->log2h) != t->h)) {
        lg("textura %ux%u nie je mocnina 2", t->w, t->h);
        DisposePtr((Ptr)t);
        return kQANotSupported;
    }
    if (!rect && (flags & kQATexture_Mipmap))
        t->levels = (t->log2w > t->log2h ? t->log2w : t->log2h) + 1;
    t->fmt = 0x06;
    t->bpp = 4;
    if (!rect && (pixelType == kQAPixel_ARGB16 || pixelType == kQAPixel_RGB16)) {
        t->fmt = 0x02;                          /* A1R5G5B5: polovica VRAM (rave-4: UT minul VRAM) */
        t->bpp = 2;
    } else if (!rect && pixelType == kQAPixel_ARGB16_4444) {
        t->fmt = 0x04;
        t->bpp = 2;
    }
    if (rect) {
        t->pitch = (t->w * 4 + 63) & ~63UL;
        size = t->pitch * t->h;
    } else {
        size = 0;
        for (i = 0, w = t->w, h = t->h; i < t->levels; i++) {
            size += w * h * t->bpp;
            if (w > 1) w >>= 1;
            if (h > 1) h >>= 1;
        }
    }
    t->size = size;
    t->sys = NewPtr((size + 3) & ~3UL);         /* halda aplikácie (UT má desiatky MB) */
    if (!t->sys)
        t->sys = NewPtrSys((size + 3) & ~3UL);
    if (!t->sys) {
        lg("textura %ux%u: malo RAM", t->w, t->h);
        DisposePtr((Ptr)t);
        return kQAOutOfMemory;
    }
    if (pixelType == kQAPixel_CL8 || pixelType == kQAPixel_CL4)
        for (i = 0; i < t->levels && i < 12; i++)
            t->src[i] = images[i];              /* nahrá sa znova po pripojení CLUT */
    CL(" new_image: sys %x size %u levels %u rb %d", (NVU32)t->sys, t->size, t->levels, images[0].rowBytes);
    upload(t, images);
    CL(" new_image: upload ok");
    {
        NVU32 k, n = t->w * t->h, z = 0;
        for (k = 0; k < n; k++)
            if (t->bpp == 2 ? !(((UInt16 *)t->sys)[k] & (t->fmt == 0x04 ? 0xF000 : 0x8000))
                            : !(((NVU32 *)t->sys)[k] >> 24))
                z++;
        t->alpha0 = n ? z * 100 / n : 0;
    }
    t->next = gTexList;
    if (gTexList)
        gTexList->prev = t;
    gTexList = t;
    t->lastUse = gFrame;
    tex_resident(t);                            /* hneď do VRAM, ak je miesto */
    CL(" new_image: hotovo off %x", t->off);
    *out = t;
    return kQANoErr;
}

static TQAError TextureNew(unsigned long flags, TQAImagePixelType type, const TQAImage images[], TQATexture **out)
{
    CL("TextureNew typ %d %dx%d fl %x", type, images ? images[0].width : 0, images ? images[0].height : 0, flags);
    if (first_time(0xA0000000 | (type << 16) | (flags & 0xffff)))
        lg("textura typ %d %dx%d flags %x", type, images ? images[0].width : 0, images ? images[0].height : 0, flags);
    return new_image(flags, type, images, 0, out);
}

static TQAError TextureDetach(TQATexture *t)
{
    (void)t;
    return kQANoErr;
}

static void TextureDelete(TQATexture *t)
{
    CL("TextureDelete %x", (NVU32)t);
    if (!t || t->magic != 'NVtx' || !S)
        return;
    gpu_sync();                                     /* GPU už textúru nečíta */
    if (t->off)
        S->vramFree(t->off);
    if (t->alt)
        S->vramFree(t->alt);
    if (t->prev)
        t->prev->next = t->next;
    else if (gTexList == t)
        gTexList = t->next;
    if (t->next)
        t->next->prev = t->prev;
    if (t->sys)
        DisposePtr((Ptr)t->sys);
    t->magic = 0;
    S->boundCtxId = 0;                              /* hwTex mohol ukazovať na ňu */
    DisposePtr((Ptr)t);
}

static TQAError BitmapNew(unsigned long flags, TQAImagePixelType type, const TQAImage *image, TQABitmap **out)
{
    CL("BitmapNew typ %d", type);
    if (first_time(0xB0000000 | type))
        lg("bitmapa typ %d %dx%d flags %x", type, image ? image->width : 0, image ? image->height : 0, flags);
    return new_image(flags & kQABitmap_FlipOrigin ? kQATexture_FlipOrigin : 0, type, image, 1,
                     (TQATexture **)out);
}

static TQAError BitmapDetach(TQABitmap *b)
{
    (void)b;
    return kQANoErr;
}

static void BitmapDelete(TQABitmap *b)
{
    TextureDelete((TQATexture *)b);
}

static TQAError ColorTableNew(TQAColorTableType type, void *data, long transparent, TQAColorTable **out)
{
    TQAColorTable *t = (TQAColorTable *)NewPtrSysClear(sizeof(TQAColorTable));
    int i, n = type == kQAColorTable_CL4_RGB32 ? 16 : 256;
    if (!t)
        return kQAOutOfMemory;
    for (i = 0; i < n; i++)
        t->argb[i] = ((NVU32 *)data)[i] | 0xFF000000;
    t->transparent = transparent;
    *out = t;
    return kQANoErr;
}

static void ColorTableDelete(TQAColorTable *t)
{
    if (t)
        DisposePtr((Ptr)t);
}

static TQAError TextureBindColorTable(TQATexture *t, TQAColorTable *clut)
{
    if (!t || t->magic != 'NVtx')
        return kQAParamErr;
    t->clut = clut;
    if (t->src[0].pixmap) {
        gpu_sync();
        upload(t, t->src);                          /* do RAM a ak je rezidentná aj do VRAM */
    }
    return kQANoErr;
}

static TQAError BitmapBindColorTable(TQABitmap *b, TQAColorTable *clut)
{
    return TextureBindColorTable((TQATexture *)b, clut);
}

/* ---- registrácia ----------------------------------------------------------- */

static TQAError EngineGetMethod(long tag, TQAEngineMethod *m)
{
    switch (tag) {
    case kQADrawPrivateNew:         m->drawPrivateNew = DrawPrivateNew; break;
    case kQADrawPrivateDelete:      m->drawPrivateDelete = DrawPrivateDelete; break;
    case kQAEngineCheckDevice:      m->engineCheckDevice = EngineCheckDevice; break;
    case kQAEngineGestalt:          m->engineGestalt = EngineGestalt; break;
    case kQATextureNew:             m->textureNew = TextureNew; break;
    case kQATextureDetach:          m->textureDetach = TextureDetach; break;
    case kQATextureDelete:          m->textureDelete = TextureDelete; break;
    case kQABitmapNew:              m->bitmapNew = BitmapNew; break;
    case kQABitmapDetach:           m->bitmapDetach = BitmapDetach; break;
    case kQABitmapDelete:           m->bitmapDelete = BitmapDelete; break;
    case kQAColorTableNew:          m->colorTableNew = ColorTableNew; break;
    case kQAColorTableDelete:       m->colorTableDelete = ColorTableDelete; break;
    case kQATextureBindColorTable:  m->textureBindColorTable = TextureBindColorTable; break;
    case kQABitmapBindColorTable:   m->bitmapBindColorTable = BitmapBindColorTable; break;
    default:
        return kQANotSupported;
    }
    return kQANoErr;
}

static int boot_arg(const char *word)
{
    RegEntryID opt;
    char args[256];
    UInt32 size = sizeof(args) - 1;
    int found = 0, i, j;

    if (RegistryCStrEntryLookup(0, "Devices:device-tree:options", &opt) != 0)
        return 0;
    if (RegistryPropertyGet(&opt, "boot-args", args, &size) == 0) {
        args[size] = 0;
        for (i = 0; args[i] && !found; i++) {
            for (j = 0; word[j] && args[i + j] == word[j]; j++)
                ;
            found = !word[j];
        }
    }
    RegistryEntryIDDispose(&opt);
    return found;
}

/* meno aktuálneho procesu začína `prefix` (UT: ATI cesta RaveDrv, rave-19) */
static int process_is(const char *prefix)
{
    ProcessSerialNumber psn;
    ProcessInfoRec info;
    Str31 name;
    int i;

    if (GetCurrentProcess(&psn) != noErr)
        return 0;
    memset(&info, 0, sizeof(info));
    info.processInfoLength = sizeof(info);
    info.processName = name;
    name[0] = 0;
    if (GetProcessInformation(&psn, &info) != noErr)
        return 0;
    for (i = 0; prefix[i]; i++)
        if (i >= name[0] || name[i + 1] != (unsigned char)prefix[i])
            return 0;
    return 1;
}

/* CFM init rutina (pef.py setinit): RAVE manager knižnicu načíta pri QAInit */
OSErr TtRegister(void *initBlock)
{
    (void)initBlock;
    if (!shared()) {
        return noErr;                               /* bez QD Accel / NV34 sa engine neregistruje */
    }
    gSpoofATI = boot_arg("nv34raveati") || process_is("Unreal") || process_is("Quake");
    /* minigl-9: UT bol so zmenami z gl-q2 (ATI gestalty, BoundToDevice) príliš svetlý → len Quake */
    gAtiGL = process_is("Quake");
    /* minigl-13: log do súboru a záznam snímky len na požiadanie (spomaľujú hru) */
    gFileLog = boot_arg("nv34filelog");
    gTraceOn = boot_arg("nv34trace");
    gTexSwap16 = boot_arg("nv34tex16swap");
    gZ16 = boot_arg("nv34z16");
    gNo2x = boot_arg("nv34no2x");
    gImmediate = boot_arg("nv34immediate");
    gLong = boot_arg("nv34ravelong");
    gStats = boot_arg("nv34ravestats");
    {
        ProcessSerialNumber psn;
        ProcessInfoRec info;
        Str31 name;
        name[0] = 0;
        memset(&info, 0, sizeof(info));
        info.processInfoLength = sizeof(info);
        info.processName = name;
        if (GetCurrentProcess(&psn) == noErr)
            GetProcessInformation(&psn, &info);
        name[name[0] < 31 ? name[0] + 1 : 31] = 0;
        lg("NV34 RAVE: registracia (3D stav %u) proces \"%s\"%s", S->state3D, (char *)name + 1,
           gSpoofATI ? ", vendor ATI" : "");
    }
    return QARegisterEngine(EngineGetMethod) == kQANoErr ? noErr : paramErr;
}

void TtDeRegister(void)
{
}

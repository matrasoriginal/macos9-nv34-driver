/*
 * NV34 MiniGL — AGL (Apple GL 2.x for Mac OS 9): pixel formats, renderer info, contexts,
 * drawable (window = CGrafPtr), buffer swap. A context draws into a back buffer in VRAM
 * and aglSwapBuffers copies it with a 2D blit through the window's visible region (like NV34 RAVE).
 */
#include <MacMemory.h>
#include <Processes.h>
#include <Multiverse.h>
#include "minigl.h"

#define AGL_NO_ERROR        0
#define AGL_BAD_ATTRIBUTE   10000
#define AGL_BAD_PROPERTY    10001
#define AGL_BAD_PIXELFMT    10002
#define AGL_BAD_RENDINFO    10003
#define AGL_BAD_CONTEXT     10004
#define AGL_BAD_DRAWABLE    10005
#define AGL_BAD_VALUE       10008
#define AGL_BAD_FULLSCREEN  10012
#define AGL_BAD_ALLOC       10016

#define RENDERER_ID         0x00022600      /* our own (Apple: generic 0x20200, ATI 0x21000) */

typedef struct PixFmt {
    NVU32   magic;                          /* 'NVpf' */
    int     dbl, depth, stencil, fullscreen;
    GDHandle gd;
} PixFmt;

typedef struct RendInfo {
    NVU32   magic;                          /* 'NVri' */
    GDHandle gd;
} RendInfo;

static GLenum gAglErr;
static GLCtx *gCtxs[16];             /* to release VRAM at exit */

static GLboolean fail(GLenum e)
{
    gAglErr = e;
    return GL_FALSE;
}

static GDHandle main_gd(void)
{
    return GetMainDevice();
}

static int depth_of(GDHandle gd)
{
    return gd && *gd ? (*(*gd)->gdPMap)->pixelSize : 32;
}

/* ---- pixel format --------------------------------------------------------------- */

static int bool_attr(GLint a)
{
    switch (a) {
    case AGL_ALL_RENDERERS: case AGL_RGBA: case AGL_DOUBLEBUFFER: case AGL_STEREO:
    case AGL_MINIMUM_POLICY: case AGL_MAXIMUM_POLICY: case AGL_OFFSCREEN: case AGL_FULLSCREEN:
    case AGL_SINGLE_RENDERER: case AGL_NO_RECOVERY: case AGL_ACCELERATED: case AGL_CLOSEST_POLICY:
    case AGL_ROBUST: case AGL_BACKING_STORE: case 77: case AGL_MP_SAFE: case AGL_WINDOW:
    case AGL_MULTISCREEN: case AGL_VIRTUAL_SCREEN: case AGL_COMPLIANT: case 90:
        return 1;
    }
    return 0;
}

void *aglChoosePixelFormat(const GDHandle *gdevs, GLint ndev, const GLint *attribs)
{
    PixFmt *p;
    int i;

    if (gDbg)
        lg("aglChoosePixelFormat: %d devices", ndev);
    p = (PixFmt *)NewPtrClear(sizeof(PixFmt));
    if (!p) {
        gAglErr = AGL_BAD_ALLOC;
        return 0;
    }
    p->magic = 'NVpf';
    p->gd = gdevs && ndev > 0 ? gdevs[0] : main_gd();
    for (i = 0; attribs && attribs[i] != AGL_NONE && i < 64; i++) {
        GLint a = attribs[i], v = 1;
        if (!bool_attr(a))
            v = attribs[++i];
        if (gDbg)
            lg("  attribute %d = %d", a, v);
        switch (a) {
        case AGL_DOUBLEBUFFER: p->dbl = 1; break;
        case AGL_DEPTH_SIZE:   p->depth = v <= 0 ? 0 : v <= 16 ? 16 : 24; break;
        case AGL_STENCIL_SIZE: p->stencil = v; break;
        case AGL_FULLSCREEN:   p->fullscreen = 1; break;
        case AGL_STEREO:
        case AGL_OFFSCREEN:
            lg("  -> unsupported");
            DisposePtr((Ptr)p);
            gAglErr = AGL_BAD_ATTRIBUTE;
            return 0;
        }
    }
    if (p->stencil)
        lg("  stencil %d unsupported (0)", p->stencil);
    if (depth_of(p->gd) < 16)
        lg("  screen %d bpp: needs Thousands/Millions", depth_of(p->gd));
    return p;
}

void aglDestroyPixelFormat(void *pix)
{
    PixFmt *p = (PixFmt *)pix;
    if (p && p->magic == 'NVpf') {
        p->magic = 0;
        DisposePtr((Ptr)p);
    }
}

void *aglNextPixelFormat(void *pix)
{
    (void)pix;
    return 0;
}

GLboolean aglDescribePixelFormat(void *pix, GLint attrib, GLint *value)
{
    PixFmt *p = (PixFmt *)pix;
    int bpp;
    if (!p || p->magic != 'NVpf')
        return fail(AGL_BAD_PIXELFMT);
    bpp = depth_of(p->gd);
    switch (attrib) {
    case AGL_RGBA: case AGL_ACCELERATED: case AGL_WINDOW: case AGL_FULLSCREEN: case AGL_COMPLIANT:
    case AGL_MP_SAFE: case AGL_NO_RECOVERY: case AGL_SINGLE_RENDERER:
        *value = 1; break;
    case AGL_DOUBLEBUFFER: *value = p->dbl; break;
    case AGL_DEPTH_SIZE:   *value = p->depth; break;
    case AGL_BUFFER_SIZE: case AGL_PIXEL_SIZE: *value = bpp; break;
    case AGL_RED_SIZE: case AGL_GREEN_SIZE: case AGL_BLUE_SIZE: *value = bpp == 16 ? 5 : 8; break;
    case AGL_ALPHA_SIZE:   *value = bpp == 16 ? 0 : 8; break;
    case AGL_RENDERER_ID:  *value = RENDERER_ID; break;
    default:               *value = 0;
    }
    return GL_TRUE;
}

GDHandle *aglDevicesOfPixelFormat(void *pix, GLint *ndevs)
{
    static GDHandle devs[1];
    PixFmt *p = (PixFmt *)pix;
    devs[0] = p && p->magic == 'NVpf' && p->gd ? p->gd : main_gd();
    if (ndevs)
        *ndevs = 1;
    return devs;
}

/* ---- renderer info ------------------------------------------------------------ */

void *aglQueryRendererInfo(const GDHandle *gdevs, GLint ndev)
{
    RendInfo *r = (RendInfo *)NewPtrClear(sizeof(RendInfo));
    if (!r) {
        gAglErr = AGL_BAD_ALLOC;
        return 0;
    }
    r->magic = 'NVri';
    r->gd = gdevs && ndev > 0 ? gdevs[0] : main_gd();
    return r;
}

void aglDestroyRendererInfo(void *rend)
{
    RendInfo *r = (RendInfo *)rend;
    if (r && r->magic == 'NVri') {
        r->magic = 0;
        DisposePtr((Ptr)r);
    }
}

void *aglNextRendererInfo(void *rend)
{
    (void)rend;
    return 0;
}

GLboolean aglDescribeRenderer(void *rend, GLint prop, GLint *value)
{
    RendInfo *r = (RendInfo *)rend;
    if (!r || r->magic != 'NVri')
        return fail(AGL_BAD_RENDINFO);
    switch (prop) {
    case AGL_RENDERER_ID:    *value = RENDERER_ID; break;
    case AGL_ACCELERATED: case AGL_COMPLIANT: case AGL_WINDOW: case AGL_FULLSCREEN: case AGL_MP_SAFE:
        *value = 1; break;
    case AGL_VIDEO_MEMORY:   *value = (GLint)vram_total(); break;
    case AGL_TEXTURE_MEMORY: *value = (GLint)(vram_total() / 2); break;
    case AGL_BUFFER_MODES:   *value = 0x1 | 0x4 | 0x8; break;          /* mono, single, double */
    case AGL_COLOR_MODES:    *value = 0x00001000 | 0x00008000; break;   /* RGB555 A8, ARGB8888 (roughly) */
    case AGL_DEPTH_MODES:    *value = 0x1 | 0x800; break;               /* 0, 24 bit */
    case AGL_STENCIL_MODES:  *value = 0x1; break;
    case AGL_MAX_AUX_BUFFERS: case AGL_MIN_LEVEL: case AGL_MAX_LEVEL:
    case AGL_ROBUST: case AGL_OFFSCREEN: case AGL_MULTISCREEN: case AGL_BACKING_STORE:
        *value = 0; break;
    default:
        if (first_time(0x60000000 | (NVU32)prop))
            lg("aglDescribeRenderer: property %d unknown", prop);
        *value = 0;
    }
    return GL_TRUE;
}

/* ---- full screen: hide the menu bar and control strip (hl-11) ---------------------------- */

static int gHidden, gStripWasVisible;

typedef void (*VoidFn)(void);

static VoidFn menu_fn(const char *name)
{
    static ConnectionID conn[2];
    static int tried;
    Str255 p, err;
    Ptr a = 0, mainAddr;
    SymClass cls;
    int i;
    if (!tried) {
        tried = 1;
        if (GetSharedLibrary("\pInterfaceLib", kPowerPCArch, kReferenceCFrag, &conn[0], &mainAddr, err) != noErr)
            conn[0] = 0;
        if (GetSharedLibrary("\pMenusLib", kPowerPCArch, kReferenceCFrag, &conn[1], &mainAddr, err) != noErr)
            conn[1] = 0;
    }
    for (i = 0; name[i]; i++)
        p[i + 1] = name[i];
    p[0] = i;
    for (i = 0; i < 2; i++)
        if (conn[i] && FindSymbol(conn[i], p, &a, &cls) == noErr && a)
            return (VoidFn)a;
    return 0;
}

/* hl-12: no HideMenuBar (Mac OS 9 does not have it in InterfaceLib) — the classic menu bar height of 0
 * (FindWindow then does not report a click at the top as inMenuBar), the original one on restore */
static short gSavedMBar = -1;

/* Control Strip: _ControlStripDispatch (0xAAF2), SBIsControlStripVisible 0x0000,
 * SBShowHideControlStrip 0x0101 (D0 selector, Pascal stack) */
static int strip(int op, int show)
{
    long attr = 0;
    UniversalProcPtr trap;
    if (Gestalt('sdev', &attr) != noErr || !(attr & 1))
        return -1;
    trap = (UniversalProcPtr)NGetTrapAddress(0xAAF2, kToolboxTrapType);
    if (!trap)
        return -1;
    if (op == 0)                                    /* visible? result 1 B, selector 2 B */
        return (int)(CallUniversalProc(trap, 8 | (1 << 4) | (2 << 6), 0x0000) & 0xff);
    CallUniversalProc(trap, 8 | (2 << 6) | (1 << 8), 0x0101, (unsigned char)(show != 0));
    return 0;
}

static void full_hide(int hide)
{
    VoidFn f;
    if (hide == gHidden)
        return;
    gHidden = hide;
    f = menu_fn(hide ? "HideMenuBar" : "ShowMenuBar");
    if (f) {
        f();
    } else if (hide) {
        gSavedMBar = LMGetMBarHeight();
        LMSetMBarHeight(0);
    } else if (gSavedMBar >= 0) {
        LMSetMBarHeight(gSavedMBar);
        gSavedMBar = -1;
        DrawMenuBar();
    }
    if (hide) {
        gStripWasVisible = strip(0, 0) == 1;
        if (gStripWasVisible)
            strip(1, 0);
    } else if (gStripWasVisible) {
        strip(1, 1);
    }
    lg("full screen: menu bar %s (%s), control strip %s", hide ? "hidden" : "restored",
       f ? "HideMenuBar" : "height 0", gStripWasVisible ? (hide ? "hidden" : "restored") : "was not visible");
}

/* ---- context and drawable ----------------------------------------------------------- */

static GDHandle device_of(PixMapPtr pm)
{
    GDHandle gd;
    for (gd = GetDeviceList(); gd; gd = GetNextDevice(gd))
        if ((*(*gd)->gdPMap)->baseAddr == pm->baseAddr)
            return gd;
    return 0;
}

/* window geometry → global rectangle, device, back buffer of the needed size */
static int update_geom(GLCtx *c)
{
    CGrafPtr port = c->port;
    PixMapPtr pm;
    Rect pr;
    int w, h, bpp;

    if (!port)
        return 0;
    if ((port->portVersion & 0xC000) != 0xC000) {
        if (first_time(0x61000000))
            lg("drawable is not a color port");
        return 0;
    }
    pm = *port->portPixMap;
    pr = port->portRect;
    c->gd = device_of(pm);
    if (!c->gd) {
        if (first_time(0x61100000))
            lg("drawable is not on the screen (baseAddr %x)", (NVU32)pm->baseAddr);
        return 0;
    }
    c->global.left = pr.left - pm->bounds.left;
    c->global.top = pr.top - pm->bounds.top;
    c->global.right = pr.right - pm->bounds.left;
    c->global.bottom = pr.bottom - pm->bounds.top;
    w = pr.right - pr.left;
    h = pr.bottom - pr.top;
    bpp = (*(*c->gd)->gdPMap)->pixelSize;
    {
        /* hl-2: log every window / screen change */
        static Rect lastG, lastScr;
        static int lastBpp;
        Rect scr = (*(*c->gd)->gdPMap)->bounds;
        if (lastG.left != c->global.left || lastG.top != c->global.top || lastG.right != c->global.right ||
            lastG.bottom != c->global.bottom || lastBpp != bpp || lastScr.right != scr.right || lastScr.bottom != scr.bottom) {
            lg("drawable: window (%d,%d)-(%d,%d), screen %dx%d %d bpp rb %d", c->global.left, c->global.top,
               c->global.right, c->global.bottom, scr.right - scr.left, scr.bottom - scr.top, bpp,
               (*(*c->gd)->gdPMap)->rowBytes & 0x3fff);
            lastG = c->global;
            lastScr = scr;
            lastBpp = bpp;
        }
    }
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048 || (bpp != 16 && bpp != 32)) {
        if (first_time(0x61200000 ^ (NVU32)(w << 12) ^ (NVU32)h ^ (NVU32)bpp))
            lg("drawable %dx%d %d bpp unsupported", w, h, bpp);
        return 0;
    }
    {
        Rect d = (*c->gd)->gdRect;
        c->full = c->global.left <= d.left && c->global.top <= d.top &&
                  c->global.right >= d.right && c->global.bottom >= d.bottom;
        if (c->full)
            full_hide(1);
    }
    if (c->hasRT && c->rt.w == w && c->rt.h == h && c->rt.bpp == bpp)
        return 1;
    gl_flush_batch();
    if (c->hasRT)
        hw_target_free(&c->rt);
    c->hasRT = 0;
    if (S->ensure3D() != 0) {
        lg("3D init failed");
        return 0;
    }
    if (!hw_target_alloc(&c->rt, w, h, bpp, c->depthBits)) {
        lg("not enough VRAM for a %dx%d back buffer (free %u KB)", w, h, S->vramFreeBytes() >> 10);
        return 0;
    }
    c->hasRT = 1;
    lg("back buffer %dx%d %d bpp z%d: color %x z %x pitch %u, window at (%d,%d), VRAM free %u KB", w, h, bpp,
       c->rt.z16 ? 16 : 24,
       c->rt.colorOff, c->rt.zOff, c->rt.colorPitch, c->global.left, c->global.top, S->vramFreeBytes() >> 10);
    if (!c->viewportSet) {
        c->s.viewport[0] = c->s.viewport[1] = 0;
        c->s.viewport[2] = w;
        c->s.viewport[3] = h;
        c->s.scissor[0] = c->s.scissor[1] = 0;
        c->s.scissor[2] = w;
        c->s.scissor[3] = h;
        c->viewportSet = 1;
    }
    return 1;
}

void *aglCreateContext(void *pix, void *share)
{
    PixFmt *p = (PixFmt *)pix;
    GLCtx *c;

    (void)share;
    if (!p || p->magic != 'NVpf') {
        gAglErr = AGL_BAD_PIXELFMT;
        return 0;
    }
    if (!shared() || !hw_init()) {
        lg("aglCreateContext: NV34 QD Accel / 3D not available");
        gAglErr = AGL_BAD_ALLOC;
        return 0;
    }
    c = (GLCtx *)NewPtrClear(sizeof(GLCtx));
    if (!c)
        c = (GLCtx *)NewPtrSysClear(sizeof(GLCtx));
    if (!c) {
        lg("aglCreateContext: out of memory (%u B)", (NVU32)sizeof(GLCtx));
        gAglErr = AGL_BAD_ALLOC;
        return 0;
    }
    c->magic = 'NVgc';
    c->dbl = p->dbl;
    c->depthBits = p->depth;
    c->rgn = NewRgn();
    gl_ctx_init(c);
    {
        int i;
        for (i = 0; i < 16; i++)
            if (!gCtxs[i]) {
                gCtxs[i] = c;
                break;
            }
    }
    lg("aglCreateContext %x: %s, depth %d", (NVU32)c, c->dbl ? "double" : "single", c->depthBits);
    return c;
}

static int ctx_ok(GLCtx *c)
{
    return c && c->magic == 'NVgc';
}

GLboolean aglDestroyContext(void *ctx)
{
    GLCtx *c = (GLCtx *)ctx;
    if (!ctx_ok(c))
        return fail(AGL_BAD_CONTEXT);
    if (C == c) {
        gl_flush_batch();
        C = 0;
    }
    if (c->full)
        full_hide(0);
    if (c->hasRT)
        hw_target_free(&c->rt);
    if (c->def.hw)
        hw_tex_free(c->def.hw);
    hw_pend_flush();
    if (c->rgn)
        DisposeRgn(c->rgn);
    {
        int i;
        for (i = 0; i < 16; i++)
            if (gCtxs[i] == c)
                gCtxs[i] = 0;
    }
    c->magic = 0;
    DisposePtr((Ptr)c);
    lg("aglDestroyContext %x", (NVU32)c);
    gl_mem_report();
    return GL_TRUE;
}

GLboolean aglCopyContext(void *src, void *dst, GLuint mask)
{
    GLCtx *a = (GLCtx *)src, *b = (GLCtx *)dst;
    (void)mask;
    if (!ctx_ok(a) || !ctx_ok(b))
        return fail(AGL_BAD_CONTEXT);
    b->s = a->s;
    return GL_TRUE;
}

GLboolean aglUpdateContext(void *ctx)
{
    GLCtx *c = (GLCtx *)ctx;
    static int n;
    if (!ctx_ok(c))
        return fail(AGL_BAD_CONTEXT);
    if (gDbg && n < 50) {
        n++;
        lg("aglUpdateContext %x", (NVU32)c);
    }
    if (c->port)
        update_geom(c);
    return GL_TRUE;
}

GLboolean aglSetCurrentContext(void *ctx)
{
    GLCtx *c = (GLCtx *)ctx;
    if (gDbg)
        lg("aglSetCurrentContext %x", (NVU32)c);
    if (c && !ctx_ok(c))
        return fail(AGL_BAD_CONTEXT);
    if (C != c)
        gl_flush_batch();
    C = c;
    return GL_TRUE;
}

void *aglGetCurrentContext(void)
{
    return C;
}

GLboolean aglSetDrawable(void *ctx, CGrafPtr port)
{
    GLCtx *c = (GLCtx *)ctx;
    if (!ctx_ok(c))
        return fail(AGL_BAD_CONTEXT);
    if (gDbg)
        lg("aglSetDrawable %x port %x", (NVU32)c, (NVU32)port);
    if (c == C)
        gl_flush_batch();
    c->port = port;
    if (!port) {
        if (c->full)
            full_hide(0);
        c->full = 0;
        if (c->hasRT)
            hw_target_free(&c->rt);
        c->hasRT = 0;
        return GL_TRUE;
    }
    if (!update_geom(c))
        return fail(AGL_BAD_DRAWABLE);
    return GL_TRUE;
}

CGrafPtr aglGetDrawable(void *ctx)
{
    GLCtx *c = (GLCtx *)ctx;
    return ctx_ok(c) ? c->port : 0;
}

GLboolean aglSetOffScreen(void *ctx, GLsizei w, GLsizei h, GLsizei rowbytes, GLvoid *base)
{
    lg("aglSetOffScreen %dx%d unsupported", w, h);
    (void)ctx; (void)rowbytes; (void)base;
    return fail(AGL_BAD_DRAWABLE);
}

GLboolean aglSetFullScreen(void *ctx, GLsizei w, GLsizei h, GLsizei freq, GLint device)
{
    lg("aglSetFullScreen %dx%d %d Hz device %d unsupported", w, h, freq, device);
    (void)ctx;
    return fail(AGL_BAD_FULLSCREEN);
}

GLboolean aglSetVirtualScreen(void *ctx, GLint screen)
{
    (void)ctx;
    return screen == 0 ? GL_TRUE : fail(AGL_BAD_VALUE);
}

GLint aglGetVirtualScreen(void *ctx)
{
    (void)ctx;
    return 0;
}

void aglGetVersion(GLint *major, GLint *minor)
{
    if (major) *major = 2;
    if (minor) *minor = 0;
}

GLboolean aglConfigure(GLenum pname, GLuint param)
{
    (void)pname; (void)param;
    return GL_TRUE;
}

/* fps-1: frame rate limit (60 by default) — above 60 fps games responded to the mouse
 * with a delay; nv34glfps30 = 30 fps, nv34glfps75 = 75 fps, nv34glnofps = no limit
 * fps-3: fixed schedule — a frame up to 4 periods late is caught up by the following ones (no waiting),
 * so the average is exactly 60; only a bigger stall (loading) restarts the schedule from now */
static NVU32 gFpsPeriod = 1, gFpsNext;
NVU32 gTFps, gFpsLate, gFpsReset;

static void fps_limit(void)
{
    NVU32 now, t0;
    if (gFpsPeriod == 1) {
        int fps = gl_boot_arg("nv34glnofps") ? 0 : gl_boot_arg("nv34glfps30") ? 30 :
                  gl_boot_arg("nv34glfps75") ? 75 : 60;
        gFpsPeriod = fps ? 18432000u / fps : 0;     /* timebase 18.432 MHz */
        lg("fps limit: %d (0 = no limit)", fps);
    }
    if (!gFpsPeriod)
        return;
    now = t0 = tb();
    if ((int)(now - gFpsNext) > (int)(4 * gFpsPeriod) || (int)(gFpsNext - now) > (int)gFpsPeriod) {
        gFpsNext = now;                             /* first frame or a big stall: start over from now */
        gFpsReset++;
    } else if ((int)(gFpsNext - now) <= 0) {
        gFpsLate++;                                 /* late: no waiting, catching up with the schedule */
    }
    while ((int)(gFpsNext - now) > 0)
        now = tb();
    gTFps += now - t0;
    gFpsNext += gFpsPeriod;
}

void aglSwapBuffers(void *ctx)
{
    GLCtx *c = (GLCtx *)ctx;
    NVU32 t0 = tb();
    if (!ctx_ok(c))
        return;
    if (gDbg)
        lg("swap %d: start", c->frames + 1);
    if (c == C)
        gl_flush_batch();
    if (!c->port || !update_geom(c) || !c->hasRT)
        return;
    if (c == C)
        gl_draw_fps();
    fps_limit();
    if (c->full) {
        /* hl-11: the whole window including the menu bar / control strip area (visRgn leaves them out) */
        hw_present(&c->rt, c->gd, &c->global, 0);
    } else {
        CopyRgn(c->port->visRgn, c->rgn);
        OffsetRgn(c->rgn, -(*c->port->portPixMap)->bounds.left, -(*c->port->portPixMap)->bounds.top);
        hw_present(&c->rt, c->gd, &c->global, c->rgn);
    }
    if (gDbg)
        lg("swap %d: done", c->frames + 1);
    gTSwap += tb() - t0;
    if (c == C)
        gl_frame_stats();
}

GLboolean aglEnable(void *ctx, GLenum pname)
{
    GLCtx *c = (GLCtx *)ctx;
    if (!ctx_ok(c))
        return fail(AGL_BAD_CONTEXT);
    if (first_time(0x63000000 | pname))
        lg("aglEnable %d", pname);
    if (pname == AGL_SWAP_RECT)
        c->swapRectOn = 1;
    return GL_TRUE;
}

GLboolean aglDisable(void *ctx, GLenum pname)
{
    GLCtx *c = (GLCtx *)ctx;
    if (!ctx_ok(c))
        return fail(AGL_BAD_CONTEXT);
    if (pname == AGL_SWAP_RECT)
        c->swapRectOn = 0;
    return GL_TRUE;
}

GLboolean aglIsEnabled(void *ctx, GLenum pname)
{
    GLCtx *c = (GLCtx *)ctx;
    if (!ctx_ok(c))
        return fail(AGL_BAD_CONTEXT);
    return pname == AGL_SWAP_RECT ? (GLboolean)c->swapRectOn : GL_FALSE;
}

GLboolean aglSetInteger(void *ctx, GLenum pname, const GLint *params)
{
    GLCtx *c = (GLCtx *)ctx;
    if (!ctx_ok(c))
        return fail(AGL_BAD_CONTEXT);
    if (pname == AGL_SWAP_RECT && params) {
        int i;
        for (i = 0; i < 4; i++)
            c->swapRect[i] = params[i];
    }
    if (first_time(0x62000000 | pname))
        lg("aglSetInteger %d = %d (ignored)", pname, params ? params[0] : 0);
    return GL_TRUE;
}

GLboolean aglGetInteger(void *ctx, GLenum pname, GLint *params)
{
    GLCtx *c = (GLCtx *)ctx;
    if (!ctx_ok(c))
        return fail(AGL_BAD_CONTEXT);
    switch (pname) {
    case AGL_SWAP_RECT: {
        int i;
        for (i = 0; i < 4; i++)
            params[i] = c->swapRect[i];
        break;
    }
    case AGL_BUFFER_RECT:
        params[0] = 0;
        params[1] = 0;
        params[2] = c->rt.w;
        params[3] = c->rt.h;
        break;
    default:
        params[0] = 0;
    }
    return GL_TRUE;
}

GLboolean aglUseFont(void *ctx, GLint fontID, short face, GLint size, GLint first, GLint count, GLint base)
{
    (void)ctx; (void)fontID; (void)face; (void)size; (void)first; (void)count; (void)base;
    lg("aglUseFont unsupported");
    return GL_FALSE;
}

GLenum aglGetError(void)
{
    GLenum e = gAglErr;
    gAglErr = AGL_NO_ERROR;
    return e;
}

const GLubyte *aglErrorString(GLenum code)
{
    switch (code) {
    case AGL_NO_ERROR:       return (const GLubyte *)"no error";
    case AGL_BAD_ATTRIBUTE:  return (const GLubyte *)"invalid pixel format attribute";
    case AGL_BAD_PIXELFMT:   return (const GLubyte *)"invalid pixel format";
    case AGL_BAD_CONTEXT:    return (const GLubyte *)"invalid context";
    case AGL_BAD_DRAWABLE:   return (const GLubyte *)"invalid drawable";
    case AGL_BAD_FULLSCREEN: return (const GLubyte *)"invalid full screen drawable";
    case AGL_BAD_ALLOC:      return (const GLubyte *)"memory allocation failure";
    }
    return (const GLubyte *)"NV34 MiniGL error";
}

void aglResetLibrary(void)
{
}

/* CFM init/term (pef.py setinit) */
/* q3-15: Power Manager — CPU clock. Mac OS 9 does not know the PowerBook6,2 and the CPU runs at
 * ~750 MHz instead of 1 GHz (measured in q3-13/14). boot-args nv34cpufull = SetProcessorSpeed(true),
 * nv34cputoggle = low speed first, then full speed (forces a switch). */
static void *lib_fn(const char *lib, const char *name)
{
    ConnectionID conn;
    Str255 p, err;
    Ptr a = 0, mainAddr;
    SymClass cls;
    int i;
    for (i = 0; lib[i]; i++)
        p[i + 1] = lib[i];
    p[0] = i;
    if (GetSharedLibrary(p, kPowerPCArch, kReferenceCFrag, &conn, &mainAddr, err) != noErr)
        return 0;
    for (i = 0; name[i]; i++)
        p[i + 1] = name[i];
    p[0] = i;
    if (FindSymbol(conn, p, &a, &cls) != noErr)
        return 0;
    return a;
}

static void cpu_speed(void)
{
    typedef short (*ShortFn)(void);
    typedef long (*LongFn)(void);
    typedef unsigned char (*BoolFn)(void);
    typedef unsigned char (*SetFn)(unsigned char);
    LongFn getCPU = (LongFn)lib_fn("InterfaceLib", "GetCPUSpeed");
    ShortFn cur = (ShortFn)lib_fn("PowerMgrLib", "CurrentProcessorSpeed");
    ShortFn max = (ShortFn)lib_fn("PowerMgrLib", "MaximumProcessorSpeed");
    ShortFn min = (ShortFn)lib_fn("PowerMgrLib", "MinimumProcessorSpeed");
    BoolFn full = (BoolFn)lib_fn("PowerMgrLib", "FullProcessorSpeed");
    BoolFn cyc = (BoolFn)lib_fn("PowerMgrLib", "IsProcessorCyclingEnabled");
    SetFn set = (SetFn)lib_fn("PowerMgrLib", "SetProcessorSpeed");
    int doFull = gl_boot_arg("nv34cpufull"), doToggle = gl_boot_arg("nv34cputoggle");

    lg("Power Manager: GetCPUSpeed %d, current %d, max %d, min %d MHz, full %d, cycling %d; SetProcessorSpeed %s",
       getCPU ? getCPU() : -1, cur ? cur() : -1, max ? max() : -1, min ? min() : -1, full ? full() : -1,
       cyc ? cyc() : -1, set ? "yes" : "no");
    lg("CPU measured: %u MHz", gl_cpu_mhz());
    if (!set || (!doFull && !doToggle))
        return;
    if (doToggle) {
        lg("SetProcessorSpeed(false) = %d", set(0));
        lg("  -> measured %u MHz, current %d", gl_cpu_mhz(), cur ? cur() : -1);
    }
    lg("SetProcessorSpeed(true) = %d", set(1));
    lg("  -> measured %u MHz, current %d, full %d", gl_cpu_mhz(), cur ? cur() : -1, full ? full() : -1);
}

/* mohaa-2: catching a process exception (error 2 in MOHAA) — PC, LR, DAR and the LR chain from the stack
 * are saved in the handler and written to the log only in MiniGLTerm (the File Manager is not safe in a handler).
 * Types per MachineExceptions.h (Retro68 does not have them). */
typedef struct { NVU32 hi, lo; } XWide;
typedef struct { XWide CTR, LR, PC; NVU32 CR, XER, MSR, MQ, kind, DSISR; XWide DAR, res; } XMachine;
typedef struct { XWide R[32]; } XRegs;
typedef struct { NVU32 area; NVU32 addr; long err; NVU32 ref; } XMemInfo;
typedef struct { NVU32 theKind; XMachine *m; XRegs *r; void *fpu; XMemInfo *mem; void *vec; } XInfo;
typedef long (*XHandler)(XInfo *);
extern pascal XHandler InstallExceptionHandler(XHandler h);

static XHandler gPrevExc;
static int gExcInst;
static NVU32 gExcN, gExcKind, gExcPC, gExcLR, gExcDAR, gExcDSISR, gExcMemAddr, gExcR[32], gExcChain[8];

static long exc_handler(XInfo *x)
{
    if (gExcN++ == 0 && x && x->m) {
        NVU32 sp, i;
        gExcKind = x->theKind;
        gExcPC = x->m->PC.lo;
        gExcLR = x->m->LR.lo;
        gExcDAR = x->m->DAR.lo;
        gExcDSISR = x->m->DSISR;
        gExcMemAddr = x->mem ? x->mem->addr : 0;
        if (x->r)
            for (i = 0; i < 32; i++)
                gExcR[i] = x->r->R[i].lo;
        sp = gExcR[1];
        for (i = 0; i < 8; i++) {                    /* frame chain: [sp] = previous sp, [sp+8] = saved LR */
            NVU32 next;
            if (!sp || (sp & 3) || sp - gExcR[1] > 0x100000)
                break;
            next = *(volatile NVU32 *)sp;
            if (next <= sp || (next & 3))
                break;
            gExcChain[i] = *(volatile NVU32 *)(next + 8);
            sp = next;
        }
    }
    if (gPrevExc)
        return gPrevExc(x);
    return -1;                                      /* not handled: pass on to the system */
}

static void exc_report(void)
{
    int i;
    if (!gExcN)
        return;
    lg("EXCEPTION %ux: kind %u, PC %x, LR %x, DAR %x, DSISR %x, address %x; MiniGL code: gl_draw_fps at %x, aglSwapBuffers at %x",
       gExcN, gExcKind, gExcPC, gExcLR, gExcDAR, gExcDSISR, gExcMemAddr,
       *(NVU32 *)(void *)gl_draw_fps, *(NVU32 *)(void *)aglSwapBuffers);
    for (i = 0; i < 32; i += 8)
        lg("  r%d-r%d: %x %x %x %x %x %x %x %x", i, i + 7, gExcR[i], gExcR[i + 1], gExcR[i + 2], gExcR[i + 3],
           gExcR[i + 4], gExcR[i + 5], gExcR[i + 6], gExcR[i + 7]);
    lg("  stack LR: %x %x %x %x %x %x %x %x", gExcChain[0], gExcChain[1], gExcChain[2], gExcChain[3],
       gExcChain[4], gExcChain[5], gExcChain[6], gExcChain[7]);
}

OSErr MiniGLInit(void *initBlock)
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
    lg("==== NV34 MiniGL 1: loaded in process \"%s\", NV34 %s, 3D state %u", (char *)name + 1,
       shared() ? "yes" : "no", S ? S->state3D : 0);
    /* CFragSystem7InitBlock: contextID, closureID, connectionID, fragLocator { where, fileSpec, … } */
    if (initBlock && (((const NVU32 *)initBlock)[3] == 1 || ((const NVU32 *)initBlock)[3] == 2))
        gl_cfg_load((const FSSpec *)((const NVU32 *)initBlock)[4]);   /* data fork / resource */
    else
        lg("config: library location unknown");
    gl_stats_init();
    cpu_speed();
    if (!gl_boot_arg("nv34glnoexc")) {
        gPrevExc = InstallExceptionHandler(exc_handler);
        gExcInst = 1;
    }
    return noErr;
}

void MiniGLTerm(void)
{
    int i;
    if (gExcInst) {
        InstallExceptionHandler(gPrevExc);          /* the library code is about to be unloaded — restore the handler */
        gExcInst = 0;
    }
    exc_report();
    if (!S)
        return;
    hw_sync();
    full_hide(0);
    gl_release_textures();
    hw_vb_free();
    hw_pend_flush();
    for (i = 0; i < 16; i++)
        if (gCtxs[i]) {
            if (gCtxs[i]->hasRT)
                hw_target_free(&gCtxs[i]->rt);
            if (gCtxs[i]->def.hw)
                hw_tex_free(gCtxs[i]->def.hw);
            gCtxs[i]->hasRT = 0;
            gCtxs[i]->def.hw = 0;
        }
    lg("NV34 MiniGL: shutting down, VRAM free %u KB", S->vramFreeBytes() >> 10);
}

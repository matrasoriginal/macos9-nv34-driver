/*
 * NV34 RAVE Test — verifies the NV34 RAVE engine through the public RAVE API (QuickDraw 3D RAVE).
 *
 * 1. lists the RAVE engines of the main screen (name, vendor, options),
 * 2. on the NV34 engine (vendor 'NV34') in a window with a double buffer and z-buffer:
 *    A z-buffer (three rectangles, like gl-7), B blending, C perspective texture (asymmetric
 *    checkerboard: also checks the swizzle bit order), D bitmap, E performance (spinning cube),
 *    each part with pixels read back from the screen,
 * 3. saves the engine log (shared with QD Accel) — including games run earlier.
 * Output: "NV34 RAVE.txt" + "NV34 RAVE log.txt" next to the application.
 */
#include <Quickdraw.h>
#include <Windows.h>
#include <Fonts.h>
#include <Events.h>
#include <Menus.h>
#include <Dialogs.h>
#include <TextEdit.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

#include "../nv34rave/rave.h"
#include "../accel/nv34_shared.h"

typedef SInt32 OSStatus;
typedef struct RegEntryID { UInt32 contents[4]; } RegEntryID;
extern OSStatus RegistryCStrEntryLookup(const RegEntryID *start, const char *path, RegEntryID *found);
extern OSStatus RegistryPropertyGet(const RegEntryID *e, const char *name, void *value, UInt32 *size);
extern OSStatus RegistryEntryIDDispose(RegEntryID *id);

#define W 480
#define H 320
#define LINES 24

static char gLines[LINES][120];
static int gLineCount;

static void log_line(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    FILE *f;

    va_start(ap, fmt);
    vsprintf(buf, fmt, ap);
    va_end(ap);
    f = fopen("NV34 RAVE.txt", "a");
    if (f) {
        fprintf(f, "%s\n", buf);
        fclose(f);
    }
    if (gLineCount < LINES)
        strncpy(gLines[gLineCount++], buf, 119);
}

static NV34Shared *get_shared(void)
{
    RegEntryID e;
    UInt32 addr = 0, size = 4;
    NV34Shared *s = 0;
    if (RegistryCStrEntryLookup(0, "Devices:device-tree:pci:NVDA,Parent:NVDA,Display-A", &e) != 0)
        return 0;
    if (RegistryPropertyGet(&e, "NV34ACCEL,shared", &addr, &size) == 0 && size == 4 && addr) {
        s = (NV34Shared *)addr;
        if (s->magic != NV34_SHARED_MAGIC)
            s = 0;
    }
    RegistryEntryIDDispose(&e);
    return s;
}

static void dump_engine_log(NV34Shared *s, const char *title)
{
    FILE *f;
    NVU32 start, i;

    if (!s || !s->log)
        return;
    f = fopen("NV34 RAVE log.txt", "a");
    if (!f)
        return;
    fprintf(f, "---- %s (%lu B) ----\n", title, s->logPos);
    start = s->logPos > s->logSize ? s->logPos - s->logSize : 0;
    for (i = start; i < s->logPos; i++) {
        char ch = s->log[i % s->logSize];
        fputc(ch == '\r' ? '\n' : ch, f);
    }
    fclose(f);
}

static void show(WindowPtr win)
{
    Rect r;
    int i;
    SetPort(win);
    SetRect(&r, 0, H, W, H + 14 + 12 * LINES);
    EraseRect(&r);
    TextSize(9);
    for (i = 0; i < gLineCount; i++) {
        unsigned char p[120];
        p[0] = (unsigned char)strlen(gLines[i]);
        memcpy(p + 1, gLines[i], p[0]);
        MoveTo(6, H + 12 + 12 * i);
        DrawString(p);
    }
}

/* screen pixel in global coordinates */
static int gBpp = 32;

static UInt32 screen_pixel(int gx, int gy)
{
    PixMapPtr pm = *(*GetMainDevice())->gdPMap;
    long rb = pm->rowBytes & 0x3fff;
    Ptr p = pm->baseAddr + (gy - pm->bounds.top) * rb + (gx - pm->bounds.left) * (gBpp / 8);
    return gBpp == 16 ? *(volatile UInt16 *)p : *(volatile UInt32 *)p;
}

static void vg(TQAVGouraud *v, float x, float y, float z, float r, float g, float b, float a)
{
    v->x = x; v->y = y; v->z = z; v->invW = 1.0f;
    v->r = r; v->g = g; v->b = b; v->a = a;
}

static void rect_g(TQADrawContext *dc, float x, float y, float w, float h, float z,
                   float r, float g, float b, float a)
{
    TQAVGouraud v[4];
    vg(&v[0], x, y, z, r, g, b, a);
    vg(&v[1], x + w, y, z, r, g, b, a);
    vg(&v[2], x + w, y + h, z, r, g, b, a);
    vg(&v[3], x, y + h, z, r, g, b, a);
    dc->drawTriGouraud(dc, &v[0], &v[1], &v[2], 0);
    dc->drawTriGouraud(dc, &v[0], &v[2], &v[3], 0);
}

static void vt(TQAVTexture *v, float x, float y, float z, float w, float u, float vv)
{
    memset(v, 0, sizeof(*v));
    v->x = x; v->y = y; v->z = z;
    v->invW = 1.0f / w;
    v->uOverW = u / w;
    v->vOverW = vv / w;
    v->a = 1.0f;
    v->r = v->g = v->b = 1.0f;
    v->kd_r = v->kd_g = v->kd_b = 1.0f;
}

int main(void)
{
    WindowPtr win;
    Rect wr;
    GDHandle gd;
    TQADevice dev;
    TQAEngine *e, *nv = 0;
    TQADrawContext *dc = 0;
    TQAClip clip;
    TQARect rr;
    RgnHandle rgn;
    NV34Shared *s;
    Point org = { 0, 0 };
    TQATexture *tex = 0;
    TQABitmap *bmp = 0;
    TQAError err;
    static UInt32 texPix[64 * 32];
    static UInt32 bmpPix[16 * 16];
    static UInt16 tex16Pix[64];
    TQATexture *tex16 = 0;
    TQAImage img;
    int gx, gy, i, variant;
    EventRecord ev;

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(0);
    InitCursor();

    s = get_shared();
    dump_engine_log(s, "before the test");

    SetRect(&wr, 40, 50, 40 + W, 50 + H + 14 + 12 * LINES);
    win = NewCWindow(0, &wr, "\pNV34 RAVE Test", true, documentProc, (WindowPtr)-1, false, 0);
    SetPort(win);
    LocalToGlobal(&org);
    gx = org.h;
    gy = org.v;

    log_line("NV34 RAVE Test 7   shared %s, 3D state %lu", s ? "ok" : "ERROR (QD Accel?)", s ? s->state3D : 0);

    gd = GetMainDevice();
    dev.deviceType = kQADeviceGDevice;
    dev.device.gDevice = gd;
    for (e = QADeviceGetFirstEngine(&dev); e; e = QADeviceGetNextEngine(&dev, e)) {
        char name[64];
        long vendor = 0, opt = 0, len = 0;
        name[0] = 0;
        QAEngineGestalt(e, kQAGestalt_ASCIINameLength, &len);
        if (len > 0 && len < 63)
            QAEngineGestalt(e, kQAGestalt_ASCIIName, name);
        QAEngineGestalt(e, kQAGestalt_VendorID, &vendor);
        QAEngineGestalt(e, kQAGestalt_OptionalFeatures, &opt);
        log_line("engine: %s  vendor %08lx  opt %08lx", name, vendor, opt);
        if (vendor == 0x4E563334)
            nv = e;
    }
    if (!nv) {
        log_line("NV34 RAVE engine not found (Extensions: NV34 RAVE, NV34 QD Accel, Millions)");
        goto finish;
    }

    /* context = window contents (top part W x H), clip = visible part of the window */
    rr.left = gx; rr.top = gy; rr.right = gx + W; rr.bottom = gy + H;
    rgn = NewRgn();
    CopyRgn(win->visRgn, rgn);
    OffsetRgn(rgn, gx, gy);
    clip.clipType = kQAClipRgn;
    clip.clip.clipRgn = rgn;

    /* texture 64x32 (asymmetric: x by 8, y by 4) and a 16x16 bitmap with a transparent corner */
    for (i = 0; i < 64 * 32; i++) {
        int x = i % 64, y = i / 64;
        texPix[i] = ((x >> 3) + (y >> 2)) & 1 ? 0xFFFF0000 : (x < 8 && y < 4 ? 0xFF0000FF : 0xFF00FF00);
    }
    img.width = 64; img.height = 32; img.rowBytes = 256; img.pixmap = texPix;
    /* FlipOrigin = row 0 at the top (without it a RAVE texture has its origin bottom left) */
    err = QATextureNew(nv, kQATexture_FlipOrigin, kQAPixel_ARGB32, &img, &tex);
    log_line("QATextureNew: %ld", err);
    for (i = 0; i < 256; i++)
        bmpPix[i] = (i % 16) + (i / 16) < 8 ? 0x00000000 : 0xFFFFFF00;
    img.width = 16; img.height = 16; img.rowBytes = 64; img.pixmap = bmpPix;
    QABitmapNew(nv, 0, kQAPixel_ARGB32, &img, &bmp);
    /* 16-bit 8x8 texture: even columns red, odd green, row 0 blue */
    for (i = 0; i < 64; i++)
        tex16Pix[i] = i < 8 ? 0x801f : (i & 1) ? 0x83e0 : 0xfc00;
    img.width = 8; img.height = 8; img.rowBytes = 16; img.pixmap = tex16Pix;
    QATextureNew(nv, kQATexture_FlipOrigin, kQAPixel_ARGB16, &img, &tex16);

    /* variant 0: back buffer with the screen pitch; 1: compact pitch (EngineSpecific1) */
    gBpp = (*(*gd)->gdPMap)->pixelSize;
    log_line("screen %d bpp", gBpp);
    for (variant = 0; variant < 1; variant++) {
        TQAPixelBuffer pb;
        static const int px[12][2] = { {15, 15}, {60, 40}, {100, 40}, {5, 5}, {170, 50}, {241, 11}, {261, 11},
                                       {20, 110}, {394, 14}, {394, 22}, {402, 22}, {330, 250} };
        UInt32 back[12];

        err = QADrawContextNew(&dev, &rr, &clip, nv, kQAContext_DoubleBuffer | kQAContext_NoDither, &dc);
        log_line("variant %d: QADrawContextNew %ld", variant, err);
        if (err || !dc)
            continue;
        dc->setFloat(dc, kQATag_ColorBG_r, 0.1f);
        dc->setFloat(dc, kQATag_ColorBG_g, 0.1f);
        dc->setFloat(dc, kQATag_ColorBG_b, 0.3f);
        dc->setFloat(dc, kQATag_ColorBG_a, 1.0f);
        dc->renderStart(dc, 0, 0);
        /* A z-buffer */
        rect_g(dc, 10, 10, 100, 60, 0.75f, 0, 1, 0, 1);
        rect_g(dc, 40, 10, 40, 60, 0.25f, 1, 0, 0, 1);
        rect_g(dc, 25, 30, 85, 20, 0.5f, 0, 0, 1, 1);
        /* B blending */
        dc->setInt(dc, kQATag_Blend, kQABlend_Interpolate);
        dc->setInt(dc, kQATag_ZFunction, kQAZFunction_None);
        rect_g(dc, 130, 10, 60, 60, 0.5f, 1, 0, 0, 1);
        rect_g(dc, 160, 40, 60, 60, 0.1f, 1, 1, 1, 0.5f);
        dc->setInt(dc, kQATag_Blend, kQABlend_PreMultiply);
        dc->setInt(dc, kQATag_ZFunction, kQAZFunction_LT);
        /* C texture without perspective (texel 2x2 px) + perspective */
        if (tex) {
            TQAVTexture v[4];
            dc->setPtr(dc, kQATag_Texture, tex);
            dc->setInt(dc, kQATag_TextureFilter, kQATextureFilter_Fast);
            vt(&v[0], 240, 10, 0.5f, 1, 0, 0);
            vt(&v[1], 368, 10, 0.5f, 1, 1, 0);
            vt(&v[2], 368, 74, 0.5f, 1, 1, 1);
            vt(&v[3], 240, 74, 0.5f, 1, 0, 1);
            dc->drawTriTexture(dc, &v[0], &v[1], &v[2], 0);
            dc->drawTriTexture(dc, &v[0], &v[2], &v[3], 0);
            vt(&v[0], 280, 100, 0.5f, 4, 0, 0);
            vt(&v[1], 330, 100, 0.5f, 4, 2, 0);
            vt(&v[2], 400, 200, 0.5f, 1, 2, 2);
            vt(&v[3], 210, 200, 0.5f, 1, 0, 2);
            dc->setInt(dc, kQATag_TextureFilter, kQATextureFilter_Mid);
            dc->drawTriTexture(dc, &v[0], &v[1], &v[2], 0);
            dc->drawTriTexture(dc, &v[0], &v[2], &v[3], 0);
        }
        /* F 16-bit 64x64 texture (texel = 8x8 px) */
        if (tex16) {
            TQAVTexture v[4];
            dc->setPtr(dc, kQATag_Texture, tex16);
            dc->setInt(dc, kQATag_TextureFilter, kQATextureFilter_Fast);
            vt(&v[0], 390, 10, 0.5f, 1, 0, 0);
            vt(&v[1], 454, 10, 0.5f, 1, 1, 0);
            vt(&v[2], 454, 74, 0.5f, 1, 1, 1);
            vt(&v[3], 390, 74, 0.5f, 1, 0, 1);
            dc->drawTriTexture(dc, &v[0], &v[1], &v[2], 0);
            dc->drawTriTexture(dc, &v[0], &v[2], &v[3], 0);
        }
        /* G depth precision (like UT: z close to 1): nearer green, then farther red */
        rect_g(dc, 300, 230, 60, 40, 0.999990f, 0, 1, 0, 1);
        rect_g(dc, 300, 230, 60, 40, 0.999995f, 1, 0, 0, 1);
        /* D bitmap */
        if (bmp) {
            TQAVGouraud bv;
            vg(&bv, 10, 100, 0.0f, 1, 1, 1, 1);
            dc->setInt(dc, kQATag_Blend, kQABlend_Interpolate);
            dc->drawBitmap(dc, &bv, bmp);
            dc->setInt(dc, kQATag_Blend, kQABlend_PreMultiply);
        }
        /* back buffer via the CPU (before the transfer to the screen) */
        err = dc->accessDrawBuffer(dc, &pb);
        if (!err) {
            for (i = 0; i < 12; i++)
                back[i] = gBpp == 16 ?
                    *(volatile UInt16 *)((char *)pb.baseAddr + px[i][1] * pb.rowBytes + px[i][0] * 2) :
                    *(volatile UInt32 *)((char *)pb.baseAddr + px[i][1] * pb.rowBytes + px[i][0] * 4);
            dc->accessDrawBufferEnd(dc, 0);
            log_line(" back buf (rb %ld): A %08lx %08lx %08lx bg %08lx B %08lx C %08lx %08lx D %08lx F %08lx %08lx %08lx G %08lx",
                     pb.rowBytes, back[0], back[1], back[2], back[3], back[4], back[5], back[6], back[7],
                     back[8], back[9], back[10], back[11]);
        } else {
            log_line(" accessDrawBuffer: %ld", err);
        }
        err = dc->renderEnd(dc, 0);
        dc->sync(dc);
        for (i = 0; i < 12; i++)
            back[i] = screen_pixel(gx + px[i][0], gy + px[i][1]);
        log_line(" screen:           A %08lx %08lx %08lx bg %08lx B %08lx C %08lx %08lx D %08lx F %08lx %08lx %08lx G %08lx",
                 back[0], back[1], back[2], back[3], back[4], back[5], back[6], back[7],
                 back[8], back[9], back[10], back[11]);

        /* E performance 2 s */
        {
            UInt32 t0 = TickCount(), frames = 0;
            float ang = 0;
            while (TickCount() - t0 < 120) {
                int k;
                TQAVTexture v[4];
                dc->renderStart(dc, 0, 0);
                for (k = 0; k < 1000; k++) {
                    float cx = 40 + (k % 40) * 10.5f, cy = 30 + (k / 40) * 10.0f;
                    float c = cosf(ang + k * 0.1f) * 6, sn = sinf(ang + k * 0.1f) * 6;
                    vt(&v[0], cx - c, cy - sn, 0.5f, 1, 0, 0);
                    vt(&v[1], cx + sn, cy - c, 0.5f, 1, 1, 0);
                    vt(&v[2], cx + c, cy + sn, 0.5f, 1, 1, 1);
                    vt(&v[3], cx - sn, cy + c, 0.5f, 1, 0, 1);
                    dc->drawTriTexture(dc, &v[0], &v[1], &v[2], 0);
                    dc->drawTriTexture(dc, &v[0], &v[2], &v[3], 0);
                }
                dc->renderEnd(dc, 0);
                ang += 0.05f;
                frames++;
            }
            dc->sync(dc);
            log_line(" E perf: %lu frames/2 s = %lu fps, %lu k tris/s", frames, frames / 2, frames * 2000 / 2000);
        }
        QADrawContextDelete(dc);
        dc = 0;
    }
    if (gBpp == 16)
        log_line("expect: A 03e0 7c00 001f bg 0c69 B 7e10 C 001f 7c00 D 7fe0 F 001f 7c00 03e0 G 03e0 (bit 15 arbitrary)");
    else
        log_line("expect: A ff00ff00 ffff0000 ff0000ff bg ff1a1a4d B bfff8080 C ff0000ff ffff0000 D ffffff00 F ff0000ff ffff0000 ff00ff00 G ff00ff00");

finish:
    if (tex)
        QATextureDelete(nv, tex);
    if (bmp)
        QABitmapDelete(nv, bmp);
    if (tex16)
        QATextureDelete(nv, tex16);
    if (dc)
        QADrawContextDelete(dc);
    dump_engine_log(s, "after the test");
    log_line("done; click = quit");
    show(win);
    for (;;) {
        if (WaitNextEvent(everyEvent, &ev, 10, 0)) {
            if (ev.what == mouseDown || ev.what == keyDown)
                break;
            if (ev.what == updateEvt) {
                BeginUpdate(win);
                show(win);
                EndUpdate(win);
            }
        }
    }
    return 0;
}

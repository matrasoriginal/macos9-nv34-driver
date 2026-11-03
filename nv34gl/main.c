/*
 * NV34 GL Test — the first triangle from the GeForce FX Go5200 3D engine (rankine 0x0697) on OS 9.
 *
 * Procedure: 2D init (nva_init_engine) → 3D init (accel/nv34_3d.c: PGRAPH context per
 * nouveau, state per xf86-video-nouveau) → 4 drawing variants, each verified by reading
 * the pixels back:
 *   gl-3: only the register combiner path draws; colors are LE (red → 0000ffff).
 *   gl-4: 3D object with/without the big-endian flag; in each a red triangle (combiners =
 *         primary color) and a square with a checkerboard texture (FP txph + combiners tex × color).
 *
 * Run WITHOUT NV34 QD Accel (Shift at boot or boot-args nv34noaccel) — the test
 * reconfigures the engine. The log "NV34 GL.txt" is written line by line.
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

#include "../accel/nv34_accel.h"
#include "../accel/nv34_3d.h"

typedef SInt32 OSStatus;
typedef struct RegEntryID { UInt32 contents[4]; } RegEntryID;
extern OSStatus RegistryCStrEntryLookup(const RegEntryID *start, const char *path, RegEntryID *found);
extern OSStatus RegistryPropertyGet(const RegEntryID *e, const char *name, void *value, UInt32 *size);
extern OSStatus RegistryEntryIDDispose(RegEntryID *id);

#define W 504
#define F1 0x3f800000
#define H 300

static char gLines[26][110];
static int gLineCount;

static void log_line(const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    FILE *f;

    va_start(ap, fmt);
    vsprintf(buf, fmt, ap);
    va_end(ap);
    f = fopen("NV34 GL.txt", "a");
    if (f) {
        fprintf(f, "%s\n", buf);
        fclose(f);
    }
    if (gLineCount < 26)
        strncpy(gLines[gLineCount++], buf, 109);
}

static NVU32 ticks(void)
{
    return TickCount();
}

static int find_hw(volatile NVU8 **mmio, volatile NVU8 **vram, NVU32 *vramSize)
{
    RegEntryID e;
    UInt32 assigned[5 * 8], logical[8], mem[2], size, n, i;

    *mmio = *vram = 0;
    if (RegistryCStrEntryLookup(0, "Devices:device-tree:pci:NVDA,Parent", &e) != 0)
        return 0;
    size = sizeof(assigned);
    if (RegistryPropertyGet(&e, "assigned-addresses", assigned, &size) == 0) {
        n = size / 20;
        size = sizeof(logical);
        if (RegistryPropertyGet(&e, "AAPL,address", logical, &size) == 0) {
            if (n > size / 4)
                n = size / 4;
            for (i = 0; i < n; i++) {
                if ((assigned[i * 5] & 0xff) == 0x10)
                    *mmio = (volatile NVU8 *)logical[i];
                else if ((assigned[i * 5] & 0xff) == 0x14)
                    *vram = (volatile NVU8 *)logical[i];
            }
        }
    }
    RegistryEntryIDDispose(&e);
    *vramSize = 0x02000000;
    if (RegistryCStrEntryLookup(0, "Devices:device-tree:pci:NVDA,Parent:NVDA,Display-A", &e) == 0) {
        size = sizeof(mem);
        if (RegistryPropertyGet(&e, "VRAM,memsize", mem, &size) == 0 && size >= 4)
            *vramSize = mem[0];
        RegistryEntryIDDispose(&e);
    }
    return *mmio && *vram;
}

static NVU32 pixel(volatile NVU8 *fb, NVU32 rowBytes, int x, int y)
{
    return *(volatile NVU32 *)(fb + y * rowBytes + x * 4);
}

static void show(WindowPtr win)
{
    Rect r;
    int i;
    SetPort(win);
    SetRect(&r, 0, H - 4, W + 16, H + 16 + 13 * 26);
    EraseRect(&r);
    for (i = 0; i < gLineCount; i++) {
        unsigned char p[110];
        p[0] = (unsigned char)strlen(gLines[i]);
        memcpy(p + 1, gLines[i], p[0]);
        MoveTo(8, H + 10 + 13 * i);
        DrawString(p);
    }
}

static void report_trap(NV3D *g, const char *what)
{
    NV3DTrap t;
    nv3d_trap(g, &t);
    log_line("  %s: PGRAPH intr %08lx nsource %08lx nstatus %08lx", what, t.intr, t.nsource, t.nstatus);
    log_line("     trapped addr %08lx data %08lx  ctx_user %08lx  status %08lx", t.addr, t.data, t.ctxUser, t.status);
    log_line("     PFIFO intr %08lx pull0 %08lx st %08lx push1 %08lx dma get %08lx put %08lx cache get %08lx put %08lx",
             t.fifoIntr, t.cache1Pull0, t.cache1Status, t.cache1Push1, t.dmaGet, t.dmaPut, t.cacheGet, t.cachePut);
}

int main(void)
{
    static const NVU32 fpMovH0[4] = {
        0x01003E81,     /* MOVH H0, f[COL0]; END */
        0x1c9dc801,
        0x0001c800,
        0x0001c800,
    };
    static const NVU32 fpTex0[4] = {
        0x18009e81,     /* TXPH H0, a[tex0], t[0]; END (xf86-video-nouveau PFP_PASS) */
        0x1c9dc801,
        0x0001c800,
        0x3fe1c800,
    };
    WindowPtr win;
    PixMapHandle spm;
    Rect wr, r;
    Point gl = { 0, 0 };
    NVAccel a;
    NV3D g;
    volatile NVU8 *mmio, *vram, *fb;
    NVU32 vramSize, rowBytes, fbOffset, boot1;
    int bpp, gx, gy, v, err;

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(0);
    InitCursor();

    SetRect(&wr, 30, 50, 30 + W + 16, 50 + H + 16 + 13 * 26);
    win = NewCWindow(0, &wr, "\pNV34 GL Test", true, documentProc, (WindowPtr)-1, false, 0);
    SetPort(win);
    TextFont(kFontIDMonaco);
    TextSize(9);

    log_line("NV34 GL Test 8  (z-buffer clear; expected blue ff0000ff red ffff0000 green ff00ff00)");
    spm = (**GetMainDevice()).gdPMap;
    bpp = (**spm).pixelSize;
    rowBytes = (**spm).rowBytes & 0x3fff;
    fb = (volatile NVU8 *)(**spm).baseAddr;
    if (bpp != 32) {
        log_line("ERROR: switch to Millions (32 bpp), currently %d bpp", bpp);
        goto done;
    }
    if (!find_hw(&mmio, &vram, &vramSize)) {
        log_line("ERROR: NVDA,Parent / BAR not found");
        goto done;
    }
    fbOffset = (NVU32)(fb - vram);
    boot1 = *(volatile NVU32 *)(mmio + 4);
    memset(&a, 0, sizeof(a));
    a.mmio = mmio;
    a.vram = vram;
    a.vramSize = vramSize;
    a.bigEndianRegs = (boot1 == 0x01000001);
    a.beObjects = 0;
    a.rawFormats = 1;
    a.ticks = ticks;
    log_line("FB +%08lx pitch %lu, VRAM %lu MB, PGRAPH st %08lx", fbOffset, rowBytes, vramSize >> 20,
             nva_rd32(&a, 0x400700));

    SetRect(&r, 0, 0, W + 16, H);
    ForeColor(blackColor);
    PaintRect(&r);
    gl.h = 0;
    gl.v = 0;
    LocalToGlobal(&gl);
    gx = gl.h;
    gy = gl.v;

    nva_init_engine(&a);
    err = nva_reset_graphics(&a, 32, rowBytes, fbOffset);
    if (!err)
        err = nva_sync(&a, 60);
    log_line("2D init: %s", err ? "TIMEOUT" : "OK");
    if (err)
        goto done;

    err = nv3d_init(&g, &a, fbOffset, rowBytes, (**spm).bounds.right, (**spm).bounds.bottom);
    log_line("3D init: %s  ctx_user %08lx  PGRAPH st %08lx", err ? "TIMEOUT" : "OK",
             nva_rd32(&a, 0x400148), nva_rd32(&a, 0x400700));
    report_trap(&g, "after init");
    if (err) {
        nva_shutdown(&a);
        goto done;
    }

    nv3d_object_be(&g, 1);
    nv3d_upload_fp(&g, fpMovH0, 4, 1);          /* gl-5: encoding 1 with the BE object */
    nv3d_use_fp(&g, 0x00000000, 0x0001000f);
    nv3d_rc_final(&g, 0x04);                    /* final = primary color */

    /* --- A: z-buffer clear (gl-7: CLEAR gave 12345678 → 00000078) --- */
    {
        static const struct { NVU32 value; int we; } cv[4] = {
            { 0xffffff00, 1 }, { 0xffffffff, 0 }, { 0xffffffff, 1 }, { 0x00ffffff, 1 } };
        volatile NVU8 *zb = vram + 0x01000000;
        int k;
        for (k = 0; k < 4; k++) {
            int x = gx + 10 + k * 120, y = gy + 10;
            NVU32 zoff = (NVU32)(y + 30) * rowBytes + (NVU32)(x + 60) * 4;
            NVU32 zc;

            *(volatile NVU32 *)(zb + zoff) = 0x12345678;
            __asm__ volatile("sync" ::: "memory");
            nv3d_clear_depth_value(&g, cv[k].value, cv[k].we);
            nva_sync(&a, 60);
            zc = *(volatile NVU32 *)(zb + zoff);

            nv3d_depth(&g, 1, F1);
            nv3d_rect_z(&g, x, y, 100, 60, 0x3f400000, 0, F1, 0, F1);          /* green z=0.75 */
            nv3d_rect_z(&g, x + 30, y, 40, 60, 0x3e800000, F1, 0, 0, F1);      /* red z=0.25 */
            nv3d_rect_z(&g, x + 15, y + 20, 85, 20, 0x3f000000, 0, 0, F1, F1); /* blue z=0.5 */
            nv3d_depth(&g, 0, F1);
            err = nva_sync(&a, 60);
            log_line("clear %08lx write %d: zeta %08lx  -> blue %08lx red %08lx green %08lx%s",
                     cv[k].value, cv[k].we, zc, pixel(fb, rowBytes, x + 90, y + 30),
                     pixel(fb, rowBytes, x + 50, y + 30), pixel(fb, rowBytes, x + 5, y + 5),
                     err ? " TIMEOUT" : "");
            if (err) {
                report_trap(&g, "clear");
                goto finish;
            }
        }
    }

    /* --- B: blending SRC_ALPHA / ONE_MINUS_SRC_ALPHA --- */
    {
        int x = gx + 400, y = gy + 90;
        nv3d_rect_z(&g, x, y, 60, 60, 0, F1, 0, 0, F1);                    /* red background */
        nv3d_blend(&g, 1);
        nv3d_rect_z(&g, x + 30, y + 15, 60, 30, 0, F1, F1, F1, 0x3f000000); /* white, alpha 0.5 */
        nv3d_blend(&g, 0);
        err = nva_sync(&a, 60);
        log_line("blend: %s  over red %08lx (ff ff 80 80)  over black %08lx (?? 80 80 80)",
                 err ? "TIMEOUT" : "ok", pixel(fb, rowBytes, x + 45, y + 30), pixel(fb, rowBytes, x + 75, y + 30));
        if (err) {
            report_trap(&g, "blend");
            goto finish;
        }
    }

    /* --- C: swizzled texture 64×64, REPEAT, normalized coordinates 0..2 --- */
    {
        volatile NVU32 *tex = (volatile NVU32 *)(vram + 0x01A00000);
        int tx, ty, x = gx + 10, y = gy + 90;
        for (ty = 0; ty < 64; ty++)
            for (tx = 0; tx < 64; tx++) {
                NVU32 m = 0, bit;
                for (bit = 0; bit < 6; bit++)
                    m |= (((tx >> bit) & 1) << (2 * bit)) | (((ty >> bit) & 1) << (2 * bit + 1));
                tex[m] = (((tx >> 3) ^ (ty >> 3)) & 1) ? 0xFF00FF00 : 0xFFFF0000;
            }
        __asm__ volatile("sync" ::: "memory");
        nv3d_upload_fp(&g, fpTex0, 4, 1);
        nv3d_use_fp(&g, 0x00000000, 0x0001000f);
        nv3d_texture_swz(&g, 0x01A00000, 6, 6);
        nv3d_rc_modulate_tex(&g);
        nv3d_quad_texf(&g, x, y, 128, 128, 0x40000000, 0x40000000);      /* 0..2 */
        err = nva_sync(&a, 60);
        log_line("swz texture: %s  (4,4) %08lx red  (12,4) %08lx green  (68,4) %08lx red  (76,68) %08lx green",
                 err ? "TIMEOUT" : "ok", pixel(fb, rowBytes, x + 4, y + 4), pixel(fb, rowBytes, x + 12, y + 4),
                 pixel(fb, rowBytes, x + 68, y + 4), pixel(fb, rowBytes, x + 76, y + 68));
        if (err) {
            report_trap(&g, "texture");
            goto finish;
        }
    }

    /* --- D: performance --- */
    {
        NVU32 t0, dt, i;
        int x = gx + 160, y = gy + 90;

        t0 = TickCount();
        for (i = 0; i < 2000; i++)
            nv3d_quad_texf(&g, x, y, 256, 192, 0x40800000, 0x40400000);
        err = nva_sync(&a, 600);
        dt = TickCount() - t0;
        if (!dt)
            dt = 1;
        log_line("fill: 2000 x 256x192 textured quads in %lu ticks = %lu Mpix/s%s", dt,
                 (2000UL * 256 * 192 / 1000) * 60 / dt / 1000, err ? " (TIMEOUT)" : "");

        nv3d_upload_fp(&g, fpMovH0, 4, 1);
        nv3d_use_fp(&g, 0x00000000, 0x0001000f);
        nv3d_rc_final(&g, 0x04);
        t0 = TickCount();
        for (i = 0; i < 200000; i++) {
            int ox = (i * 7) % 200, oy = (i * 13) % 150;
            nv3d_triangle(&g, x + ox, y + oy + 8, x + ox, y + oy, x + ox + 8, y + oy,
                          (i & 1) ? F1 : 0, (i & 2) ? F1 : 0, (i & 4) ? F1 : 0, F1);
        }
        err = nva_sync(&a, 600);
        dt = TickCount() - t0;
        if (!dt)
            dt = 1;
        log_line("triangles: 200000 small ones in %lu ticks = %lu k/s%s", dt, 200000UL * 60 / dt / 1000,
                 err ? " (TIMEOUT)" : "");
    }
finish:
    nva_sync(&a, 60);
    log_line("done; the engine stays in 3D state (restart before using QD Accel)");

done:
    log_line("click = quit");
    show(win);
    while (!Button())
        ;
    return 0;
}

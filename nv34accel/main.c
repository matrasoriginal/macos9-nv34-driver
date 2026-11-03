/*
 * NV34 Accel — test of the GeForce FX Go5200 2D engine on OS 9 (step A of phase 3).
 *
 * 1. finds MMIO/VRAM in the Name Registry, the framebuffer from the main GDevice,
 * 2. initializes PFIFO/PGRAPH (accel/nv34_accel.c, port of xf86-video-nv),
 * 3. GPU fill and GPU blit in a window, verified by reading the pixels back,
 * 4. measures GPU vs. CPU (QuickDraw) for fill and screen → screen copy.
 *
 * The log "NV34 Accel.txt" is written line by line (open/close) so that the last executed step
 * survives a GPU hang.
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

typedef SInt32 OSStatus;
typedef struct RegEntryID { UInt32 contents[4]; } RegEntryID;
extern OSStatus RegistryCStrEntryLookup(const RegEntryID *start, const char *path, RegEntryID *found);
extern OSStatus RegistryPropertyGet(const RegEntryID *e, const char *name, void *value, UInt32 *size);
extern OSStatus RegistryEntryIDDispose(RegEntryID *id);

#define W 504
#define H 384

static char gLines[28][100];
static int gLineCount;

static void log_line(const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    FILE *f;

    va_start(ap, fmt);
    vsprintf(buf, fmt, ap);
    va_end(ap);
    f = fopen("NV34 Accel.txt", "a");
    if (f) {
        fprintf(f, "%s\n", buf);
        fclose(f);
    }
    if (gLineCount < 28)
        strncpy(gLines[gLineCount++], buf, 99);
}

static void show(WindowPtr win)
{
    Rect r;
    int i;
    SetPort(win);
    SetRect(&r, 0, 0, W + 16, H + 16);
    EraseRect(&r);
    for (i = 0; i < gLineCount; i++) {
        unsigned char p[100];
        p[0] = (unsigned char)strlen(gLines[i]);
        memcpy(p + 1, gLines[i], p[0]);
        MoveTo(8, 14 + 13 * i);
        DrawString(p);
    }
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

static NVU32 rd_pixel(volatile NVU8 *fb, NVU32 rowBytes, int bpp, int x, int y)
{
    volatile NVU8 *p = fb + y * rowBytes + x * (bpp / 8);
    if (bpp == 32)
        return *(volatile NVU32 *)p;
    if (bpp == 16)
        return *(volatile UInt16 *)p;
    return *p;
}

static void draw_content(Rect *r)
{
    Rect q;
    int i;
    EraseRect(r);
    for (i = 0; i < 8; i++) {
        RGBColor c;
        c.red = (i & 1) ? 0xffff : 0;
        c.green = (i & 2) ? 0xffff : 0x4000;
        c.blue = (i & 4) ? 0xffff : 0x8000;
        RGBForeColor(&c);
        SetRect(&q, r->left + i * 60, r->top, r->left + i * 60 + 56, r->top + 40);
        PaintRect(&q);
    }
    ForeColor(blackColor);
    for (i = 0; i < 30; i++) {
        MoveTo(r->left + 4, r->top + 56 + i * 11);
        DrawString("\pNV34 Accel: The quick brown fox jumps over the lazy dog 0123456789");
    }
}

int main(void)
{
    WindowPtr win;
    PixMapHandle spm;
    Rect wr, r;
    Point gl = { 0, 0 };
    NVAccel a;
    volatile NVU8 *mmio, *vram, *fb;
    NVU32 vramSize, rowBytes, fbOffset, boot0, boot1, expect, got;
    int bpp, gx, gy, err, i, ok;

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(0);
    InitCursor();

    SetRect(&wr, 40, 60, 40 + W + 16, 60 + H + 16);
    win = NewCWindow(0, &wr, "\pNV34 Accel", true, documentProc, (WindowPtr)-1, false, 0);
    SetPort(win);
    TextFont(kFontIDMonaco);
    TextSize(9);
    SetRect(&r, 8, 8, 8 + W, 8 + H);

    log_line("NV34 Accel 2");

    spm = (**GetMainDevice()).gdPMap;
    bpp = (**spm).pixelSize;
    rowBytes = (**spm).rowBytes & 0x3fff;
    fb = (volatile NVU8 *)(**spm).baseAddr;

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
    a.beObjects = 1;
    a.ticks = ticks;
    boot0 = nva_rd32(&a, 0);
    log_line("MMIO %08lx VRAM %08lx (%lu MB) FB +%08lx pitch %lu, %d bpp", (UInt32)mmio, (UInt32)vram,
             vramSize >> 20, fbOffset, rowBytes, bpp);
    log_line("PMC_BOOT_0 %08lx BOOT_1 %08lx  PGRAPH st %08lx  PFIFO caches %08lx", boot0, boot1,
             nva_rd32(&a, 0x400700), nva_rd32(&a, 0x002500));
    for (i = 0; i < 8; i++) {
        NVU32 t = 0x100240 + i * 0x10;
        NVU32 v0 = nva_rd32(&a, t), v1 = nva_rd32(&a, t + 4), v2 = nva_rd32(&a, t + 8), v3 = nva_rd32(&a, t + 12);
        if (v0 | v1 | v2 | v3)
            log_line("PFB tile %d: %08lx %08lx %08lx %08lx", i, v0, v1, v2, v3);
    }
    log_line("PFB cfg0 %08lx cfg1 %08lx", nva_rd32(&a, 0x100200), nva_rd32(&a, 0x100204));
    if (((boot0 >> 20) & 0x1ff) != 0x34) {
        log_line("ERROR: chip is not an NV34");
        goto done;
    }
    if (fbOffset >= vramSize || (fbOffset & 63)) {
        log_line("ERROR: FB outside VRAM or misaligned");
        goto done;
    }

    draw_content(&r);
    gl.h = r.left;
    gl.v = r.top;
    LocalToGlobal(&gl);
    gx = gl.h;
    gy = gl.v;

    /* --- init --- */
    log_line("init engine (BE objects %d)...", a.beObjects);
    nva_init_engine(&a);
    err = nva_reset_graphics(&a, bpp == 16 ? 15 : bpp, rowBytes, fbOffset);
    if (!err)
        err = nva_sync(&a, 60);
    log_line("init: %s  GET %lu  PGRAPH st %08lx intr %08lx  PFIFO intr %08lx",
             err ? "TIMEOUT" : "OK", nva_rd32(&a, 0x800044) >> 2, nva_rd32(&a, 0x400700),
             nva_rd32(&a, 0x400100), nva_rd32(&a, 0x002100));
    if (err) {
        nva_shutdown(&a);
        goto done;
    }

    /* --- fill: color in the native screen format --- */
    for (i = 0; i < 2; i++) {
        expect = bpp == 32 ? 0x00C01020 : bpp == 16 ? 0x6044 : 0xE3;
        nva_fill(&a, gx + 300, gy + 60, 80, 60, expect);
        err = nva_sync(&a, 60);
        got = rd_pixel(fb, rowBytes, bpp, gx + 340, gy + 90);
        log_line("fill: %s  expected %08lx read %08lx", err ? "TIMEOUT" : "OK", expect, got);
        if (err || got == expect || i == 1)
            break;
        /* mismatch → try the objects without the big-endian flag */
        a.beObjects = !a.beObjects;
        log_line("reinit with BE objects %d", a.beObjects);
        nva_init_engine(&a);
        if (nva_reset_graphics(&a, bpp == 16 ? 15 : bpp, rowBytes, fbOffset) || nva_sync(&a, 60)) {
            err = 1;
            break;
        }
    }
    if (err) {
        log_line("  GET %lu PUT %lu PGRAPH st %08lx intr %08lx PFIFO intr %08lx",
                 a.errGet, a.errPut, a.errPgraphStatus, a.errPgraphIntr, a.errPfifoIntr);
        nva_shutdown(&a);
        goto done;
    }

    /* --- blit: top band of colored squares 200 px lower --- */
    nva_copy(&a, gx, gy, gx, gy + 200, 480, 40);
    err = nva_sync(&a, 60);
    ok = 1;
    for (i = 0; i < 480; i += 7)
        if (rd_pixel(fb, rowBytes, bpp, gx + i, gy + 20) != rd_pixel(fb, rowBytes, bpp, gx + i, gy + 220))
            ok = 0;
    log_line("blit: %s, contents %s", err ? "TIMEOUT" : "OK", ok ? "match" : "MISMATCH");
    if (err) {
        nva_shutdown(&a);
        goto done;
    }

    /* --- measurement: 1 s per test, W-8 x H px --- */
    {
        NVU32 t0, n, rate, bytes = (W - 8) * H * (bpp / 8);
        Rect r1 = r, r2 = r;
        r1.right -= 8;
        OffsetRect(&r2, 8, 0);
        r2.right -= 8;

        n = 0;
        t0 = TickCount();
        while (TickCount() - t0 < 60) {
            nva_fill(&a, gx, gy, W - 8, H, (n & 1) ? 0 : expect);
            if ((++n & 7) == 0)
                nva_kickoff(&a);
        }
        nva_sync(&a, 120);
        rate = n * 60 / (TickCount() - t0);
        log_line("GPU fill     %5lu /s  %7lu KB/s", rate, rate * (bytes >> 10));

        n = 0;
        t0 = TickCount();
        while (TickCount() - t0 < 60) {
            if (n & 1)
                nva_copy(&a, gx + 8, gy, gx, gy, W - 8, H);
            else
                nva_copy(&a, gx, gy, gx + 8, gy, W - 8, H);
            if ((++n & 7) == 0)
                nva_kickoff(&a);
        }
        err = nva_sync(&a, 120);
        rate = n * 60 / (TickCount() - t0);
        log_line("GPU scr->scr %5lu /s  %7lu KB/s%s", rate, rate * (bytes >> 10), err ? " (TIMEOUT)" : "");

        n = 0;
        t0 = TickCount();
        while (TickCount() - t0 < 60) {
            CopyBits(&win->portBits, &win->portBits, (n & 1) ? &r2 : &r1, (n & 1) ? &r1 : &r2, srcCopy, 0);
            n++;
        }
        rate = n * 60 / (TickCount() - t0);
        log_line("CPU scr->scr %5lu /s  %7lu KB/s (QuickDraw)", rate, rate * (bytes >> 10));
    }
    log_line("leaving the engine idle; done");

done:
    log_line("click = quit");
    show(win);
    while (!Button())
        ;
    return 0;
}

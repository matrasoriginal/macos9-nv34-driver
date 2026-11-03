/*
 * NV34 Bench — measures the speed of software QuickDraw on the main screen (OS 9).
 *
 * Each test runs ~2 s and counts repetitions. Results are appended to "NV34 Bench.txt"
 * next to the application together with the active video driver, so runs can be compared
 * (COFB / NV34 NDRV without cache / with write-through cache, various depths).
 */
#include <Quickdraw.h>
#include <Windows.h>
#include <Fonts.h>
#include <Events.h>
#include <Menus.h>
#include <Dialogs.h>
#include <TextEdit.h>
#include <stdio.h>
#include <string.h>

typedef SInt32 OSStatus;
typedef struct RegEntryID { UInt32 contents[4]; } RegEntryID;
extern OSStatus RegistryCStrEntryLookup(const RegEntryID *start, const char *path, RegEntryID *found);
extern OSStatus RegistryPropertyGet(const RegEntryID *e, const char *name, void *value, UInt32 *size);
extern OSStatus RegistryEntryIDDispose(RegEntryID *id);

#define TEST_TICKS  120     /* 2 s */
#define RAW_TICKS   60      /* 1 s */
#define W           512
#define H           384

static char gLines[24][96];
static int gLineCount;

static void line(const char *s)
{
    if (gLineCount < 24)
        strncpy(gLines[gLineCount++], s, 95);
}

static void show(WindowPtr win)
{
    Rect r;
    int i;
    SetPort(win);
    SetRect(&r, 0, 0, W + 16, 16 * 15);
    EraseRect(&r);
    for (i = 0; i < gLineCount; i++) {
        unsigned char p[96];
        p[0] = (unsigned char)strlen(gLines[i]);
        memcpy(p + 1, gLines[i], p[0]);
        MoveTo(8, 14 + 14 * i);
        DrawString(p);
    }
}

static void driver_label(char *out)
{
    RegEntryID e;
    unsigned char d[128];
    UInt32 size = sizeof(d);

    strcpy(out, "driver: ?");
    if (RegistryCStrEntryLookup(0, "Devices:device-tree:pci:NVDA,Parent:NVDA,Display-A", &e) != 0)
        return;
    if (RegistryPropertyGet(&e, "driver-descriptor", d, &size) == 0 && size >= 80) {
        int n = d[48] < 31 ? d[48] : 31;
        char name[32];
        memcpy(name, d + 49, n);
        name[n] = 0;
        sprintf(out, "driver: %s %x.%x stage %02x rev %d", name, d[40], d[41] >> 4, d[42], d[43]);
    }
    size = sizeof(d);
    if (RegistryPropertyGet(&e, "NV34NDRV,diag", d, &size) == 0 && size >= 116) {
        static const char *modes[] = { "default", "inhibited", "write-through", "copyback" };
        UInt32 m = (UInt32)d[112] << 24 | (UInt32)d[113] << 16 | d[114] << 8 | d[115];
        sprintf(out + strlen(out), ", fb cache: %s", m < 4 ? modes[m] : "?");
    }
    RegistryEntryIDDispose(&e);
}

/* ---- direct memory access: access width matters with uncached VRAM ---- */

typedef struct MemSpan {
    volatile UInt8 *base;   /* first row, aligned to 16 B */
    UInt32 rowBytes;        /* stride */
    UInt32 bytes;           /* length of one row (multiple of 16) */
    UInt32 rows;
} MemSpan;

static void read32(const MemSpan *g)
{
    UInt32 y, x, acc = 0;
    for (y = 0; y < g->rows; y++) {
        volatile const UInt32 *p = (volatile const UInt32 *)(g->base + y * g->rowBytes);
        for (x = 0; x < g->bytes / 4; x++)
            acc += p[x];
    }
    (void)acc;
}

static void read64(const MemSpan *g)
{
    UInt32 y, x;
    double acc = 0;
    for (y = 0; y < g->rows; y++) {
        volatile const double *p = (volatile const double *)(g->base + y * g->rowBytes);
        for (x = 0; x < g->bytes / 8; x++)
            acc = p[x];
    }
    (void)acc;
}

/* AltiVec: the Retro68 assembler does not know lvx/stvx → .long. VRSAVE marks v0 as used. */
static void vec_row(volatile const UInt8 *src, volatile UInt8 *dst, UInt32 bytes)
{
    register volatile const UInt8 *a __asm__("r9") = src;
    register volatile UInt8 *b __asm__("r10") = dst;
    register UInt32 n __asm__("r8") = bytes >> 4;
    __asm__ volatile(
        ".long 0x7D6042A6\n\t"     /* mfspr r11,VRSAVE */
        "oris 12,11,0x8000\n\t"
        ".long 0x7D8043A6\n\t"     /* mtspr VRSAVE,r12 */
        "mtctr 8\n"
        "1:\n\t"
        ".long 0x7C0048CE\n\t"     /* lvx v0,0,r9 */
        "cmpwi 10,0\n\t"
        "beq 2f\n\t"
        ".long 0x7C0051CE\n\t"     /* stvx v0,0,r10 */
        "addi 10,10,16\n"
        "2:\n\t"
        "addi 9,9,16\n\t"
        "bdnz 1b\n\t"
        ".long 0x7D6043A6"         /* mtspr VRSAVE,r11 */
        : "+r"(a), "+r"(b), "+r"(n) : : "ctr", "r11", "r12", "cr0", "memory");
}

static void read128(const MemSpan *g)
{
    UInt32 y;
    for (y = 0; y < g->rows; y++)
        vec_row(g->base + y * g->rowBytes, 0, g->bytes);
}

static void copy128(const MemSpan *src, const MemSpan *dst)
{
    UInt32 y;
    for (y = 0; y < src->rows; y++)
        vec_row(src->base + y * src->rowBytes, dst->base + y * dst->rowBytes, src->bytes);
}

/* Runs `body` repeatedly for ~2 s, returns repetitions per second ×100. */
#define RUN(var, body) do { \
        UInt32 t0 = TickCount(), n = 0; \
        while (TickCount() == t0) ; \
        t0 = TickCount(); \
        while (TickCount() - t0 < TEST_TICKS) { body; n++; } \
        var = n * 6000 / (TickCount() - t0); \
    } while (0)

#define RUN1(var, body) do { \
        UInt32 t0 = TickCount(), n = 0; \
        while (TickCount() == t0) ; \
        t0 = TickCount(); \
        while (TickCount() - t0 < RAW_TICKS) { body; n++; } \
        var = n * 6000 / (TickCount() - t0); \
    } while (0)

int main(void)
{
    WindowPtr win;
    GWorldPtr gw = 0;
    PixMapHandle gpm = 0, spm;
    Rect wr, r, r2;
    UInt32 fill, s2s, o2s, s2o, text, bpp, bytes, raw = 0, rd = 0, rawRates[6] = { 0 };
    char buf[128];
    FILE *f;
    int i;

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(0);
    InitCursor();

    spm = (**GetMainDevice()).gdPMap;
    bpp = (**spm).pixelSize;
    bytes = (W - 8) * H * bpp / 8;

    SetRect(&wr, 40, 60, 40 + W + 16, 60 + H + 16);
    win = NewCWindow(0, &wr, "\pNV34 Bench", true, documentProc, (WindowPtr)-1, false, 0);
    SetPort(win);
    TextFont(kFontIDMonaco);
    TextSize(9);

    SetRect(&r, 8, 8, 8 + W, 8 + H);
    r2 = r;
    OffsetRect(&r2, 8, 0);
    r2.right -= 8;
    r.right -= 8;

    if (NewGWorld(&gw, 0, &r, 0, 0, 0) == 0) {
        gpm = GetGWorldPixMap(gw);
        LockPixels(gpm);
    }

    /* 1. rectangle fill (write only) */
    RUN(fill, { ForeColor((n & 1) ? blackColor : redColor); PaintRect(&r); });
    ForeColor(blackColor);

    /* 2. screen → screen (VRAM read + write, like dragging a window / scrolling) */
    RUN(s2s, CopyBits(&win->portBits, &win->portBits, (n & 1) ? &r2 : &r, (n & 1) ? &r : &r2, srcCopy, 0));

    /* 3. offscreen → screen (like redrawing a window from a buffer) */
    o2s = s2o = 0;
    if (gw) {
        RUN(o2s, CopyBits((BitMap *)*gpm, &win->portBits, &r, &r, srcCopy, 0));
        /* 4. screen → offscreen (pure VRAM read) */
        RUN(s2o, CopyBits(&win->portBits, (BitMap *)*gpm, &r, &r, srcCopy, 0));
    }

    /* 5. text: 40 lines of Monaco 9 per repetition */
    EraseRect(&r);
    RUN(text, {
        for (i = 0; i < 40; i++) {
            MoveTo(10, 20 + i * 9);
            DrawString("\pThe quick brown fox jumps over the lazy dog 0123456789");
        }
    });

    /* 6. direct VRAM read/write with various widths + RAM for comparison (shadow FB) */
    {
        Point tl = { 0, 0 };
        UInt32 rb = (**spm).rowBytes & 0x3fff, off;
        MemSpan vr, rr;
        Ptr ram;
        UInt32 rd32, rd64, rd128, wr128, ram128, r2v;

        SetPort(win);
        tl.h = r.left;
        tl.v = r.top;
        LocalToGlobal(&tl);
        off = tl.v * rb + tl.h * (bpp / 8);
        off = (off + 15) & ~15UL;
        vr.base = (volatile UInt8 *)(**spm).baseAddr + off;
        vr.rowBytes = rb;
        vr.bytes = ((r.right - r.left - 16) * (bpp / 8)) & ~15UL;
        vr.rows = r.bottom - r.top;
        raw = vr.bytes * vr.rows;

        RUN1(rd32, read32(&vr));
        RUN1(rd64, read64(&vr));
        RUN1(rd128, read128(&vr));
        ram = NewPtr(raw + 16);
        rd = (UInt32)raw;
        ram128 = r2v = 0;
        if (ram) {
            rr.base = (volatile UInt8 *)(((UInt32)ram + 15) & ~15UL);
            rr.rowBytes = vr.bytes;
            rr.bytes = vr.bytes;
            rr.rows = vr.rows;
            RUN1(ram128, read128(&rr));
            RUN1(r2v, copy128(&rr, &vr));
            DisposePtr(ram);
        }
        RUN1(wr128, copy128(&vr, &vr));     /* VRAM read and write */
        EraseRect(&r);
        rawRates[0] = rd32;
        rawRates[1] = rd64;
        rawRates[2] = rd128;
        rawRates[3] = ram128;
        rawRates[4] = r2v;
        rawRates[5] = wr128;
    }

    driver_label(buf);
    line("NV34 Bench 2");
    line(buf);
    sprintf(buf, "screen %d x %d, %lu bpp; test %d x %d", qd.screenBits.bounds.right,
            qd.screenBits.bounds.bottom, bpp, W - 8, H);
    line(buf);
    sprintf(buf, "fill       %4lu.%02lu /s  %6lu KB/s", fill / 100, fill % 100, fill * (bytes / 1024) / 100);
    line(buf);
    sprintf(buf, "scr->scr   %4lu.%02lu /s  %6lu KB/s", s2s / 100, s2s % 100, s2s * (bytes / 1024) / 100);
    line(buf);
    sprintf(buf, "off->scr   %4lu.%02lu /s  %6lu KB/s", o2s / 100, o2s % 100, o2s * (bytes / 1024) / 100);
    line(buf);
    sprintf(buf, "scr->off   %4lu.%02lu /s  %6lu KB/s", s2o / 100, s2o % 100, s2o * (bytes / 1024) / 100);
    line(buf);
    sprintf(buf, "text       %4lu.%02lu /s  (40 lines)", text / 100, text % 100);
    line(buf);
    {
        static const char *names[] = { "VRAM rd  4B", "VRAM rd  8B", "VRAM rd 16B",
                                       "RAM  rd 16B", "RAM->VRAM 16B", "VRAM->VRAM 16B" };
        for (i = 0; i < 6; i++) {
            sprintf(buf, "%-15s %7lu KB/s", names[i], rawRates[i] * (raw / 1024) / 100);
            line(buf);
        }
    }

    f = fopen("NV34 Bench.txt", "a");
    if (f) {
        for (i = 0; i < gLineCount; i++)
            fprintf(f, "%s\n", gLines[i]);
        fprintf(f, "\n");
        fclose(f);
        line("appended to \"NV34 Bench.txt\"");
    }
    line("click = quit");
    show(win);

    while (!Button())
        ;
    if (gw)
        DisposeGWorld(gw);
    return 0;
}

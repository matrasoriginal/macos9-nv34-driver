/*
 * NV34 QD Accel — hardvérová akcelerácia CopyBits obrazovka → obrazovka (krok 2B fázy 3).
 *
 * PPC fragment bez runtime aplikácie. 68k INIT (init68k.c) ho pri štarte načíta do
 * systémovej haldy a zavolá nv34qd_install(): inicializuje 2D engine (accel/nv34_accel.c)
 * a nainštaluje patch trapu CopyBits. Patch nainštalovaný počas INIT fázy je globálny
 * (patch z aplikácie Process Manager obmedzí na jej kontext — overené v qd-2: calls 1).
 *
 * Patch akceleruje iba bezpečný prípad, všetko ostatné ide na pôvodný CopyBits:
 *   - zdroj aj cieľ = framebuffer hlavnej obrazovky (rovnaký baseAddr/rowBytes/hĺbka),
 *   - srcCopy, rovnaká veľkosť zdroja a cieľa, čierne popredie a biele pozadie portu,
 *   - cieľ = bitmapa aktuálneho portu (klip = visRgn ∩ clipRgn ∩ maskRgn), alebo iná
 *     bitmapa obrazovky bez portu (klip = maskRgn),
 *   - klip sa rozloží na obdĺžniky, poradie podľa smeru posunu (prekryv zdroja a cieľa).
 *
 * Vypínače (Open Firmware boot-args): nv34off, nv34noaccel. Štatistiky: property
 * "NV34ACCEL,stats" na NVDA,Display-A (adresa štruktúry, číta NV34 Info).
 */
#include <Quickdraw.h>
#include <MacMemory.h>

#include "../accel/nv34_accel.h"
#include "../accel/nv34_shared.h"

typedef SInt32 OSStatus;
typedef UInt32 ProcInfoType;
typedef struct RegEntryID { UInt32 contents[4]; } RegEntryID;

extern OSStatus RegistryCStrEntryLookup(const RegEntryID *start, const char *path, RegEntryID *found);
extern OSStatus RegistryPropertyGet(const RegEntryID *e, const char *name, void *value, UInt32 *size);
extern OSStatus RegistryPropertyCreate(const RegEntryID *e, const char *name, const void *value, UInt32 size);
extern OSStatus RegistryPropertySet(const RegEntryID *e, const char *name, const void *value, UInt32 size);
extern OSStatus RegistryEntryIDDispose(RegEntryID *id);

extern UniversalProcPtr NGetTrapAddress(UInt16 trapNum, SInt8 tTyp);
extern void NSetTrapAddress(UniversalProcPtr trapAddr, UInt16 trapNum, SInt8 tTyp);
extern long CallUniversalProc(UniversalProcPtr proc, ProcInfoType info, ...);

#define TRAP_COPYBITS     0xA8EC
#define TRAP_COPYMASK     0xA817
#define TRAP_COPYDEEPMASK 0xAA51

/* pascal void CopyMask(BitMap*, BitMap*, BitMap*, Rect*, Rect*, Rect*) */
#define uppCopyMaskProcInfo     0x0003FFC0
/* pascal void CopyDeepMask(BitMap*, BitMap*, BitMap*, Rect*, Rect*, Rect*, short, RgnHandle) */
#define uppCopyDeepMaskProcInfo 0x003BFFC0

/* časovanie (DriverServicesLib) */
typedef struct { UInt32 hi, lo; } AbsTime;
extern AbsTime UpTime(void);
extern SInt32 AbsoluteDeltaToDuration(AbsTime leftAbsoluteTime, AbsTime rightAbsoluteTime);

static UInt32 elapsed_us(AbsTime t0)
{
    SInt32 d = AbsoluteDeltaToDuration(UpTime(), t0);   /* > 0 ms, < 0 µs */
    return d < 0 ? (UInt32)-d : (UInt32)d * 1000;
}

/* pascal void CopyBits(BitMap*, BitMap*, Rect*, Rect*, short, RgnHandle) */
#define uppCopyBitsProcInfo 0x0003BFC0

#define MAX_RECTS   512
#define MAX_XS      256


/* bez libc */
void *memset(void *d, int c, unsigned long n)
{
    unsigned char *p = d;
    while (n--)
        *p++ = (unsigned char)c;
    return d;
}

static int contains(const char *s, const char *w)
{
    int i, j;
    for (i = 0; s[i]; i++) {
        for (j = 0; w[j] && s[i + j] == w[j]; j++)
            ;
        if (!w[j])
            return 1;
    }
    return 0;
}

/* ---- štatistiky (číta NV34 Info) -------------------------------------------- */

typedef struct Stats {
    UInt32 magic;           /* 'NVQS' */
    UInt32 version;
    UInt32 calls;           /* všetky CopyBits */
    UInt32 accelerated;     /* vybavené GPU */
    UInt32 rects;           /* GPU obdĺžniky */
    UInt32 pixels;          /* GPU pixely (÷ 1024) */
    UInt32 notScreen;       /* zdroj alebo cieľ nie je obrazovka */
    UInt32 notSrcCopy;      /* iný transfer mód */
    UInt32 scaled;          /* rôzna veľkosť */
    UInt32 colorized;       /* popredie/pozadie nie čierna/biela, iná CLUT */
    UInt32 complexRgn;      /* príliš veľa obdĺžnikov */
    UInt32 busy;            /* rekurzia */
    UInt32 timeouts;
    UInt32 reinits;
    UInt32 surfaceResets;
    UInt32 disabled;        /* 1 = akcelerácia vypnutá po chybe */
    /* v2: diagnostika inštalácie */
    UInt32 stage;           /* 1 hw, 2 engine, 3 UPP, 4 nainštalované, 5 samotest hotový, 9 koniec */
    UInt32 myUPP;           /* náš routine descriptor */
    UInt32 oldTrap;         /* pôvodná adresa trapu */
    UInt32 trapAfterSet;    /* NGetTrapAddress hneď po NSetTrapAddress */
    UInt32 trapNow;         /* NGetTrapAddress v event loope */
    UInt32 heartbeat;       /* počet prechodov event loopu */
    UInt32 selfTestCalls;   /* koľko volaní patchu vyvolal vlastný CopyBits (0 = PPC volania ho obchádzajú) */
    /* v4: profil času (µs) — kde Finder trávi čas */
    UInt32 accelUs;         /* GPU cesta vrátane klipu a sync */
    UInt32 dstScrCalls, dstScrUs;   /* CPU CopyBits: cieľ obrazovka, zdroj nie (offscreen → obrazovka) */
    UInt32 srcScrCalls, srcScrUs;   /* CPU CopyBits: zdroj obrazovka (čítanie VRAM) */
    UInt32 otherCalls, otherUs;     /* CPU CopyBits: ostatné (offscreen ↔ offscreen, iný mód na obrazovku…) */
    UInt32 maskCalls, maskUs;       /* CopyMask (pass-through) */
    UInt32 deepCalls, deepUs;       /* CopyDeepMask (pass-through) */
    UInt32 maskDstScr, deepDstScr;  /* z toho s cieľom na obrazovke */
    /* v5: rozpad kategórie "ostatné": index = srcVRAM*4 + dstVRAM*2 + (mode == srcCopy) */
    UInt32 otherBucketCalls[8], otherBucketUs[8];
    /* najdrahšie volanie v kategórii "ostatné" */
    UInt32 worstUs, worstMode, worstSrcBase, worstDstBase, worstSrcRB, worstDstRB;
    UInt32 worstDepths;     /* src << 16 | dst */
    UInt32 worstSize;       /* w << 16 | h */
    UInt32 worstMask;       /* maskRgn != 0 */
} Stats;

static Stats gStats;

/* ---- stav ---------------------------------------------------------------- */

static NVAccel gA;
static UniversalProcPtr gOldCopyBits;
static UniversalProcPtr gMyCopyBitsUPP;
static volatile int gBusy;
static int gEngineOK;
static Ptr gSurfBase;
static UInt32 gSurfRowBytes, gSurfDepth;

static NVU32 ticks(void)
{
    return TickCount();
}

/* ---- zdieľanie s RAVE enginom (accel/nv34_shared.h) ------------------------ */

static NV3D gG;
static NV34Shared gShared;
static int ensure_surface(PixMapPtr screen);

static long qd_ensure3D(void)
{
    PixMapPtr pm;
    NVU32 off, rb;

    if (gShared.state3D == 2 || !gEngineOK || gStats.disabled || gBusy)
        return -1;
    if (gShared.state3D == 1) {
        /* 2D povrch podľa aktuálnej obrazovky (RAVE blit po zmene hĺbky) */
        int ok;
        gBusy = 1;
        ok = ensure_surface(*(*GetMainDevice())->gdPMap);
        gBusy = 0;
        return ok ? 0 : -1;
    }
    gBusy = 1;
    pm = *(*GetMainDevice())->gdPMap;
    if (!ensure_surface(pm) || nva_sync(&gA, 60) != kNVAOK)
        goto fail;
    rb = pm->rowBytes & 0x3fff;
    off = (NVU32)((volatile NVU8 *)pm->baseAddr - gA.vram);
    if (nv3d_init(&gG, &gA, off, rb, pm->bounds.right - pm->bounds.left,
                  pm->bounds.bottom - pm->bounds.top) != kNVAOK)
        goto fail;
    if (nva_restore_2d(&gA) != kNVAOK || nva_sync(&gA, 60) != kNVAOK)
        goto fail;
    gShared.state3D = 1;
    gShared.generation++;
    gBusy = 0;
    return 0;

fail:
    gShared.state3D = 2;
    gStats.disabled = 1;        /* GPU v neznámom stave → 2D radšej softvérovo */
    gBusy = 0;
    return -1;
}

static void qd_restore2D(void)
{
    if (gEngineOK && !gStats.disabled)
        nva_restore_2d(&gA);
}

/* Push buffer (opt-2): 512 KB od 4 MB — viac ako celá snímka Quake II (~430 KB), CPU a GPU
 * bežia súbežne; predtým 32 KB na konci VRAM a CPU čakal ~13× za snímku. */
#define PUSH_START  0x00400000
#define PUSH_SIZE   0x00080000

/* Alokátor VRAM pre 3D: od 4,5 MB (pod tým framebuffer, fragment programy NV3D, push buffer)
 * po 31 MB (nad tým inštančná pamäť GPU); bloky zoradené podľa offsetu, voľné sa zlučujú. */
#define ARENA_START 0x00480000
#define ARENA_END   0x01F00000
#define MAX_BLOCKS  4096                /* rave-5: UT má stovky malých textúr */

typedef struct { NVU32 off, size, used; } Block;
static Block gBlocks[MAX_BLOCKS];
static int gBlockCount;
static char gLog[NV34_LOG_SIZE];

static NVU32 vram_alloc(NVU32 size, NVU32 align)
{
    int i, j;

    if (!gBlockCount) {
        gBlocks[0].off = ARENA_START;
        gBlocks[0].size = ARENA_END - ARENA_START;
        gBlocks[0].used = 0;
        gBlockCount = 1;
    }
    if (align < 256)
        align = 256;
    size = (size + 255) & ~255UL;
    for (i = 0; i < gBlockCount; i++) {
        Block *b = &gBlocks[i];
        NVU32 start = (b->off + align - 1) & ~(align - 1);
        NVU32 pad = start - b->off;
        if (b->used || b->size < pad + size)
            continue;
        if (gBlockCount + 2 > MAX_BLOCKS)
            return 0;
        if (pad) {                      /* voľný kúsok pred zarovnaním */
            for (j = gBlockCount; j > i; j--)
                gBlocks[j] = gBlocks[j - 1];
            gBlockCount++;
            gBlocks[i].size = pad;
            i++;
            b = &gBlocks[i];
            b->off = start;
            b->size -= pad;
        }
        if (b->size > size) {           /* zvyšok za blokom */
            for (j = gBlockCount; j > i + 1; j--)
                gBlocks[j] = gBlocks[j - 1];
            gBlockCount++;
            gBlocks[i + 1].off = start + size;
            gBlocks[i + 1].size = b->size - size;
            gBlocks[i + 1].used = 0;
            b->size = size;
        }
        b->used = 1;
        return start;
    }
    return 0;
}

static void vram_free(NVU32 off)
{
    int i, j;
    for (i = 0; i < gBlockCount; i++)
        if (gBlocks[i].off == off && gBlocks[i].used)
            break;
    if (i == gBlockCount)
        return;
    gBlocks[i].used = 0;
    if (i + 1 < gBlockCount && !gBlocks[i + 1].used) {
        gBlocks[i].size += gBlocks[i + 1].size;
        for (j = i + 1; j < gBlockCount - 1; j++)
            gBlocks[j] = gBlocks[j + 1];
        gBlockCount--;
    }
    if (i > 0 && !gBlocks[i - 1].used) {
        gBlocks[i - 1].size += gBlocks[i].size;
        for (j = i; j < gBlockCount - 1; j++)
            gBlocks[j] = gBlocks[j + 1];
        gBlockCount--;
    }
}

static NVU32 vram_free_bytes(void)
{
    NVU32 n = 0;
    int i;
    if (!gBlockCount)
        return ARENA_END - ARENA_START;
    for (i = 0; i < gBlockCount; i++)
        if (!gBlocks[i].used)
            n += gBlocks[i].size;
    return n;
}

static void publish_shared(void)
{
    RegEntryID e;
    UInt32 addr = (UInt32)&gShared;

    gShared.magic = NV34_SHARED_MAGIC;
    gShared.version = NV34_SHARED_VERSION;
    gShared.a = &gA;
    gShared.g = &gG;
    gShared.ensure3D = qd_ensure3D;
    gShared.restore2D = qd_restore2D;
    gShared.busy = &gBusy;
    gShared.vramAlloc = vram_alloc;
    gShared.vramFree = vram_free;
    gShared.vramFreeBytes = vram_free_bytes;
    gShared.nextCtxId = 1;
    gShared.log = gLog;
    gShared.logSize = NV34_LOG_SIZE;
    if (RegistryCStrEntryLookup(0, "Devices:device-tree:pci:NVDA,Parent:NVDA,Display-A", &e) != 0)
        return;
    if (RegistryPropertyCreate(&e, "NV34ACCEL,shared", &addr, 4) != 0)
        RegistryPropertySet(&e, "NV34ACCEL,shared", &addr, 4);
    RegistryEntryIDDispose(&e);
}

/* ---- hardvér ------------------------------------------------------------- */

static int find_hw(void)
{
    RegEntryID e;
    UInt32 assigned[5 * 8], logical[8], mem[2], size, n, i;

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
                    gA.mmio = (volatile NVU8 *)logical[i];
                else if ((assigned[i * 5] & 0xff) == 0x14)
                    gA.vram = (volatile NVU8 *)logical[i];
            }
        }
    }
    RegistryEntryIDDispose(&e);

    gA.vramSize = 0x02000000;
    if (RegistryCStrEntryLookup(0, "Devices:device-tree:pci:NVDA,Parent:NVDA,Display-A", &e) == 0) {
        size = sizeof(mem);
        if (RegistryPropertyGet(&e, "VRAM,memsize", mem, &size) == 0 && size >= 4)
            gA.vramSize = mem[0];
        RegistryEntryIDDispose(&e);
    }
    if (!gA.mmio || !gA.vram)
        return 0;
    gA.bigEndianRegs = (*(volatile UInt32 *)(gA.mmio + 4) == 0x01000001);
    /* Objekty v LE režime: blit je potom presná kópia bajtov v každej hĺbke. BE príznak
     * (xf86 na PPC) prehadzuje 32-bit slová — pri 32 bpp neškodné, pri 16/8 bpp prehodí
     * susedné pixely pri posune o nepárny počet (qd-6: Thousands rozbité). Výplne nepoužívame. */
    gA.beObjects = 0;
    gA.rawFormats = 1;
    gA.ticks = ticks;
    gA.dmaOffset = PUSH_START;
    gA.dmaSize = PUSH_SIZE;
    return ((nva_rd32(&gA, 0) >> 20) & 0x1ff) == 0x34;
}

static int boot_arg(const char *word)
{
    RegEntryID opt;
    char args[256];
    UInt32 size = sizeof(args) - 1;
    int found = 0;

    if (RegistryCStrEntryLookup(0, "Devices:device-tree:options", &opt) != 0)
        return 0;
    if (RegistryPropertyGet(&opt, "boot-args", args, &size) == 0) {
        args[size] = 0;
        found = contains(args, word);
    }
    RegistryEntryIDDispose(&opt);
    return found;
}

static void publish_stats(void)
{
    RegEntryID e;
    UInt32 addr = (UInt32)&gStats;

    gStats.magic = 'NVQS';
    gStats.version = 2;
    if (RegistryCStrEntryLookup(0, "Devices:device-tree:pci:NVDA,Parent:NVDA,Display-A", &e) != 0)
        return;
    if (RegistryPropertyCreate(&e, "NV34ACCEL,stats", &addr, 4) != 0)
        RegistryPropertySet(&e, "NV34ACCEL,stats", &addr, 4);
    RegistryEntryIDDispose(&e);
}

/* Povrch enginu musí zodpovedať aktuálnej obrazovke (driver mení hĺbku/rozlíšenie). */
static int ensure_surface(PixMapPtr screen)
{
    UInt32 rb = screen->rowBytes & 0x3fff;
    UInt32 depth = screen->pixelSize;
    NVU32 off;

    if (screen->baseAddr == gSurfBase && rb == gSurfRowBytes && depth == gSurfDepth)
        return 1;
    off = (NVU32)((volatile NVU8 *)screen->baseAddr - gA.vram);
    if (off >= gA.vramSize || (off & 63) || (rb & 63))
        return 0;
    if (nva_sync(&gA, 30) != kNVAOK)
        return 0;
    /* prvýkrát po inite celý reset (push buffer od nuly), potom iba nové príkazy povrchu —
     * reset za behu by GPU nechal čítať staré príkazy od starého GET (qd-7: Thousands). */
    if (!gSurfBase) {
        if (nva_reset_graphics(&gA, depth, rb, off) != kNVAOK)
            return 0;
    } else if (nva_set_surface(&gA, depth, rb, off) != kNVAOK) {
        return 0;
    }
    gSurfBase = screen->baseAddr;
    gSurfRowBytes = rb;
    gSurfDepth = depth;
    gStats.surfaceResets++;
    return 1;
}

static int engine_start(void)
{
    nva_init_engine(&gA);
    gSurfBase = 0;
    if (nva_sync(&gA, 60) != kNVAOK)
        return 0;
    return 1;
}

/* ---- regióny ------------------------------------------------------------- */

typedef struct { short top, left, bottom, right; } R;

static R gRects[MAX_RECTS];
static int gRectCount;

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
        return -1;
    for (j = n; j > i; j--)
        xs[j] = xs[j - 1];
    xs[i] = x;
    return n + 1;
}

/* Klasický QuickDraw región → obdĺžniky (pásy zhora nadol, v páse zľava doprava). */
static int region_rects(RgnHandle rgn)
{
    Region *r = *rgn;
    short xs[MAX_XS];
    const short *p;
    int n = 0, i;
    short y, prevY = 0;

    gRectCount = 0;
    if (r->rgnBBox.top >= r->rgnBBox.bottom || r->rgnBBox.left >= r->rgnBBox.right)
        return 1;
    if (r->rgnSize == 10) {
        gRects[0].top = r->rgnBBox.top;
        gRects[0].left = r->rgnBBox.left;
        gRects[0].bottom = r->rgnBBox.bottom;
        gRects[0].right = r->rgnBBox.right;
        gRectCount = 1;
        return 1;
    }
    p = (const short *)((const char *)r + 10);
    for (;;) {
        short x;
        y = *p++;
        if (y == 0x7FFF)
            break;
        if (n > 0 && y > prevY) {
            for (i = 0; i + 1 < n; i += 2) {
                if (gRectCount >= MAX_RECTS)
                    return 0;
                gRects[gRectCount].top = prevY;
                gRects[gRectCount].bottom = y;
                gRects[gRectCount].left = xs[i];
                gRects[gRectCount].right = xs[i + 1];
                gRectCount++;
            }
        }
        while ((x = *p++) != 0x7FFF) {
            n = toggle_x(xs, n, x);
            if (n < 0)
                return 0;
        }
        prevY = y;
    }
    return 1;
}

/* ---- patch --------------------------------------------------------------- */

/* BitMap* z CopyBits → PixMap (CGrafPort portBits = handle, PixMap = bit 15). */
static PixMapPtr as_pixmap(const BitMap *b)
{
    short rb = b->rowBytes;
    if ((rb & 0xC000) == 0xC000)
        return *(PixMapHandle)b->baseAddr;
    if (rb & 0x8000)
        return (PixMapPtr)b;
    return 0;
}

static int is_screen(PixMapPtr pm, PixMapPtr screen)
{
    return pm && pm->baseAddr == screen->baseAddr &&
           (pm->rowBytes & 0x3fff) == (screen->rowBytes & 0x3fff) && pm->pixelSize == screen->pixelSize;
}

/* Zdroj/cieľ ako oblasť obrazovky. Okrem PixMapu obrazovky berieme aj klasickú 1-bit BitMap
 * s baseAddr framebuffera (screenBits, port Window Managera) — QuickDraw ju nahradí PixMapom
 * zariadenia; súradnice určuje jej bounds. Presun okien ide práve takto (qd-5: 76 volaní, 31 s). */
typedef struct {
    Rect        bounds;
    CTabHandle  ct;         /* 0 pri 1-bit aliase */
} Surf;

static int resolve_screen(const BitMap *b, PixMapPtr screen, Surf *s)
{
    PixMapPtr pm = as_pixmap(b);
    if (pm) {
        if (!is_screen(pm, screen))
            return 0;
        s->bounds = pm->bounds;
        s->ct = pm->pmTable;
        return 1;
    }
    if (b->baseAddr == screen->baseAddr) {
        s->bounds = b->bounds;
        s->ct = 0;
        return 1;
    }
    return 0;
}

static int plain_colors(GrafPtr port)
{
    if ((port->portBits.rowBytes & 0xC000) == 0xC000) {
        CGrafPtr c = (CGrafPtr)port;
        return c->rgbFgColor.red == 0 && c->rgbFgColor.green == 0 && c->rgbFgColor.blue == 0 &&
               c->rgbBkColor.red == 0xffff && c->rgbBkColor.green == 0xffff && c->rgbBkColor.blue == 0xffff;
    }
    return port->fgColor == blackColor && port->bkColor == whiteColor;
}

static void call_old(BitMap *src, BitMap *dst, Rect *sr, Rect *dr, short mode, RgnHandle mask)
{
    CallUniversalProc(gOldCopyBits, uppCopyBitsProcInfo, src, dst, sr, dr, (long)mode, mask);
}

static int accel_copy(BitMap *src, BitMap *dst, Rect *sr, Rect *dr, short mode, RgnHandle mask)
{
    PixMapHandle sh = (**GetMainDevice()).gdPMap;
    PixMapPtr screen = *sh;
    Surf ss, ds;
    GrafPtr port;
    RgnHandle clip, tmp;
    Rect gbox;
    Point zero = { 0, 0 };
    int dxl, dyl, sxo, syo, dxo, dyo, i, ok = 0, w, h;

    if (mode != srcCopy) {
        gStats.notSrcCopy++;
        return 0;
    }
    if (!resolve_screen(src, screen, &ss) || !resolve_screen(dst, screen, &ds)) {
        gStats.notScreen++;
        return 0;
    }
    if (sr->right - sr->left != dr->right - dr->left || sr->bottom - sr->top != dr->bottom - dr->top) {
        gStats.scaled++;
        return 0;
    }
    /* zdroj musí celý ležať v zdrojovej bitmape */
    if (sr->left < ss.bounds.left || sr->top < ss.bounds.top ||
        sr->right > ss.bounds.right || sr->bottom > ss.bounds.bottom) {
        gStats.notScreen++;
        return 0;
    }
    GetPort(&port);
    if (!plain_colors(port) || (ss.ct && ds.ct && (**ss.ct).ctSeed != (**ds.ct).ctSeed)) {
        gStats.colorized++;
        return 0;
    }
    if (!ensure_surface(screen))
        return 0;

    /* klip v lokálnych súradniciach cieľa */
    clip = NewRgn();
    tmp = NewRgn();
    if (!clip || !tmp)
        goto out;
    RectRgn(clip, dr);
    RectRgn(tmp, &ds.bounds);
    SectRgn(clip, tmp, clip);
    if (dst == &port->portBits) {
        SectRgn(clip, port->visRgn, clip);
        SectRgn(clip, port->clipRgn, clip);
    }
    if (mask)
        SectRgn(clip, mask, clip);
    if (!region_rects(clip)) {
        gStats.complexRgn++;
        goto out;
    }
    ok = 1;
    if (gRectCount == 0)
        goto out;

    /* lokálne → pixely obrazovky */
    dxo = -ds.bounds.left;
    dyo = -ds.bounds.top;
    sxo = sr->left - dr->left - ss.bounds.left;
    syo = sr->top - dr->top - ss.bounds.top;
    dxl = (dr->left + dxo) - (sr->left - ss.bounds.left);   /* posun v pixeloch */
    dyl = (dr->top + dyo) - (sr->top - ss.bounds.top);

    gbox = (**clip).rgnBBox;
    OffsetRect(&gbox, dxo, dyo);
    ShieldCursor(&gbox, zero);

    /* poradie: pri posune nadol od spodného pásu, pri posune doprava sprava */
    for (i = 0; i < gRectCount; i++) {
        int k = (dyl > 0 || (dyl == 0 && dxl > 0)) ? gRectCount - 1 - i : i;
        R *q = &gRects[k];
        w = q->right - q->left;
        h = q->bottom - q->top;
        nva_copy(&gA, q->left + sxo, q->top + syo, q->left + dxo, q->top + dyo, w, h);
        gStats.rects++;
        gStats.pixels += (UInt32)(w * h) >> 10;
    }
    if (nva_sync(&gA, 60) != kNVAOK) {
        gStats.timeouts++;
        ok = 0;
        gEngineOK = engine_start();
        gStats.reinits++;
        if (!gEngineOK)
            gStats.disabled = 1;
    }
    ShowCursor();
    if (ok)
        gStats.accelerated++;

out:
    if (clip)
        DisposeRgn(clip);
    if (tmp)
        DisposeRgn(tmp);
    return ok;
}

static int on_screen(const BitMap *b)
{
    PixMapPtr screen = *(**GetMainDevice()).gdPMap;
    Surf s;
    return resolve_screen(b, screen, &s);
}

/* baseAddr / rowBytes / hĺbka pre BitMap aj PixMap */
static void bits_info(const BitMap *b, UInt32 *base, UInt32 *rb, UInt32 *depth)
{
    PixMapPtr pm = as_pixmap(b);
    if (pm) {
        *base = (UInt32)pm->baseAddr;
        *rb = pm->rowBytes & 0x3fff;
        *depth = pm->pixelSize;
    } else {
        *base = (UInt32)b->baseAddr;
        *rb = b->rowBytes & 0x3fff;
        *depth = 1;
    }
}

static int in_vram(UInt32 base)
{
    return base >= (UInt32)gA.vram && base < (UInt32)gA.vram + gA.vramSize;
}

static void timed_old(BitMap *src, BitMap *dst, Rect *sr, Rect *dr, short mode, RgnHandle mask)
{
    AbsTime t0 = UpTime();
    int s = on_screen(src), d = on_screen(dst);
    UInt32 us;
    call_old(src, dst, sr, dr, mode, mask);
    us = elapsed_us(t0);
    if (s) {
        gStats.srcScrCalls++;
        gStats.srcScrUs += us;
    } else if (d && mode == srcCopy) {
        gStats.dstScrCalls++;
        gStats.dstScrUs += us;
    } else {
        UInt32 sb, srb, sd, db, drb, dd;
        int k;
        bits_info(src, &sb, &srb, &sd);
        bits_info(dst, &db, &drb, &dd);
        k = in_vram(sb) * 4 + in_vram(db) * 2 + (mode == srcCopy);
        gStats.otherCalls++;
        gStats.otherUs += us;
        gStats.otherBucketCalls[k]++;
        gStats.otherBucketUs[k] += us;
        if (us > gStats.worstUs) {
            gStats.worstUs = us;
            gStats.worstMode = (UInt16)mode;
            gStats.worstSrcBase = sb;
            gStats.worstDstBase = db;
            gStats.worstSrcRB = srb;
            gStats.worstDstRB = drb;
            gStats.worstDepths = sd << 16 | dd;
            gStats.worstSize = (UInt32)(UInt16)(dr->right - dr->left) << 16 | (UInt16)(dr->bottom - dr->top);
            gStats.worstMask = mask != 0;
        }
    }
}

static void MyCopyBits(BitMap *src, BitMap *dst, Rect *sr, Rect *dr, short mode, RgnHandle mask)
{
    AbsTime t0;

    gStats.calls++;
    if (gBusy || !gEngineOK || gStats.disabled) {
        gStats.busy += gBusy ? 1 : 0;
        call_old(src, dst, sr, dr, mode, mask);
        return;
    }
    gBusy = 1;
    t0 = UpTime();
    if (accel_copy(src, dst, sr, dr, mode, mask))
        gStats.accelUs += elapsed_us(t0);
    else
        timed_old(src, dst, sr, dr, mode, mask);
    gBusy = 0;
}

/* ---- pass-through meranie CopyMask / CopyDeepMask (ikony) ---------------- */

static UniversalProcPtr gOldCopyMask, gOldCopyDeepMask;

static void MyCopyMask(BitMap *src, BitMap *mask, BitMap *dst, Rect *sr, Rect *mr, Rect *dr)
{
    AbsTime t0 = UpTime();
    if (on_screen(dst))
        gStats.maskDstScr++;
    CallUniversalProc(gOldCopyMask, uppCopyMaskProcInfo, src, mask, dst, sr, mr, dr);
    gStats.maskCalls++;
    gStats.maskUs += elapsed_us(t0);
}

static void MyCopyDeepMask(BitMap *src, BitMap *mask, BitMap *dst, Rect *sr, Rect *mr, Rect *dr,
                           short mode, RgnHandle rgn)
{
    AbsTime t0 = UpTime();
    if (on_screen(dst))
        gStats.deepDstScr++;
    CallUniversalProc(gOldCopyDeepMask, uppCopyDeepMaskProcInfo, src, mask, dst, sr, mr, dr, (long)mode, rgn);
    gStats.deepCalls++;
    gStats.deepUs += elapsed_us(t0);
}

/* ---- inštalácia (volá 68k INIT) ---------------------------------------- */

/* Návratová hodnota: 0 = patch nainštalovaný, inak stage, kde sa skončilo. */
long nv34qd_install(void)
{
    THz oldZone = GetZone();

    publish_stats();
    gStats.version = 7;
    if (boot_arg("nv34off") || boot_arg("nv34noaccel")) {
        gStats.stage = 8;
        return 8;
    }
    if (!find_hw())
        return 1;
    gStats.stage = 1;
    gEngineOK = engine_start();
    if (!gEngineOK)
        return 2;
    gStats.stage = 2;
    if (!boot_arg("nv34no3d"))
        publish_shared();

    SetZone(SystemZone());      /* routine descriptor musí prežiť všetky aplikácie */
    gMyCopyBitsUPP = NewRoutineDescriptor((ProcPtr)MyCopyBits, uppCopyBitsProcInfo, kPowerPCISA);
    SetZone(oldZone);
    if (!gMyCopyBitsUPP)
        return 3;
    gStats.myUPP = (UInt32)gMyCopyBitsUPP;
    gStats.stage = 3;

    gOldCopyBits = NGetTrapAddress(TRAP_COPYBITS, kToolboxTrapType);
    gStats.oldTrap = (UInt32)gOldCopyBits;
    NSetTrapAddress(gMyCopyBitsUPP, TRAP_COPYBITS, kToolboxTrapType);
    gStats.trapAfterSet = (UInt32)NGetTrapAddress(TRAP_COPYBITS, kToolboxTrapType);
    gStats.stage = 4;

    /* meranie ikon: CopyMask / CopyDeepMask iba počítajú čas a volajú pôvodné */
    SetZone(SystemZone());
    {
        UniversalProcPtr m = NewRoutineDescriptor((ProcPtr)MyCopyMask, uppCopyMaskProcInfo, kPowerPCISA);
        UniversalProcPtr d = NewRoutineDescriptor((ProcPtr)MyCopyDeepMask, uppCopyDeepMaskProcInfo, kPowerPCISA);
        SetZone(oldZone);
        if (m) {
            gOldCopyMask = NGetTrapAddress(TRAP_COPYMASK, kToolboxTrapType);
            NSetTrapAddress(m, TRAP_COPYMASK, kToolboxTrapType);
        }
        if (d) {
            gOldCopyDeepMask = NGetTrapAddress(TRAP_COPYDEEPMASK, kToolboxTrapType);
            NSetTrapAddress(d, TRAP_COPYDEEPMASK, kToolboxTrapType);
        }
    }
    return 0;
}

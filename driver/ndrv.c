/*
 * NV34 NDRV — minimálny Mac OS 9 video driver pre GeForce FX Go5200 (PowerBook 6,2).
 *
 * Režimy pre interný panel: natívne 1024×768 + škálované 800×600, 640×480,
 * každý v 8 / 16 / 32 bpp. Bez 2D akcelerácie, bez hardvérového kurzora.
 *
 * Štruktúra podľa Apple "Designing PCI Cards and Drivers" a QemuMacDrivers.
 */
#include "ndrv_api.h"
#include "nv34.h"

#define DRV_MAJOR   0x04
#define DRV_MINOR   0x04
#define DRV_STAGE   finalStage
#define DRV_REV     0

/* Cache režim framebuffera pre CPU (0 = nechať systémový default). Na PB 6,2 write-through
 * (2) nemá účinok — OS 9 mapuje AGP priestor necacheovane a čítanie VRAM ostáva ~12 MB/s
 * (NV34 Bench, docs/faza3.md). Ponechané ako experimentálny prepínač. */
#ifndef NV_FB_CACHE
#define NV_FB_CACHE 0
#endif

#ifndef NV_SCALED_MODES
#define NV_SCALED_MODES 1
#endif

/* ---- popis drivera (Expansion Manager matchuje driverType proti `compatible`) -- */

DriverDescription TheDriverDescription = {
    kTheDescriptionSignature,
    kInitialDriverDescriptor,
    { "\pNVDA,NVMac", { DRV_MAJOR, DRV_MINOR, DRV_STAGE, DRV_REV } },
    {
        kDriverIsOpenedUponLoad | kDriverIsUnderExpertControl,
        "\p.Display_NV34",
        { 0, 0, 0, 0, 0, 0, 0, 0 }
    },
    {
        1,
        { { kServiceCategoryNdrvDriver, kNdrvTypeIsVideo, { DRV_MAJOR, DRV_MINOR, DRV_STAGE, DRV_REV } } }
    }
};

/* ---- režimy ------------------------------------------------------------ */

typedef struct ModeDef {
    DisplayModeID   id;
    UInt32          width;
    UInt32          height;
    UInt32          timing;
} ModeDef;

#define MAX_MODES 3

static ModeDef gModes[MAX_MODES];
static int gModeCount;

/* ---- stav drivera -------------------------------------------------------- */

typedef struct GammaBuf {
    GammaTbl    hdr;            /* gFormulaData[0] je prvý bajt dát (gFormulaSize = 0) */
    UInt8       data[3 * 256];
} GammaBuf;

static struct {
    RegEntryID      entry;
    int             entryValid;
    int             opened;
    NVHw            hw;
    UInt32          nativeW, nativeH;
    DisplayModeID   curMode;
    DepthMode       curDepth;
    DisplayModeID   prefMode;
    DepthMode       prefDepth;
    Ptr             vramLogical;
    int             grayMode;
    int             irqEnabled;
    InterruptServiceIDType vblService;
    TimerID         timer;
    volatile int    timerRunning;
    UInt8           clutR[256], clutG[256], clutB[256];    /* logická paleta (pred gamou) */
    UInt8           gamR[256], gamG[256], gamB[256];
    UInt8           hwR[256], hwG[256], hwB[256];
    GammaBuf        gammaTbl;
    UInt32          fbCacheMode;
} G;

/* ---- záznam volaní (pre ladenie bootu) ------------------------------------
 * Kruhový buffer mimo G (prežije memset v inite). Adresa ide do diag d[29];
 * NV34 Info ho číta priamo z pamäte (OS 9 má jeden adresný priestor). */

#define TRACE_SIZE 1024

typedef struct TraceEntry {
    UInt32  what;       /* IOCommandCode << 24 | kind << 16 | csCode */
    UInt32  arg0, arg1; /* prvé dva dwordy záznamu csParam pred volaním */
    UInt32  result;
    UInt32  state;      /* po volaní: curDepth << 16 | curMode << 8 | HW bpp */
    UInt32  pitch;      /* po volaní: HW pitch */
} TraceEntry;

static struct {
    UInt32      magic;  /* 'NVTR' */
    UInt32      count;  /* celkový počet záznamov (index = count % TRACE_SIZE) */
    UInt32      size;   /* TRACE_SIZE */
    TraceEntry  e[TRACE_SIZE];
} gTrace;

/* GCC môže pri -O2 generovať volania memset/memcpy. */
void *memset(void *d, int c, unsigned long n)
{
    UInt8 *p = d;
    while (n--)
        *p++ = (UInt8)c;
    return d;
}

void *memcpy(void *d, const void *s, unsigned long n)
{
    UInt8 *p = d;
    const UInt8 *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}

static UInt32 depth_bpp(DepthMode d)
{
    return d == kDepthMode1 ? 8 : d == kDepthMode2 ? 16 : 32;
}

static DepthMode bpp_depth(UInt32 bpp)
{
    return bpp == 8 ? kDepthMode1 : bpp == 16 ? kDepthMode2 : kDepthMode3;
}

static UInt32 row_bytes(UInt32 width, UInt32 bpp)
{
    return ((width * (bpp >> 3)) + 63) & ~63UL;
}

static const ModeDef *find_mode(DisplayModeID id)
{
    int i;
    for (i = 0; i < gModeCount; i++)
        if (gModes[i].id == id)
            return &gModes[i];
    return 0;
}

static Ptr fb_base(void)
{
    return G.vramLogical + G.hw.fbOffset;
}

static UInt32 mode_flags(const ModeDef *m)
{
    /* hl-6: aj škálované režimy sú kModeSafe — bez neho DrawSprocket pri SetState(Active) vracia
     * kDSpConfirmSwitchWarning (-30448) a Half-Life (SDL) po prepnutí zamrzol; Quake II / UT
     * v 640×480 / 800×600 fungujú */
    UInt32 f = (1 << kModeValid) | (1 << kModeShowNow) | (1 << kModeSafe);
    if (m->width == G.nativeW && m->height == G.nativeH)
        f |= (1 << kModeDefault);
    else
        f |= (1 << kModeStretched);
    return f;
}

static void build_modes(void)
{
    static const struct { UInt32 w, h, timing; } candidates[] = {
        { 1024, 768, timingVESA_1024x768_60hz },
        { 800, 600, timingVESA_800x600_60hz },
        { 640, 480, timingVESA_640x480_60hz },
    };
    int i;

    gModeCount = 0;
    gModes[gModeCount].id = 1;
    gModes[gModeCount].width = G.nativeW;
    gModes[gModeCount].height = G.nativeH;
    gModes[gModeCount].timing = timingVESA_1024x768_60hz;
    for (i = 0; i < 3; i++)
        if (candidates[i].w == G.nativeW && candidates[i].h == G.nativeH)
            gModes[gModeCount].timing = candidates[i].timing;
    gModeCount++;
#if NV_SCALED_MODES
    for (i = 0; i < 3; i++) {
        if (candidates[i].w >= G.nativeW || candidates[i].h >= G.nativeH)
            continue;
        gModes[gModeCount].id = gModeCount + 1;
        gModes[gModeCount].width = candidates[i].w;
        gModes[gModeCount].height = candidates[i].h;
        gModes[gModeCount].timing = candidates[i].timing;
        gModeCount++;
    }
#endif
}

/* ---- Name Registry ------------------------------------------------------ */

static OSStatus get_prop(const RegEntryID *e, const char *name, void *buf, UInt32 max, UInt32 *outSize)
{
    RegPropertyValueSize size;
    OSStatus err = RegistryPropertyGetSize(e, name, &size);
    if (err)
        return err;
    if (size > max)
        return paramErr;
    err = RegistryPropertyGet(e, name, buf, &size);
    if (outSize)
        *outSize = size;
    return err;
}

static UInt32 get_prop32(const RegEntryID *e, const char *name, UInt32 dflt)
{
    UInt32 v[2], size;
    if (get_prop(e, name, v, sizeof(v), &size) || size < 4)
        return dflt;
    return v[0];
}

static int has_prop(const RegEntryID *e, const char *name)
{
    RegPropertyValueSize size;
    return RegistryPropertyGetSize(e, name, &size) == noErr;
}

static OSStatus find_parent(const RegEntryID *e, RegEntryID *parent)
{
    char path[256];
    RegPathNameSize size;
    OSStatus err;
    int i, cut = -1;

    err = RegistryEntryToPathSize(e, &size);
    if (err)
        return err;
    if (size > sizeof(path))
        return paramErr;
    err = RegistryCStrEntryToPath(e, path, size);
    if (err)
        return err;
    for (i = 0; path[i]; i++)
        if (path[i] == ':')
            cut = i;
    if (cut <= 0)
        return nrNotFoundErr;
    path[cut] = 0;
    return RegistryCStrEntryLookup(0, path, parent);
}

/* BAR0 = MMIO (config 0x10), BAR1 = framebuffer apertúra (config 0x14). */
static OSStatus map_bars(void)
{
    RegEntryID parent, *pci = &G.entry;
    UInt32 assigned[5 * 8], logical[8];
    UInt32 aSize, lSize, i, n;
    int haveParent = 0;
    OSStatus err;

    if (!has_prop(pci, "assigned-addresses")) {
        err = find_parent(&G.entry, &parent);
        if (err)
            return err;
        pci = &parent;
        haveParent = 1;
    }
    err = get_prop(pci, "assigned-addresses", assigned, sizeof(assigned), &aSize);
    if (!err)
        err = get_prop(pci, "AAPL,address", logical, sizeof(logical), &lSize);
    if (!err) {
        n = aSize / 20;
        if (n > lSize / 4)
            n = lSize / 4;
        for (i = 0; i < n; i++) {
            UInt32 reg = assigned[i * 5] & 0xff;
            if (reg == 0x10)
                G.hw.mmio = (volatile UInt8 *)logical[i];
            else if (reg == 0x14)
                G.vramLogical = (Ptr)logical[i];
        }
    }
    if (haveParent)
        RegistryEntryIDDispose(&parent);

    /* Záloha: OF `address` na display uzle = BAR1 + fboffset. */
    if (!G.vramLogical) {
        UInt32 addr = get_prop32(&G.entry, "address", 0);
        if (addr)
            G.vramLogical = (Ptr)(addr - G.hw.fbOffset);
    }
    if (!G.hw.mmio || !G.vramLogical)
        return nrNotFoundErr;
    return noErr;
}

/* Fázy initu pre diagnostiku (d[12]); d[13] = chybový kód. */
enum {
    kStageOK            = 0,
    kStageNotBuiltIn    = 1,
    kStageNoBars        = 2,
    kStageNotNV34       = 3,
    kStageDisabled      = 4     /* boot-args obsahuje nv34off */
};

static void publish_diag(UInt32 stage, OSStatus err)
{
    UInt32 d[30];
    int h;
    d[0] = (DRV_MAJOR << 24) | (DRV_MINOR << 16) | (DRV_STAGE << 8) | DRV_REV;
    d[1] = G.hw.boot0;
    d[2] = G.hw.head;
    d[3] = G.hw.bigEndianRegs;
    d[4] = (UInt32)G.hw.mmio;
    d[5] = (UInt32)G.vramLogical;
    d[6] = G.hw.fbOffset;
    d[7] = G.hw.vramSize;
    d[8] = G.hw.nat.cr_pixel | G.hw.nat.cr_lfb << 8 | G.hw.nat.cr_offset << 16;
    d[9] = G.hw.nat.gen_ctrl;
    d[10] = G.hw.nat.fp_tg;
    d[11] = G.hw.pcrtcConfig;
    d[12] = stage;
    d[13] = (UInt32)err;
    d[14] = G.hw.byteXor | G.hw.byteLanesVerified << 8 | G.hw.keepEndian << 16;
    d[15] = G.hw.nat.cr_rpc0 | G.hw.nat.cr_42 << 8 | G.hw.nat.cr_ff << 16 | (UInt32)G.hw.nat.cr_fflwm << 24;
    for (h = 0; h < 2; h++) {
        NVHeadDiag *hd = &G.hw.headDiag[h];
        d[16 + h * 6] = hd->cr;
        d[17 + h * 6] = hd->start;
        d[18 + h * 6] = hd->fp_tg;
        d[19 + h * 6] = hd->gen_ctrl;
        d[20 + h * 6] = hd->crtc_cfg;
        d[21 + h * 6] = hd->score | (UInt32)G.hw.cr44 << 8;
    }
    d[28] = G.fbCacheMode;
    d[29] = (UInt32)&gTrace;
    if (RegistryPropertyCreate(&G.entry, "NV34NDRV,diag", d, sizeof(d)) != noErr)
        RegistryPropertySet(&G.entry, "NV34NDRV,diag", d, sizeof(d));
}

/* ---- paleta a gama ------------------------------------------------------- */

static void gamma_linear(void)
{
    int i;
    for (i = 0; i < 256; i++)
        G.gamR[i] = G.gamG[i] = G.gamB[i] = (UInt8)i;
}

static void gamma_publish(void)
{
    GammaBuf *t = &G.gammaTbl;
    t->hdr.gVersion = 0;
    t->hdr.gType = 0;
    t->hdr.gFormulaSize = 0;
    t->hdr.gChanCnt = 3;
    t->hdr.gDataCnt = 256;
    t->hdr.gDataWidth = 8;
    memcpy((UInt8 *)t->hdr.gFormulaData, G.gamR, 256);
    memcpy((UInt8 *)t->hdr.gFormulaData + 256, G.gamG, 256);
    memcpy((UInt8 *)t->hdr.gFormulaData + 512, G.gamB, 256);
}

static void load_hw_range(UInt32 start, UInt32 count)
{
    UInt32 i;
    for (i = start; i < start + count; i++) {
        G.hwR[i] = G.gamR[G.clutR[i]];
        G.hwG[i] = G.gamG[G.clutG[i]];
        G.hwB[i] = G.gamB[G.clutB[i]];
    }
    nv_load_palette(&G.hw, start, count, &G.hwR[start], &G.hwG[start], &G.hwB[start]);
}

/* Direct režimy: CLUT je per-kanálová gama rampa. */
static void clut_identity(void)
{
    int i;
    for (i = 0; i < 256; i++)
        G.clutR[i] = G.clutG[i] = G.clutB[i] = (UInt8)i;
}

/* ---- prepínanie režimov -------------------------------------------------- */

static void gray_fill(void)
{
    const ModeDef *m = find_mode(G.curMode);
    UInt32 bpp = depth_bpp(G.curDepth);
    UInt32 pitch = row_bytes(m->width, bpp);
    UInt32 words = (m->width * (bpp >> 3)) >> 2;
    UInt32 x, y;
    volatile UInt8 *row = (volatile UInt8 *)fb_base();

    for (y = 0; y < m->height; y++, row += pitch) {
        volatile UInt32 *p = (volatile UInt32 *)row;
        UInt32 pat;
        if (bpp == 8)
            pat = (y & 1) ? 0xFF00FF00 : 0x00FF00FF;    /* 0 = biela, 255 = čierna */
        else if (bpp == 16)
            pat = 0x42104210;
        else
            pat = 0x00808080;
        for (x = 0; x < words; x++)
            p[x] = pat;
    }
}

/* hl-4: pred zmenou režimu počkať, kým GPU dokončí push buffer (kanál 0 QD Accel) a PGRAPH je
 * nečinný. Half-Life prepína na celú obrazovku počas 3D (až 512 KB príkazov v rade) → Mac stál;
 * Quake II pred prepnutím ruší kontext (GPU už nečinné) a funguje. Max. 250 ms. */
static void wait_gpu_idle(void)
{
    AbsoluteTime end = AddDurationToAbsolute(250 * durationMillisecond, UpTime());
    for (;;) {
        AbsoluteTime now;
        UInt32 put = nv_rd32(&G.hw, 0x800040), get = nv_rd32(&G.hw, 0x800044);
        if (put == get && nv_rd32(&G.hw, 0x400700) == 0)
            return;
        now = UpTime();
        if (now.hi > end.hi || (now.hi == end.hi && now.lo >= end.lo))
            return;
    }
}

static OSStatus set_mode(DisplayModeID id, DepthMode depth)
{
    const ModeDef *m = find_mode(id);
    UInt32 bpp, pitch;

    if (!m || depth < kDepthMode1 || depth > kDepthMode3)
        return paramErr;
    bpp = depth_bpp(depth);
    pitch = row_bytes(m->width, bpp);
    if (G.hw.fbOffset + pitch * m->height > G.hw.vramSize)
        return paramErr;

    wait_gpu_idle();
    nv_set_mode(&G.hw, m->width, m->height, G.nativeW, G.nativeH, bpp, pitch);
    G.curMode = id;
    G.curDepth = depth;

    if (bpp != 8)
        clut_identity();
    load_hw_range(0, 256);
    return noErr;
}

/* ---- VBL (časovač 60 Hz, ako QemuVGA) ------------------------------------ */

static OSStatus vbl_tick(void *p1, void *p2);

static void arm_timer(void)
{
    AbsoluteTime when = AddDurationToAbsolute(16 * durationMillisecond, UpTime());
    SetInterruptTimer(&when, vbl_tick, 0, &G.timer);
}

static OSStatus vbl_tick(void *p1, void *p2)
{
    (void)p1;
    (void)p2;
    if (!G.timerRunning)
        return noErr;
    if (G.irqEnabled && G.vblService)
        VSLDoInterruptService(G.vblService);
    arm_timer();
    return noErr;
}

/* ---- Control ------------------------------------------------------------- */

static OSStatus do_set_entries(VDSetEntryRecord *e, int direct)
{
    ColorSpec *t = e->csTable;
    SInt32 i, count = e->csCount + 1;
    int isDirect = depth_bpp(G.curDepth) != 8;

    if (!t || count <= 0 || count > 256 || isDirect != direct)
        return controlErr;
    if (e->csStart != -1 && (e->csStart < 0 || e->csStart + count > 256))
        return controlErr;

    for (i = 0; i < count; i++) {
        UInt32 idx = (e->csStart == -1) ? (UInt32)(t[i].value & 0xff) : (UInt32)(e->csStart + i);
        UInt32 r = t[i].red, g = t[i].green, b = t[i].blue;

        if (G.grayMode && !direct)
            r = g = b = (r * 30 + g * 59 + b * 11) / 100;
        if (direct && depth_bpp(G.curDepth) == 16) {
            /* 5-bit kanály: položka i pokrýva HW indexy i*8 .. i*8+7 */
            UInt32 j;
            if (idx >= 32)
                return controlErr;
            for (j = 0; j < 8; j++) {
                G.clutR[idx * 8 + j] = r >> 8;
                G.clutG[idx * 8 + j] = g >> 8;
                G.clutB[idx * 8 + j] = b >> 8;
            }
            load_hw_range(idx * 8, 8);
            continue;
        }
        G.clutR[idx] = r >> 8;
        G.clutG[idx] = g >> 8;
        G.clutB[idx] = b >> 8;
        load_hw_range(idx, 1);
    }
    return noErr;
}

static OSStatus do_set_gamma(VDGammaRecord *rec)
{
    GammaTbl *t = (GammaTbl *)rec->csGTable;

    if (!t) {
        gamma_linear();
    } else {
        const UInt8 *data;
        if (t->gVersion != 0 || t->gDataWidth != 8 || t->gDataCnt != 256 ||
            (t->gChanCnt != 1 && t->gChanCnt != 3))
            return paramErr;
        data = (const UInt8 *)t->gFormulaData + t->gFormulaSize;
        memcpy(G.gamR, data, 256);
        memcpy(G.gamG, t->gChanCnt == 3 ? data + 256 : data, 256);
        memcpy(G.gamB, t->gChanCnt == 3 ? data + 512 : data, 256);
    }
    gamma_publish();
    load_hw_range(0, 256);
    return noErr;
}

static OSStatus driver_control(SInt16 code, void *param)
{
    switch (code) {
    case cscSetMode: {
        VDPageInfo *p = param;
        OSStatus err;
        if (p->csPage != 0)
            return paramErr;
        err = set_mode(G.curMode, (DepthMode)p->csMode);
        p->csBaseAddr = fb_base();
        return err;
    }
    case cscSwitchMode: {
        VDSwitchInfoRec *s = param;
        OSStatus err;
        if (s->csPage != 0)
            return paramErr;
        err = set_mode(s->csData, s->csMode);
        s->csBaseAddr = fb_base();
        return err;
    }
    case cscSetEntries:
        return do_set_entries(param, 0);
    case cscDirectSetEntries:
        return do_set_entries(param, 1);
    case cscSetGamma:
        return do_set_gamma(param);
    case cscGrayPage: {
        VDPageInfo *p = param;
        if (p->csPage != 0)
            return paramErr;
        gray_fill();
        return noErr;
    }
    case cscSetGray:
        G.grayMode = ((VDGrayRecord *)param)->csMode != 0;
        return noErr;
    case cscSetInterrupt:
        G.irqEnabled = ((VDFlagRecord *)param)->csMode == 0;
        return noErr;
    case cscSetDefaultMode:
        return noErr;
    case cscSavePreferredConfiguration: {
        VDSwitchInfoRec *s = param;
        if (!find_mode(s->csData) || s->csMode < kDepthMode1 || s->csMode > kDepthMode3)
            return paramErr;
        G.prefMode = s->csData;
        G.prefDepth = s->csMode;
        return noErr;
    }
    default:
        return controlErr;
    }
}

/* ---- Status -------------------------------------------------------------- */

static OSStatus driver_status(SInt16 code, void *param)
{
    switch (code) {
    case cscGetMode: {
        VDPageInfo *p = param;
        p->csMode = G.curDepth;
        p->csPage = 0;
        p->csBaseAddr = fb_base();
        return noErr;
    }
    case cscGetPages:
        ((VDPageInfo *)param)->csPage = 1;
        return noErr;
    case cscGetBaseAddr: {
        VDPageInfo *p = param;
        if (p->csPage != 0)
            return paramErr;
        p->csBaseAddr = fb_base();
        return noErr;
    }
    case cscGetEntries: {
        VDSetEntryRecord *e = param;
        ColorSpec *t = e->csTable;
        SInt32 i, count = e->csCount + 1;
        if (!t || count <= 0 || count > 256)
            return statusErr;
        for (i = 0; i < count; i++) {
            UInt32 idx = (e->csStart == -1) ? (UInt32)(t[i].value & 0xff) : (UInt32)(e->csStart + i);
            if (idx > 255)
                return statusErr;
            if (e->csStart != -1)
                t[i].value = idx;
            t[i].red = G.clutR[idx] * 0x0101;
            t[i].green = G.clutG[idx] * 0x0101;
            t[i].blue = G.clutB[idx] * 0x0101;
        }
        return noErr;
    }
    case cscGetGray:
        ((VDGrayRecord *)param)->csMode = G.grayMode;
        return noErr;
    case cscGetInterrupt:
        ((VDFlagRecord *)param)->csMode = G.irqEnabled ? 0 : 1;
        return noErr;
    case cscGetGamma:
        ((VDGammaRecord *)param)->csGTable = (Ptr)&G.gammaTbl;
        return noErr;
    case cscGetCurMode: {
        VDSwitchInfoRec *s = param;
        s->csMode = G.curDepth;
        s->csData = G.curMode;
        s->csPage = 0;
        s->csBaseAddr = fb_base();
        return noErr;
    }
    case cscGetConnection: {
        VDDisplayConnectInfoRec *c = param;
        c->csDisplayType = kPanelTFTConnect;
        c->csConnectTaggedType = 0;
        c->csConnectTaggedData = 0;
        /* bez kAllModesSafe: škálované režimy majú ísť cez potvrdenie */
        c->csConnectFlags = (1 << kAllModesValid) | (1 << kHasDirectConnection) | (1 << kBuiltInConnection);
        c->csDisplayComponent = 0;
        return noErr;
    }
    case cscGetModeTiming: {
        VDTimingInfoRec *t = param;
        const ModeDef *m = find_mode(t->csTimingMode);
        if (!m) {
            t->csTimingFlags = 0;
            return paramErr;
        }
        t->csTimingFormat = kDeclROMtables;
        t->csTimingData = m->timing;
        t->csTimingFlags = mode_flags(m);
        return noErr;
    }
    case cscGetPreferredConfiguration: {
        VDSwitchInfoRec *s = param;
        s->csMode = G.prefDepth;
        s->csData = G.prefMode;
        s->csPage = 0;
        s->csBaseAddr = fb_base();
        return noErr;
    }
    case cscGetNextResolution: {
        VDResolutionInfoRec *r = param;
        const ModeDef *m = 0;
        int i;
        if (r->csPreviousDisplayModeID == kDisplayModeIDFindFirstResolution) {
            m = &gModes[0];
        } else if (r->csPreviousDisplayModeID == kDisplayModeIDCurrent) {
            m = find_mode(G.curMode);
        } else {
            for (i = 0; i < gModeCount; i++)
                if (gModes[i].id == r->csPreviousDisplayModeID)
                    break;
            if (i >= gModeCount)
                return paramErr;
            if (i + 1 < gModeCount)
                m = &gModes[i + 1];
        }
        if (!m) {
            r->csDisplayModeID = kDisplayModeIDNoMoreResolutions;
            return noErr;
        }
        r->csDisplayModeID = m->id;
        r->csHorizontalPixels = m->width;
        r->csVerticalLines = m->height;
        r->csRefreshRate = 60L << 16;
        r->csMaxDepthMode = kDepthMode3;
        return noErr;
    }
    case cscGetVideoParameters: {
        VDVideoParametersInfoRec *v = param;
        const ModeDef *m = find_mode(v->csDisplayModeID);
        VPBlock *vp = v->csVPBlockPtr;
        UInt32 bpp;
        if (!m || !vp || v->csDepthMode < kDepthMode1 || v->csDepthMode > kDepthMode3)
            return paramErr;
        bpp = depth_bpp(v->csDepthMode);
        memset(vp, 0, sizeof(*vp));
        vp->vpRowBytes = row_bytes(m->width, bpp);
        vp->vpBounds.right = m->width;
        vp->vpBounds.bottom = m->height;
        vp->vpHRes = 72L << 16;
        vp->vpVRes = 72L << 16;
        vp->vpPixelType = bpp == 8 ? 0 : 16;   /* chunky / RGBDirect */
        vp->vpPixelSize = bpp;
        vp->vpCmpCount = bpp == 8 ? 1 : 3;
        vp->vpCmpSize = bpp == 8 ? 8 : bpp == 16 ? 5 : 8;
        v->csPageCount = 1;
        v->csDeviceType = bpp == 8 ? clutType : directType;
        v->csDepthFlags = 0;
        return noErr;
    }
    default:
        return statusErr;
    }
}

/* ---- životný cyklus ------------------------------------------------------ */

/* Núdzový vypínač: v Open Firmware `setenv boot-args nv34off` → driver sa nenaviaže
 * (Extensions Off/Shift načítanie ndrv z Extensions nezastaví). */
static int disabled_by_boot_args(void)
{
    RegEntryID opt;
    char args[256];
    UInt32 size = sizeof(args) - 1;
    int i, found = 0;

    if (RegistryCStrEntryLookup(0, "Devices:device-tree:options", &opt) != noErr)
        return 0;
    if (RegistryPropertyGet(&opt, "boot-args", args, &size) == noErr) {
        args[size] = 0;
        for (i = 0; i + 7 <= (int)size; i++)
            if (args[i] == 'n' && args[i + 1] == 'v' && args[i + 2] == '3' && args[i + 3] == '4' &&
                args[i + 4] == 'o' && args[i + 5] == 'f' && args[i + 6] == 'f')
                found = 1;
    }
    RegistryEntryIDDispose(&opt);
    return found;
}

static OSStatus driver_initialize(DriverInitInfo *info)
{
    OSStatus err;
    UInt32 bpp;
    int i;

    memset(&G, 0, sizeof(G));
    RegistryEntryIDCopy(&info->deviceEntry, &G.entry);
    G.entryValid = 1;

    if (disabled_by_boot_args()) {
        publish_diag(kStageDisabled, paramErr);
        return paramErr;
    }

    /* Iba interný panel; Display-B (mini-DVI) ostáva pôvodnému driveru. */
    if (!has_prop(&G.entry, "built-in")) {
        publish_diag(kStageNotBuiltIn, paramErr);
        return paramErr;
    }

    G.nativeW = get_prop32(&G.entry, "width", 1024);
    G.nativeH = get_prop32(&G.entry, "height", 768);
    G.hw.fbOffset = get_prop32(&G.entry, "fboffset", 0x4000);

    G.hw.vramSize = get_prop32(&G.entry, "VRAM,memsize", 0x02000000);

    err = map_bars();
    if (err) {
        publish_diag(kStageNoBars, err);
        return err;
    }

    err = nv_probe(&G.hw, (int)get_prop32(&G.entry, "reg", 0),
                   get_prop32(&G.entry, "depth", 32), get_prop32(&G.entry, "linebytes", G.nativeW * 4));
    if (err) {
        publish_diag(kStageNotNV34, err);
        return err;
    }

    /* Začíname v režime, ktorý už beží (OF alebo predchodca) — init do HW nezapisuje. */
    bpp = G.hw.curBpp;
    G.hw.fbOffset = G.hw.nat.start;
    build_modes();
    G.curMode = gModes[0].id;
    G.curDepth = bpp_depth(bpp);
    G.prefMode = G.curMode;
    G.prefDepth = G.curDepth;
    G.irqEnabled = 1;

    gamma_linear();
    gamma_publish();
    if (bpp == 8)
        for (i = 0; i < 256; i++)
            G.clutR[i] = G.clutG[i] = G.clutB[i] = (UInt8)(255 - i);
    else
        clut_identity();

#if NV_FB_CACHE
    SetProcessorCacheMode(kCurrentAddressSpaceID, fb_base(), G.hw.vramSize - G.hw.fbOffset, NV_FB_CACHE);
    G.fbCacheMode = NV_FB_CACHE;
#endif

    publish_diag(kStageOK, noErr);
    return noErr;
}

static OSStatus driver_open(void)
{
    if (G.opened)
        return noErr;
    if (VSLNewInterruptService(&G.entry, kVBLInterruptServiceType, &G.vblService) != noErr)
        G.vblService = 0;
    G.timerRunning = 1;
    arm_timer();
    G.opened = 1;
    return noErr;
}

static OSStatus driver_close(void)
{
    AbsoluteTime remaining;

    if (!G.opened)
        return noErr;
    G.timerRunning = 0;
    if (G.timer)
        CancelTimer(G.timer, &remaining);
    if (G.vblService)
        VSLDisposeInterruptService(G.vblService);
    G.vblService = 0;
    G.opened = 0;
    return noErr;
}

static OSStatus driver_finalize(void)
{
    driver_close();
    if (G.fbCacheMode)
        SetProcessorCacheMode(kCurrentAddressSpaceID, fb_base(), G.hw.vramSize - G.hw.fbOffset,
                              kProcessorCacheModeDefault);
    G.fbCacheMode = 0;
    if (G.hw.mmio)
        nv_restore(&G.hw);
    if (G.entryValid)
        RegistryEntryIDDispose(&G.entry);
    G.entryValid = 0;
    return noErr;
}

/* Argumenty len pre csCode, ktoré driver sám obsluhuje a ktoré nesú pointer na záznam.
 * Iné volania (Power Manager, súkromné kódy) môžu mať v csParam hodnotu, nie adresu —
 * čítanie z nej pri štarte = výnimka a zaseknutie (4.0.1). */
static int trace_has_record(IOCommandCode code, SInt16 cs)
{
    if (code == kControlCommand)
        return cs == cscSetMode || cs == cscSwitchMode || cs == cscSetEntries || cs == cscDirectSetEntries ||
               cs == cscSavePreferredConfiguration || cs == cscGrayPage;
    if (code == kStatusCommand)
        return cs == cscGetMode || cs == cscGetCurMode || cs == cscGetModeTiming ||
               cs == cscGetNextResolution || cs == cscGetVideoParameters ||
               cs == cscGetPreferredConfiguration || cs == cscGetBaseAddr;
    return 0;
}

static TraceEntry *trace_begin(IOCommandCode code, IOCommandKind kind, void *contents)
{
    TraceEntry *t = &gTrace.e[gTrace.count % TRACE_SIZE];

    gTrace.magic = FOURCC('N', 'V', 'T', 'R');
    gTrace.size = TRACE_SIZE;
    gTrace.count++;
    t->what = (code << 24) | ((kind & 0xff) << 16);
    t->arg0 = t->arg1 = 0;
    if (code == kControlCommand || code == kStatusCommand) {
        SInt16 cs = ((CntrlParam *)contents)->csCode;
        t->what |= (UInt16)cs;
        if (trace_has_record(code, cs)) {
            UInt32 *p = CS_PARAM_PTR(contents);
            if (p && !((UInt32)p & 1)) {
                t->arg0 = p[0];
                t->arg1 = p[1];
            }
        }
    }
    return t;
}

static void trace_end(TraceEntry *t, OSStatus err)
{
    t->result = (UInt32)err;
    t->state = (UInt32)G.curDepth << 16 | (G.curMode & 0xff) << 8 | (G.hw.curBpp & 0xff);
    t->pitch = G.hw.curPitch;
}

OSStatus DoDriverIO(AddressSpaceID spaceID, IOCommandID cmdID, void *contents,
                    IOCommandCode code, IOCommandKind kind)
{
    OSStatus err;
    TraceEntry *t = trace_begin(code, kind, contents);
    (void)spaceID;

    switch (code) {
    case kInitializeCommand:
    case kReplaceCommand:
        err = driver_initialize((DriverInitInfo *)contents);
        trace_end(t, err);
        return err;
    case kFinalizeCommand:
    case kSupersededCommand:
        err = driver_finalize();
        trace_end(t, err);
        return err;
    case kOpenCommand:
        err = driver_open();
        break;
    case kCloseCommand:
        err = driver_close();
        break;
    case kControlCommand:
        err = driver_control(((CntrlParam *)contents)->csCode, CS_PARAM_PTR(contents));
        break;
    case kStatusCommand:
        err = driver_status(((CntrlParam *)contents)->csCode, CS_PARAM_PTR(contents));
        break;
    case kKillIOCommand:
        err = noErr;
        break;
    default:
        err = paramErr;
        break;
    }
    trace_end(t, err);
    if (!(kind & kImmediateIOCommandKind))
        err = IOCommandIsComplete(cmdID, err);
    return err;
}

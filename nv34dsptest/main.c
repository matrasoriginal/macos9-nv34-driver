/*
 * NV34 DSp Test — volá DrawSprocket ako Half-Life (SDL macosclassic): FindBestContext,
 * GetMonitorFrequency, FadeGammaOut/In, Reserve, SetState… Pred aj po každom kroku zapíše riadok
 * do „NV34 DSp Test.txt“ vedľa programu a vyprázdni disk — pri zamrznutí ukáže posledný krok.
 */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <Files.h>
#include <Multiverse.h>

typedef long Fixed_;
typedef struct DSpAttr {
    Fixed_ frequency;
    UInt32 displayWidth, displayHeight, reserved1, reserved2, colorNeeds;
    void *colorTable;
    UInt32 contextOptions, backBufferDepthMask, displayDepthMask, backBufferBestDepth, displayBestDepth,
           pageCount;
    char filler[3];
    unsigned char gameMustConfirmSwitch;
    UInt32 reserved3[4];
} DSpAttr;

typedef long (*F0)(void);
typedef long (*FFind)(DSpAttr *, void **);
typedef long (*FCtxAttr)(void *, DSpAttr *);
typedef long (*FCtx)(void *);
typedef long (*FState)(void *, UInt32);
typedef long (*FFade)(void *, void *);
typedef long (*FFreq)(void *, Fixed_ *);

static FILE *gOut;

static void out(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    if (gOut) {
        va_start(ap, fmt);
        vfprintf(gOut, fmt, ap);
        va_end(ap);
        fprintf(gOut, "\n");
        fflush(gOut);
        FlushVol(0, 0);
    }
}

static ConnectionID gConn;

static void *sym(const char *name)
{
    Str255 p;
    Ptr a = 0;
    SymClass c;
    p[0] = strlen(name);
    memcpy(p + 1, name, p[0]);
    if (FindSymbol(gConn, p, &a, &c) != 0) {
        out("chyba symbol %s", name);
        return 0;
    }
    return a;
}

static void wait_ticks(long n)
{
    long f;
    Delay(n, &f);
}

int main(void)
{
    Ptr mainAddr;
    Str255 err;
    OSErr e;
    long r;
    void *ctx = 0;
    DSpAttr want, got;
    Fixed_ freq = 0;
    F0 Startup, Shutdown;
    FFind FindBest;
    FCtxAttr Reserve, GetAttr;
    FCtx Release;
    FState SetState;
    FFade FadeOut, FadeIn;
    FFreq GetFreq;

    gOut = fopen("NV34 DSp Test.txt", "w");
    out("NV34 DSp Test 1");
    e = GetSharedLibrary("\pDrawSprocketLib", kPowerPCArch, kReferenceCFrag, &gConn, &mainAddr, err);
    out("GetSharedLibrary(DrawSprocketLib): %d", e);
    if (e)
        goto end;
    Startup = (F0)sym("DSpStartup");
    Shutdown = (F0)sym("DSpShutdown");
    FindBest = (FFind)sym("DSpFindBestContext");
    Reserve = (FCtxAttr)sym("DSpContext_Reserve");
    GetAttr = (FCtxAttr)sym("DSpContext_GetAttributes");
    Release = (FCtx)sym("DSpContext_Release");
    SetState = (FState)sym("DSpContext_SetState");
    FadeOut = (FFade)sym("DSpContext_FadeGammaOut");
    FadeIn = (FFade)sym("DSpContext_FadeGammaIn");
    GetFreq = (FFreq)sym("DSpContext_GetMonitorFrequency");
    if (!Startup || !FindBest || !Reserve || !SetState || !FadeOut || !FadeIn || !GetFreq)
        goto end;

    out("1 DSpStartup ...");
    r = Startup();
    out("  -> %ld", r);

    memset(&want, 0, sizeof(want));
    want.displayWidth = 640;
    want.displayHeight = 480;
    want.colorNeeds = 2;                    /* kDSpColorNeeds_Require */
    want.backBufferDepthMask = want.displayDepthMask = 16 | 32;
    want.backBufferBestDepth = want.displayBestDepth = 16;
    want.pageCount = 1;
    out("2 DSpFindBestContext 640x480 ...");
    r = FindBest(&want, &ctx);
    out("  -> %ld ctx %lx", r, (unsigned long)ctx);
    if (r || !ctx)
        goto shut;

    if (GetAttr) {
        memset(&got, 0, sizeof(got));
        r = GetAttr(ctx, &got);
        out("3 GetAttributes -> %ld: %lux%lu freq %lx depth %lu", r, got.displayWidth, got.displayHeight,
            (unsigned long)got.frequency, got.displayBestDepth);
    }
    out("4 GetMonitorFrequency ...");
    r = GetFreq(ctx, &freq);
    out("  -> %ld freq %lx (%ld Hz)", r, (unsigned long)freq, (long)(freq >> 16));

    out("5 FadeGammaOut(NULL) ...");
    r = FadeOut(0, 0);
    out("  -> %ld", r);
    out("6 FadeGammaIn(NULL) ...");
    r = FadeIn(0, 0);
    out("  -> %ld", r);

    out("7 Reserve ...");
    r = Reserve(ctx, &want);
    out("  -> %ld", r);
    if (r)
        goto shut;
    out("8 FadeGammaOut(ctx) ...");
    r = FadeOut(ctx, 0);
    out("  -> %ld", r);
    out("9 SetState(Active) ...");
    r = SetState(ctx, 0);
    out("  -> %ld", r);
    out("10 FadeGammaIn(ctx) ...");
    r = FadeIn(ctx, 0);
    out("  -> %ld", r);
    wait_ticks(120);
    out("11 FadeGammaOut(ctx) ...");
    r = FadeOut(ctx, 0);
    out("  -> %ld", r);
    out("12 SetState(Inactive) ...");
    r = SetState(ctx, 1);
    out("  -> %ld", r);
    out("13 FadeGammaIn(ctx) ...");
    r = FadeIn(ctx, 0);
    out("  -> %ld", r);
    out("14 Release ...");
    r = Release(ctx);
    out("  -> %ld", r);
shut:
    out("15 DSpShutdown ...");
    r = Shutdown ? Shutdown() : 0;
    out("  -> %ld", r);
end:
    out("Hotovo. Stlac Return.");
    if (gOut)
        fclose(gOut);
    getchar();
    return 0;
}

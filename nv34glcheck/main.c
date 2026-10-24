/*
 * NV34 GL Check — spustiť v priečinku hry (Quake 2). Zistí, odkiaľ CFM berie „OpenGLLibrary“
 * a prečo sa nenačíta ref_gl.lib. Výsledok na obrazovku aj do „NV34 GL Check.txt“ vedľa programu.
 */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <Files.h>
#include <Multiverse.h>

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
    }
}

static const char *pstr(const unsigned char *p)
{
    static char b[256];
    memcpy(b, p + 1, p[0]);
    b[p[0]] = 0;
    return b;
}

static const char *cfm_err(OSErr e)
{
    switch (e) {
    case 0: return "OK";
    case -2800: return "cfragContextIDErr";
    case -2801: return "cfragConnectionIDErr";
    case -2802: return "cfragNoSymbolErr";
    case -2803: return "cfragNoSectionErr";
    case -2804: return "cfragNoLibraryErr (kniznica nenajdena)";
    case -2805: return "cfragDupRegistrationErr";
    case -2806: return "cfragFragmentFormatErr";
    case -2807: return "cfragUnresolvedErr (chyba symbol)";
    case -2808: return "cfragNoPositionErr";
    case -2809: return "cfragNoPrivateMemErr";
    case -2810: return "cfragNoClientMemErr (malo pamate)";
    case -2811: return "cfragNoIDsErr";
    case -2812: return "cfragInitOrderErr";
    case -2813: return "cfragImportTooOldErr";
    case -2814: return "cfragImportTooNewErr";
    case -2815: return "cfragInitLoopErr";
    case -2816: return "cfragInitAtBootErr";
    case -2817: return "cfragLibConnErr";
    case -2818: return "cfragCFMStartupErr";
    case -2819: return "cfragCFMInternalErr";
    case -2820: return "cfragFragmentCorruptErr";
    case -2821: return "cfragInitFunctionErr (init rutina zlyhala)";
    case -2822: return "cfragNoApplicationErr";
    case -2823: return "cfragArchitectureErr";
    case -2824: return "cfragFragmentUsageErr";
    case -2825: return "cfragFileSizeErr";
    case -2826: return "cfragNotClosureErr";
    case -2827: return "cfragNoRegistrationErr";
    case -2828: return "cfragContainerIDErr";
    case -2829: return "cfragClosureIDErr";
    case -2830: return "cfragAbortClosureErr";
    case -2831: return "cfragOutputLengthErr";
    case -2832: return "cfragMapFileErr";
    case -2833: return "cfragExecFileRefErr";
    case -2834: return "cfragStdFolderErr";
    case -2835: return "cfragRsrcForkErr";
    case -2836: return "cfragCFragRsrcErr";
    case -43: return "fnfErr (subor nenajdeny)";
    }
    return "?";
}

typedef const unsigned char *(*GetStringFn)(unsigned int);

int main(void)
{
    ConnectionID conn = 0, c2 = 0;
    Ptr mainAddr;
    Str255 errName;
    OSErr e;
    FSSpec spec;

    gOut = fopen("NV34 GL Check.txt", "w");
    out("NV34 GL Check 1");

    errName[0] = 0;
    e = GetSharedLibrary("\pOpenGLLibrary", kPowerPCArch, kReferenceCFrag, &conn, &mainAddr, errName);
    out("1) GetSharedLibrary(OpenGLLibrary): %d %s  [%s]", e, cfm_err(e), pstr(errName));
    if (e == 0) {
        SymClass cls;
        Ptr sym = 0;
        e = FindSymbol(conn, "\pglGetString", &sym, &cls);
        out("   glGetString: %d", e);
        if (e == 0 && sym) {
            const unsigned char *v = ((GetStringFn)sym)(0x1F00);
            const unsigned char *r = ((GetStringFn)sym)(0x1F01);
            out("   vendor:   %s", v ? (const char *)v : "(null)");
            out("   renderer: %s", r ? (const char *)r : "(null)");
            out("   -> %s", r && strstr((const char *)r, "MiniGL") ? "NASA kniznica (MiniGL)" : "Apple OpenGL / ina");
        }
    }

    errName[0] = 0;
    e = FSMakeFSSpec(0, 0, "\pref_gl.lib", &spec);
    out("2) ref_gl.lib v priecinku programu: %d %s", e, cfm_err(e));
    if (e == 0) {
        errName[0] = 0;
        e = GetDiskFragment(&spec, 0, 0, "\pref_gl", kLoadNewCopy, &c2, &mainAddr, errName);
        out("   GetDiskFragment(ref_gl.lib): %d %s  [%s]", e, cfm_err(e), pstr(errName));
        if (e == 0) {
            SymClass cls;
            Ptr sym = 0;
            e = FindSymbol(c2, "\pGetRefAPI", &sym, &cls);
            out("   GetRefAPI: %d %s", e, cfm_err(e));
            CloseConnection(&c2);
        }
    }
    if (conn)
        CloseConnection(&conn);
    out("Hotovo. Vysledok je aj v subore NV34 GL Check.txt.");
    out("Stlac Return.");
    if (gOut)
        fclose(gOut);
    getchar();
    return 0;
}

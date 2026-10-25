/*
 * NV34 Info — vypíše z Name Registry všetko o NVIDIA uzloch (OS 9).
 *
 * Pre každý uzol, ktorého cesta obsahuje "NVDA", vypíše všetky properties.
 * Dekóduje: driver-descriptor (aký driver je naviazaný), NV34NDRV,diag,
 * assigned-addresses, AAPL,address, driver,AAPL,MacOS,PowerPC (vložený ROM driver).
 *
 * Výstup ide do okna aj do súboru "NV34 Info.txt" vedľa aplikácie.
 *
 * cpu-1: takt procesora — uzly cpus, gpio, mac-io, i2c/teplota, vlastné meranie taktu a hodnoty
 * GPIO registrov KeyLargo (len čítanie). PowerBook6,x prepína rýchlosť zbernicou 101/133 MHz cez
 * voltage-gpio, frequency-gpio a slewing-done (Linux cpufreq_32.c, gpios_set_cpu_speed).
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <MacTypes.h>
#include <Quickdraw.h>
#include <Events.h>
#include <OSUtils.h>
typedef SInt32 OSStatus;

typedef struct RegEntryID { UInt32 contents[4]; } RegEntryID;
typedef struct OpaqueRegEntryIter *RegEntryIter;
typedef struct OpaqueRegPropertyIter *RegPropertyIter;

enum {
    kRegIterContinue    = 1,
    kRegIterDescendants = 5
};

extern OSStatus RegistryEntryIterateCreate(RegEntryIter *cookie);
extern OSStatus RegistryEntryIterateDispose(RegEntryIter *cookie);
extern OSStatus RegistryEntryIterate(RegEntryIter *cookie, UInt32 op, RegEntryID *found, Boolean *done);
extern OSStatus RegistryEntryIDDispose(RegEntryID *id);
extern OSStatus RegistryEntryToPathSize(const RegEntryID *e, UInt32 *size);
extern OSStatus RegistryCStrEntryToPath(const RegEntryID *e, char *path, UInt32 size);
extern OSStatus RegistryPropertyIterateCreate(const RegEntryID *e, RegPropertyIter *cookie);
extern OSStatus RegistryPropertyIterateDispose(RegPropertyIter *cookie);
extern OSStatus RegistryPropertyIterate(RegPropertyIter *cookie, char *name, Boolean *done);
extern OSStatus RegistryPropertyGetSize(const RegEntryID *e, const char *name, UInt32 *size);
extern OSStatus RegistryPropertyGet(const RegEntryID *e, const char *name, void *value, UInt32 *size);

static FILE *gLog;

static void out(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (gLog) {
        va_start(ap, fmt);
        vfprintf(gLog, fmt, ap);
        va_end(ap);
    }
}

static unsigned char gBuf[65536];

static UInt32 be32(const unsigned char *p)
{
    return (UInt32)p[0] << 24 | (UInt32)p[1] << 16 | (UInt32)p[2] << 8 | p[3];
}

/* Reťazec = aspoň 2 tlačiteľné znaky, nuly iba ako terminátory na konci. */
static int printable(const unsigned char *p, UInt32 n)
{
    UInt32 i, len = n;
    while (len && p[len - 1] == 0)
        len--;
    if (len < 2)
        return 0;
    for (i = 0; i < len; i++)
        if (p[i] < 0x20 || p[i] > 0x7e)
            return 0;
    return 1;
}

static void dump_hex(const unsigned char *p, UInt32 n, UInt32 max)
{
    UInt32 i;
    for (i = 0; i < n && i < max; i++) {
        if (i && (i % 4) == 0)
            out(i % 32 ? " " : "\n        ");
        out("%02x", p[i]);
    }
    if (n > max)
        out(" ... (%lu B)", n);
    out("\n");
}

static void pstr(const unsigned char *p, char *dst)
{
    memcpy(dst, p + 1, p[0] < 31 ? p[0] : 31);
    dst[p[0] < 31 ? p[0] : 31] = 0;
}

/* DriverDescription: 'mtej', ver, Str31 deviceType, NumVersion, runtime, Str31 driverName, ... */
static void dump_driver_description(const unsigned char *d, UInt32 n)
{
    char type[32], name[32];
    if (n < 112 || memcmp(d, "mtej", 4) != 0) {
        out("      (nie je DriverDescription)\n");
        return;
    }
    pstr(d + 8, type);
    pstr(d + 48, name);
    out("      => deviceType \"%s\", driver \"%s\", verzia %x.%x.%x stage %02x rev %d, runtime 0x%lx\n",
        type, name, d[40], d[41] >> 4, d[41] & 15, d[42], d[43], be32(d + 44));
}

static int gBlobCount;

static void dump_pef_blob(const unsigned char *d, UInt32 n, const char *path)
{
    UInt32 i;
    char fname[64];
    const char *leaf = strrchr(path, ':');
    FILE *f;

    if (n < 8 || memcmp(d, "Joy!peff", 8) != 0)
        return;
    out("      => PEF kontajner (%lu B)", n);
    for (i = 0; i + 4 < n; i++)
        if (memcmp(d + i, "mtej", 4) == 0) {
            out(", obsahuje 'mtej' na +0x%lx", i);
            break;
        }
    /* uloží vložený driver (napr. ROM ndrv karty) — pre fázu 1 / reverse */
    sprintf(fname, "ndrv %d %.24s.pef", ++gBlobCount, leaf ? leaf + 1 : path);
    f = fopen(fname, "wb");
    if (f) {
        fwrite(d, 1, n, f);
        fclose(f);
        out("\n      => uložené do \"%s\"", fname);
    }
    out("\n");
}

static const char *csc_name(UInt32 cmd, UInt32 cs)
{
    static const char *ctl[] = { "Reset", "KillIO", "SetMode", "SetEntries", "SetGamma", "GrayPage",
                                 "SetGray", "SetInterrupt", "DirectSetEntries", "SetDefaultMode",
                                 "SwitchMode", "SetSync" };
    static const char *st[] = { "?0", "?1", "GetMode", "GetEntries", "GetPages", "GetBaseAddr",
                                "GetGray", "GetInterrupt", "GetGamma", "GetDefaultMode", "GetCurMode",
                                "GetSync", "GetConnection", "GetModeTiming", "GetModeBaseAddress",
                                "GetScanProc", "GetPreferredConfiguration", "GetNextResolution",
                                "GetVideoParameters", "?19", "GetGammaInfoList" };
    static const char *cmds[] = { "Open", "Close", "Read", "Write", "Control", "Status", "KillIO",
                                  "Initialize", "Finalize", "Replace", "Superseded" };
    if (cmd == 4) {
        if (cs < 12) return ctl[cs];
        if (cs == 16) return "SavePreferredConfiguration";
        if (cs == 22) return "SetHardwareCursor";
        if (cs == 23) return "DrawHardwareCursor";
        if (cs == 25) return "SetPowerState";
        { static char b[16]; sprintf(b, "Control %lu", cs); return b; }
    }
    if (cmd == 5) {
        if (cs <= 20) return st[cs];
        if (cs == 22) return "SupportsHardwareCursor";
        if (cs == 23) return "GetHardwareCursorDrawState";
        if (cs == 25) return "GetPowerState";
        if (cs == 27) return "GetDDCBlock";
        { static char b[16]; sprintf(b, "Status %lu", cs); return b; }
    }
    return cmd <= 10 ? cmds[cmd] : "?";
}

/* Záznam volaní drivera: adresa v diag d[29], štruktúra viď driver/ndrv.c gTrace. */
static void dump_trace(UInt32 addr)
{
    const UInt32 *t = (const UInt32 *)addr;
    UInt32 count, size, first, i;

    if (!addr || (addr & 3) || t[0] != 0x4E565452 /* 'NVTR' */) {
        out("      => trace: nenájdený na %08lx\n", addr);
        return;
    }
    count = t[1];
    size = t[2];
    if (size == 0 || size > 4096)
        size = 256;
    first = count > size ? count - size : 0;
    out("\n  trace drivera (%lu volaní, vypísaných %lu):\n", count, count - first);
    out("     #  príkaz                        arg0     arg1     výsledok  depth mode bpp pitch\n");
    for (i = first; i < count; i++) {
        const UInt32 *e = t + 3 + (i % size) * 6;
        UInt32 cmd = e[0] >> 24, cs = e[0] & 0xffff;
        out("  %4lu  %-28s  %08lx %08lx %6ld    %3lu   %lu  %2lu  %lu\n", i, csc_name(cmd, cs),
            e[1], e[2], (long)e[3], e[4] >> 16, (e[4] >> 8) & 0xff, e[4] & 0xff, e[5]);
    }
}

static UInt32 gTraceAddr;

/* Štatistiky NV34 QD Accel (štruktúra Stats v nv34qd/main.c). */
static void dump_qd_stats(const unsigned char *d, UInt32 n)
{
    static const char *names[] = { "calls", "accelerated", "rects", "pixels/1024", "notScreen",
                                   "notSrcCopy", "scaled", "colorized", "complexRgn", "busy",
                                   "timeouts", "reinits", "surfaceResets", "disabled",
                                   "stage", "myUPP", "oldTrap", "trapAfterSet", "trapNow",
                                   "heartbeat", "selfTestCalls" };
    const UInt32 *s;
    int i;

    if (n < 4)
        return;
    s = (const UInt32 *)be32(d);
    if (!s || ((UInt32)s & 3) || s[0] != 0x4E565153 /* 'NVQS' */) {
        out("      => štatistiky nenájdené (NV34 QD Accel nebeží?)\n");
        return;
    }
    out("      => NV34 QD Accel v%lu:", s[1]);
    for (i = 0; i < (s[1] >= 2 ? 21 : 14); i++) {
        if (i >= 15 && i <= 18)
            out("%s %s %08lx", i % 5 ? "," : "\n        ", names[i], s[2 + i]);
        else
            out("%s %s %lu", i % 5 ? "," : "\n        ", names[i], s[2 + i]);
    }
    if (s[1] >= 4) {
        static const char *cat[] = { "GPU (akcelerované)", "CPU offscreen -> obrazovka", "CPU z obrazovky (číta VRAM)",
                                     "CPU ostatné CopyBits", "CopyMask", "CopyDeepMask" };
        UInt32 calls[6], us[6];
        int k;
        calls[0] = s[3]; us[0] = s[23];
        calls[1] = s[24]; us[1] = s[25];
        calls[2] = s[26]; us[2] = s[27];
        calls[3] = s[28]; us[3] = s[29];
        calls[4] = s[30]; us[4] = s[31];
        calls[5] = s[32]; us[5] = s[33];
        out("\n         profil času:                     volaní     spolu ms   priemer µs\n");
        for (k = 0; k < 6; k++)
            out("         %-30s %8lu %12lu %12lu\n", cat[k], calls[k], us[k] / 1000, calls[k] ? us[k] / calls[k] : 0);
        out("         CopyMask na obrazovku %lu, CopyDeepMask na obrazovku %lu\n", s[34], s[35]);
    }
    if (s[1] >= 5) {
        int k;
        out("         ostatné podľa (zdroj VRAM, cieľ VRAM, srcCopy):\n");
        for (k = 0; k < 8; k++)
            if (s[36 + k])
                out("           src %s  dst %s  %-8s  %6lu volaní  %8lu ms\n", k & 4 ? "VRAM" : "RAM ",
                    k & 2 ? "VRAM" : "RAM ", k & 1 ? "srcCopy" : "iný mód", s[36 + k], s[44 + k] / 1000);
        out("         najdrahšie: %lu ms, mód %lu, src %08lx rb %lu, dst %08lx rb %lu, hĺbky %lu/%lu, %lu x %lu, maska %lu\n",
            s[52] / 1000, s[53], s[54], s[56], s[55], s[57], s[58] >> 16, s[58] & 0xffff,
            s[59] >> 16, s[59] & 0xffff, s[60]);
    }
    out("\n");
}

static void dump_diag(const unsigned char *d, UInt32 n)
{
    static const char *stages[] = { "OK", "chýba built-in", "nenašli sa BAR (assigned-addresses/AAPL,address)", "čip nie je NV34", "vypnutý cez boot-args nv34off" };
    UInt32 v[14];
    UInt32 i;
    if (n < 48)
        return;
    for (i = 0; i < 14; i++)
        v[i] = (i * 4 + 4 <= n) ? be32(d + i * 4) : 0;
    out("      => verzia %08lx  PMC_BOOT_0 %08lx  hlava %lu  BE regs %lu\n", v[0], v[1], v[2], v[3]);
    out("         MMIO %08lx  VRAM %08lx  fboffset %08lx  VRAM size %08lx\n", v[4], v[5], v[6], v[7]);
    out("         CR28 %02lx  CR46 %02lx  CR13 %02lx  GENCTRL %08lx  FP_TG %08lx  PCRTC_CONFIG %08lx\n",
        v[8] & 0xff, (v[8] >> 8) & 0xff, (v[8] >> 16) & 0xff, v[9], v[10], v[11]);
    if (n >= 56)
        out("         init: %s (err %ld)\n", v[12] < 5 ? stages[v[12]] : "?", (long)v[13]);
    if (n >= 64) {
        UInt32 x = be32(d + 56), r = be32(d + 60);
        out("         byteXor %lu (overené %lu)  keepEndian %lu\n", x & 0xff, (x >> 8) & 0xff, (x >> 16) & 0xff);
        out("         CR19 %02lx  CR42 %02lx  CR1B %02lx  CR20 %02lx\n", r & 0xff, (r >> 8) & 0xff, (r >> 16) & 0xff, r >> 24);
    }
    if (n >= 112) {
        int h;
        out("         CR44 (owner) %02lx\n", (be32(d + 84) >> 8) & 0xff);
        for (h = 0; h < 2; h++) {
            const unsigned char *q = d + 64 + h * 24;
            UInt32 cr = be32(q);
            UInt32 pitch = ((cr >> 8) & 0xff | ((cr >> 21) & 7) << 8 | ((cr >> 30) & 1) << 11) << 3;
            out("         hlava %d: skóre %lu  CR28 %02lx  pitch %lu  START %08lx  FP_TG %08lx  GENCTRL %08lx  CONFIG %08lx\n",
                h, be32(q + 20) & 0xff, cr & 0xff, pitch, be32(q + 4), be32(q + 8), be32(q + 12), be32(q + 16));
        }
    }
    /* trace len z inštancie, ktorá sa naviazala (Display-B s chybou initu už nemá pamäť) */
    if (n >= 120 && v[12] == 0)
        gTraceAddr = be32(d + 116);
}

static void dump_assigned(const unsigned char *d, UInt32 n)
{
    UInt32 i;
    for (i = 0; i + 20 <= n; i += 20)
        out("      => BAR reg 0x%02lx  phys %08lx  size %08lx  (hi %08lx)\n",
            be32(d + i) & 0xff, be32(d + i + 8), be32(d + i + 16), be32(d + i));
}

/* cpu-1: GPIO uzly (meno, reg) a logická adresa mac-io */
#define GPIO_MAX 64
static char gGpioName[GPIO_MAX][40];
static UInt32 gGpioReg[GPIO_MAX];
static int gGpioN;
static UInt32 gMacIO, gTbFreq;

static void note_entry(const RegEntryID *e, const char *path)
{
    const char *leaf = strrchr(path, ':');
    UInt32 v[4], size = sizeof(v);
    leaf = leaf ? leaf + 1 : path;
    if (!strncmp(leaf, "mac-io", 6) && !gMacIO && RegistryPropertyGet(e, "AAPL,address", v, &size) == 0)
        gMacIO = v[0];
    size = sizeof(v);
    if (strstr(path, ":cpus:") && !gTbFreq && RegistryPropertyGet(e, "timebase-frequency", v, &size) == 0)
        gTbFreq = v[0];
    size = sizeof(v);
    if (strstr(path, "gpio") && strncmp(leaf, "gpio", 4) && gGpioN < GPIO_MAX &&
        RegistryPropertyGet(e, "reg", v, &size) == 0 && size >= 4) {
        strncpy(gGpioName[gGpioN], leaf, sizeof(gGpioName[0]) - 1);
        gGpioReg[gGpioN++] = v[0];
    }
}

static UInt32 tb(void)
{
    UInt32 v;
    __asm__ volatile("mftb %0" : "=r"(v));
    return v;
}

/* rozdiel slučiek s 12 a 4 závislými sčítaniami = 8 taktov na iteráciu (ako MiniGL q3-14) */
static UInt32 loop_tb(int twelve)
{
    UInt32 t0, t1, x = 1;
    long n = 1000000;
    t0 = tb();
    if (twelve)
        __asm__ volatile("mtctr %1\n"
                         "1: add %0,%0,%0\n add %0,%0,%0\n add %0,%0,%0\n add %0,%0,%0\n"
                         " add %0,%0,%0\n add %0,%0,%0\n add %0,%0,%0\n add %0,%0,%0\n"
                         " add %0,%0,%0\n add %0,%0,%0\n add %0,%0,%0\n add %0,%0,%0\n bdnz 1b"
                         : "+r"(x) : "r"(n) : "ctr");
    else
        __asm__ volatile("mtctr %1\n"
                         "1: add %0,%0,%0\n add %0,%0,%0\n add %0,%0,%0\n add %0,%0,%0\n bdnz 1b"
                         : "+r"(x) : "r"(n) : "ctr");
    t1 = tb();
    return t1 - t0;
}

static void cpu_report(void)
{
    int i, k;
    UInt32 tbf = gTbFreq ? gTbFreq : 18432000;
    out("\n== cpu-1: takt procesora\n");
    for (k = 0; k < 3; k++) {
        UInt32 a = loop_tb(0), b = loop_tb(1);
        out("  meranie %d: %lu MHz (timebase %lu Hz)\n", k + 1,
            b > a ? (UInt32)(8.0 * tbf / (b - a)) : 0, tbf);
    }
    out("  mac-io AAPL,address %08lx, GPIO uzlov %d\n", gMacIO, gGpioN);
    if (!gMacIO)
        return;
    /* reg je posun voči 0x50 alebo voči mac-io (Linux read_gpio) */
    for (i = 0; i < gGpioN; i++) {
        UInt32 off = gGpioReg[i] < 0x50 ? gGpioReg[i] + 0x50 : gGpioReg[i];
        if (off < 0x50 || off > 0x7f) {
            out("  %-28s reg %lx (mimo 0x50-0x7f, necitam)\n", gGpioName[i], gGpioReg[i]);
            continue;
        }
        out("  %-28s reg %lx -> mac-io+%02lx = %02x\n", gGpioName[i], gGpioReg[i], off,
            *(volatile unsigned char *)(gMacIO + off));
    }
    out("  GPIO 0x50-0x7f:");
    for (i = 0x50; i < 0x80; i++)
        out("%s%02x", (i & 15) ? " " : "\n    ", *(volatile unsigned char *)(gMacIO + i));
    out("\n");
}

static int cpu_path(const char *path)
{
    const char *leaf = strrchr(path, ':');
    leaf = leaf ? leaf + 1 : path;
    return strstr(path, ":cpus") || strstr(path, "gpio") || !strncmp(leaf, "mac-io", 6) || strstr(path, "i2c") ||
           strstr(path, "pmu") || strstr(path, "fan") || strstr(path, "therm") || strstr(path, "temp") ||
           strstr(path, "power-mgt");
}

static void dump_entry(const RegEntryID *e, const char *path)
{
    RegPropertyIter it;
    Boolean done = 0;
    char name[64];

    out("\n== %s\n", path);
    if (RegistryPropertyIterateCreate(e, &it) != 0)
        return;
    for (;;) {
        UInt32 size = 0;
        if (RegistryPropertyIterate(&it, name, &done) != 0 || done)
            break;
        if (RegistryPropertyGetSize(e, name, &size) != 0) {
            out("  %-32s (?)\n", name);
            continue;
        }
        out("  %-32s [%4lu] ", name, size);
        if (size > sizeof(gBuf)) {
            out("(príliš veľké)\n");
            continue;
        }
        if (RegistryPropertyGet(e, name, gBuf, &size) != 0) {
            out("(chyba čítania)\n");
            continue;
        }
        if (size >= 8 && memcmp(gBuf, "Joy!peff", 8) == 0)
            out("\n");
        else if (printable(gBuf, size))
            out("\"%.*s\"\n", (int)size, gBuf);
        else
            dump_hex(gBuf, size, 64);

        if (!strcmp(name, "driver-descriptor"))
            dump_driver_description(gBuf, size);
        else if (!strcmp(name, "NV34ACCEL,stats"))
            dump_qd_stats(gBuf, size);
        else if (!strcmp(name, "NV34NDRV,diag"))
            dump_diag(gBuf, size);
        else if (!strcmp(name, "assigned-addresses"))
            dump_assigned(gBuf, size);
        else if (!strncmp(name, "driver,", 7))
            dump_pef_blob(gBuf, size, path);
    }
    RegistryPropertyIterateDispose(&it);
}

int main(void)
{
    RegEntryIter it;
    RegEntryID e;
    Boolean done = 0;
    UInt32 op = kRegIterDescendants;
    int total = 0, shown = 0;
    static char path[1024];

    /* pripisuje (beh zo Startup Items pri rozbitom obraze sa neprepíše ďalším behom) */
    gLog = fopen("NV34 Info.txt", "a");
    {
        unsigned long secs;
        GetDateTime(&secs);
        out("\n==================== NV34 Info 10 — %lu s od 1904, %lu s od štartu\n",
            secs, TickCount() / 60);
    }

    if (RegistryEntryIterateCreate(&it) != 0) {
        out("RegistryEntryIterateCreate zlyhal\n");
        goto end;
    }
    for (;;) {
        UInt32 size;
        if (RegistryEntryIterate(&it, op, &e, &done) != 0 || done)
            break;
        op = kRegIterContinue;
        total++;
        if (RegistryEntryToPathSize(&e, &size) == 0 && size < sizeof(path) &&
            RegistryCStrEntryToPath(&e, path, size) == 0) {
            note_entry(&e, path);
            if (strstr(path, "NVDA") || strstr(path, "ATY") || strstr(path, "display") || cpu_path(path)) {
                dump_entry(&e, path);
                shown++;
            }
        }
        RegistryEntryIDDispose(&e);
    }
    RegistryEntryIterateDispose(&it);
    out("\n%d uzlov v registri, vypísaných %d\n", total, shown);

    /* pohľad QuickDraw na hlavnú obrazovku — porovnať s HW v diag/trace */
    {
        PixMapHandle pm = (**GetMainDevice()).gdPMap;
        out("\nQuickDraw hlavná obrazovka: %d bpp, rowBytes %d, baseAddr %08lx, %d x %d\n",
            (**pm).pixelSize, (**pm).rowBytes & 0x3fff, (UInt32)(**pm).baseAddr,
            (**pm).bounds.right - (**pm).bounds.left, (**pm).bounds.bottom - (**pm).bounds.top);
    }
    dump_trace(gTraceAddr);
    cpu_report();

end:
    if (gLog) {
        fclose(gLog);
        printf("Uložené do \"NV34 Info.txt\" vedľa aplikácie.\n");
    }
    /* klik ukončí; bez kliknutia koniec po 20 s (beh zo Startup Items) */
    printf("(klik ukončí, inak sám o 20 s)\n");
    {
        UInt32 t0 = TickCount();
        while (!Button() && TickCount() - t0 < 20 * 60)
            ;
    }
    return 0;
}

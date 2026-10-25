/*
 * NV34 CPU — prepne PowerBook6,2 pod Mac OS 9 na plný takt (1 GHz).
 *
 * cpu-1: firmware nechá zbernicu na 102 MHz (bus-frequencies 133,12 / 102 MHz, procesor ×7,5) →
 * procesor ~765 MHz. Mac OS X ju prepne, Mac OS 9 na tomto modeli o tom nevie. Prepnutie je ako
 * v Linuxe (cpufreq_32.c, gpios_set_cpu_speed) cez GPIO KeyLargo:
 *   hore:  voltage-gpio = 0x05, 10 ms; frequency-gpio = 0x05, 200 µs; čakať na slewing-done (bit 0x02)
 *   dole:  frequency-gpio = 0x04, čakať na slewing-done; voltage-gpio = 0x04, 10 ms
 * GPIO bajt: 0x04 = výstup zapnutý, 0x01 = výstupná hodnota, 0x02 = vstupná úroveň.
 *
 * Spustenie prepne na plný takt, s podržaným Option späť na znížený. Log "NV34 CPU.txt" vedľa
 * aplikácie (pripisuje). Do Startup Items sa dá dať na prepnutie pri každom štarte.
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <MacTypes.h>
#include <Events.h>
#include <OSUtils.h>
typedef SInt32 OSStatus;

typedef struct RegEntryID { UInt32 contents[4]; } RegEntryID;
extern OSStatus RegistryCStrEntryLookup(const RegEntryID *start, const char *path, RegEntryID *found);
extern OSStatus RegistryEntryIDDispose(RegEntryID *id);
extern OSStatus RegistryPropertyGet(const RegEntryID *e, const char *name, void *value, UInt32 *size);

#define DT "Devices:device-tree:"

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
        fflush(gLog);                               /* log musí prežiť zamrznutie pri prepnutí */
    }
}

/* vlastnosť uzla (prvé slová) */
static int prop(const char *path, const char *name, UInt32 *v, UInt32 n)
{
    RegEntryID e;
    UInt32 size = n * 4;
    int ok;
    memset(v, 0, size);
    if (RegistryCStrEntryLookup(0, path, &e) != 0)
        return 0;
    ok = RegistryPropertyGet(&e, name, v, &size) == 0;
    RegistryEntryIDDispose(&e);
    return ok;
}

static UInt32 tb(void)
{
    UInt32 v;
    __asm__ volatile("mftb %0" : "=r"(v));
    return v;
}

static UInt32 gTbFreq = 18432000;

static void delay_us(UInt32 us)
{
    UInt32 t0 = tb(), n = (UInt32)((double)gTbFreq * us / 1000000.0);
    while (tb() - t0 < n)
        ;
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

static UInt32 cpu_mhz(void)
{
    UInt32 a = loop_tb(0), b = loop_tb(1);
    return b > a ? (UInt32)(8.0 * gTbFreq / (b - a)) : 0;
}

static volatile UInt8 *gFreq, *gVolt, *gSlew;

static void gpio_w(volatile UInt8 *p, UInt8 v)
{
    *p = v;
    __asm__ volatile("eieio\n sync" ::: "memory");
}

static void state(const char *when)
{
    out("%s: %lu MHz, frequency-gpio %02x, voltage-gpio %02x, slewing-done %02x\n", when, cpu_mhz(), *gFreq,
        *gVolt, *gSlew);
}

/* čaká na slewing-done (bit 0x02), max. 100 ms; vráti µs alebo 0 pri timeoute */
static UInt32 wait_slew(void)
{
    UInt32 t0 = tb(), us;
    for (;;) {
        us = (UInt32)((double)(tb() - t0) * 1000000.0 / gTbFreq);
        if (*gSlew & 0x02)
            return us ? us : 1;
        if (us > 100000)
            return 0;
        delay_us(100);
    }
}

/* GPIO uzol → adresa (reg je posun voči 0x50, Linux read_gpio) */
static volatile UInt8 *gpio_addr(UInt32 macio, const char *name, UInt32 expect)
{
    char path[96];
    UInt32 reg, off;
    sprintf(path, DT "pci:mac-io:gpio:%s", name);
    if (!prop(path, "reg", &reg, 1)) {
        out("uzol %s nenajdeny\n", path);
        return 0;
    }
    off = reg < 0x50 ? reg + 0x50 : reg;
    if (off != expect) {
        out("%s: reg %lx -> +%02lx, ocakavane +%02lx — nemenim nic\n", name, reg, off, expect);
        return 0;
    }
    return (volatile UInt8 *)(macio + off);
}

static int run(void)
{
    UInt32 macio, bus[2], ratio, model[8];
    EventRecord ev;
    int low;

    prop(DT "cpus:PowerPC,G4", "timebase-frequency", &gTbFreq, 1);
    if (!gTbFreq)
        gTbFreq = 18432000;
    prop("Devices:device-tree", "model", model, 8);
    ((char *)model)[31] = 0;
    out("model \"%s\"\n", (char *)model);
    if (!prop(DT "cpus:PowerPC,G4", "bus-frequencies", bus, 2) ||
        !prop(DT "cpus:PowerPC,G4", "processor-to-bus-ratio*2", &ratio, 1) || !ratio) {
        out("procesor nema bus-frequencies / processor-to-bus-ratio*2 — prepinanie zbernicou nie je\n");
        return 0;
    }
    out("zbernica %lu / %lu Hz, nasobok %lu/2 -> %lu / %lu MHz\n", bus[0], bus[1], ratio,
        (UInt32)((double)bus[0] * ratio / 2e6), (UInt32)((double)bus[1] * ratio / 2e6));
    if (!prop(DT "pci:mac-io", "AAPL,address", &macio, 1) || !macio) {
        out("mac-io AAPL,address nenajdene\n");
        return 0;
    }
    gFreq = gpio_addr(macio, "frequency-gpio", 0x6a);
    gVolt = gpio_addr(macio, "voltage-gpio", 0x6b);
    gSlew = gpio_addr(macio, "slewing-done", 0x62);
    if (!gFreq || !gVolt || !gSlew)
        return 0;

    EventAvail(0, &ev);
    low = (ev.modifiers & 0x0800) != 0;             /* optionKey */
    state("pred");
    if (!low) {
        UInt32 us;
        if (*gFreq & 0x01) {
            out("uz na plnom takte\n");
            return 1;
        }
        gpio_w(gVolt, 0x05);                        /* najprv vyššie napätie */
        delay_us(10000);
        gpio_w(gFreq, 0x05);
        delay_us(200);
        us = wait_slew();
        if (us)
            out("plny takt: slewing-done po %lu us\n", us);
        else
            out("plny takt: slewing-done do 100 ms neprislo\n");
    } else {
        UInt32 us;
        if (!(*gFreq & 0x01) && !(*gVolt & 0x01)) {
            out("uz na znizenom takte\n");
            return 1;
        }
        gpio_w(gFreq, 0x04);
        delay_us(200);
        us = wait_slew();
        out("znizeny takt: slewing-done %s %lu us\n", us ? "po" : "neprislo,", us);
        gpio_w(gVolt, 0x04);                        /* napätie až po znížení frekvencie */
        delay_us(10000);
    }
    state("po");
    return 1;
}

int main(void)
{
    unsigned long secs;
    gLog = fopen("NV34 CPU.txt", "a");
    GetDateTime(&secs);
    out("\n==== NV34 CPU 1 — %lu s od 1904, %lu s od startu (Option = znizeny takt)\n", secs,
        TickCount() / 60);
    run();
    if (gLog)
        fclose(gLog);
    printf("(klik ukonci, inak sam o 10 s)\n");
    {
        UInt32 t0 = TickCount();
        while (!Button() && TickCount() - t0 < 10 * 60)
            ;
    }
    return 0;
}

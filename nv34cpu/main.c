/*
 * NV34 CPU — switches the PowerBook6,2 under Mac OS 9 to its full clock (1 GHz).
 *
 * cpu-1: the firmware leaves the bus at 102 MHz (bus-frequencies 133.12 / 102 MHz, CPU ×7.5) →
 * CPU ~765 MHz. Mac OS X switches it, Mac OS 9 on this model does not know about it. The switch works as
 * in Linux (cpufreq_32.c, gpios_set_cpu_speed) via KeyLargo GPIO:
 *   up:    voltage-gpio = 0x05, 10 ms; frequency-gpio = 0x05, 200 µs; wait for slewing-done (bit 0x02)
 *   down:  frequency-gpio = 0x04, wait for slewing-done; voltage-gpio = 0x04, 10 ms
 * GPIO byte: 0x04 = output enabled, 0x01 = output value, 0x02 = input level.
 *
 * Running it switches to the full clock, with Option held down back to the reduced one. Log "NV34 CPU.txt" next to
 * the application (appended). Can be put into Startup Items to switch on every boot.
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <MacTypes.h>
#include <Events.h>
#include <OSUtils.h>
#include <MacMemory.h>
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
        fflush(gLog);                               /* the log must survive a freeze during the switch */
    }
}

/* node property (first words) */
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

/* difference of loops with 12 and 4 dependent adds = 8 cycles per iteration (like MiniGL q3-14) */
static UInt32 loop_tb_n(int twelve, long n)
{
    UInt32 t0, t1, x = 1;
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

static UInt32 loop_tb(int twelve)
{
    return loop_tb_n(twelve, 1000000);
}

static UInt32 cpu_mhz(void)
{
    UInt32 a = loop_tb(0), b = loop_tb(1);
    return b > a ? (UInt32)(8.0 * gTbFreq / (b - a)) : 0;
}

/* cpu-4: fastest of 300 short loops (~50 µs) — without interrupts; the difference to cpu_mhz =
 * time taken by interrupts */
static UInt32 cpu_mhz_min(void)
{
    UInt32 a = 0xffffffff, b = 0xffffffff, t;
    int i;
    for (i = 0; i < 300; i++) {
        if ((t = loop_tb_n(0, 5000)) < a)
            a = t;
        if ((t = loop_tb_n(1, 5000)) < b)
            b = t;
    }
    return b > a ? (UInt32)(8.0 * 5000 * gTbFreq / 1e6 / (b - a)) : 0;
}

static volatile UInt8 *gFreq, *gVolt, *gSlew;

static void gpio_w(volatile UInt8 *p, UInt8 v)
{
    *p = v;
    __asm__ volatile("eieio\n sync" ::: "memory");
}

/* cpu-3: timebase against TickCount (VIA, independent of the bus) over 1 s → real tb/s */
static UInt32 tb_per_sec(void)
{
    UInt32 t, t0, b0;
    t = TickCount();
    while (TickCount() == t)
        ;
    t0 = TickCount();
    b0 = tb();
    while (TickCount() - t0 < 60)
        ;
    return (UInt32)((double)(tb() - b0) * 60.15 / 60.0);   /* tick = 1/60.15 s */
}

/* reads 4 MB from RAM (bus + memory), MB/s based on the real tb/s */
static UInt32 *gMem;
#define MEM_WORDS (1u << 19)                       /* 2 MB (L2 512 KB) */

static UInt32 mem_mbs(UInt32 tbs)
{
    UInt32 i, t0, sum = 0, k;
    volatile UInt32 sink;
    if (!gMem)
        return 0;
    t0 = tb();
    for (k = 0; k < 4; k++)
        for (i = 0; i < MEM_WORDS; i += 8)
            sum += gMem[i];                         /* one word per 32 B cache line */
    /* 4 passes × 2 MB */
    sink = sum;
    (void)sink;
    return (UInt32)(4.0 * 2.0 * tbs / (double)(tb() - t0));
}

static void state(const char *when)
{
    UInt32 tbs = tb_per_sec(), mhz = cpu_mhz(), mn = cpu_mhz_min();
    out("%s: %lu MHz (dlha slucka), %lu MHz (bez preruseni, prerusenia %lu %%), timebase %lu Hz, "
        "citanie RAM %lu MB/s; frequency-gpio %02x, voltage-gpio %02x, slewing-done %02x\n", when, mhz, mn,
        mn > mhz ? (mn - mhz) * 100 / mn : 0, tbs, mem_mbs(tbs), *gFreq, *gVolt, *gSlew);
}

/* waits for slewing-done (bit 0x02), max. 100 ms; returns µs or 0 on timeout */
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

/* GPIO node → address (reg is an offset from 0x50, Linux read_gpio) */
static volatile UInt8 *gpio_addr(UInt32 macio, const char *name, UInt32 expect)
{
    char path[96];
    UInt32 reg, off;
    sprintf(path, DT "pci:mac-io:gpio:%s", name);
    if (!prop(path, "reg", &reg, 1)) {
        out("node %s not found\n", path);
        return 0;
    }
    off = reg < 0x50 ? reg + 0x50 : reg;
    if (off != expect) {
        out("%s: reg %lx -> +%02lx, expected +%02lx — changing nothing\n", name, reg, off, expect);
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
        out("CPU has no bus-frequencies / processor-to-bus-ratio*2 — no bus switching\n");
        return 0;
    }
    out("bus %lu / %lu Hz, multiplier %lu/2 -> %lu / %lu MHz\n", bus[0], bus[1], ratio,
        (UInt32)((double)bus[0] * ratio / 2e6), (UInt32)((double)bus[1] * ratio / 2e6));
    if (!prop(DT "pci:mac-io", "AAPL,address", &macio, 1) || !macio) {
        out("mac-io AAPL,address not found\n");
        return 0;
    }
    gFreq = gpio_addr(macio, "frequency-gpio", 0x6a);
    gVolt = gpio_addr(macio, "voltage-gpio", 0x6b);
    gSlew = gpio_addr(macio, "slewing-done", 0x62);
    if (!gFreq || !gVolt || !gSlew)
        return 0;

    {
        OSErr err;
        Handle h = TempNewHandle(MEM_WORDS * 4, &err);   /* the application heap is small (cpu-3: 0 MB/s) */
        if (h) {
            HLock(h);
            gMem = (UInt32 *)*h;
        } else {
            gMem = (UInt32 *)NewPtr(MEM_WORDS * 4);
        }
        out("memory for the RAM test: %s\n", gMem ? "ok" : "none");
    }
    EventAvail(0, &ev);
    low = (ev.modifiers & 0x0800) != 0;             /* optionKey */
    state("before");
    if (!low) {
        UInt32 us;
        if (*gFreq & 0x01) {
            out("already at full clock\n");
            state("+2 s");
            return 1;
        }
        gpio_w(gVolt, 0x05);                        /* raise the voltage first */
        delay_us(10000);
        gpio_w(gFreq, 0x05);
        delay_us(200);
        us = wait_slew();
        if (us)
            out("full clock: slewing-done after %lu us\n", us);
        else
            out("full clock: slewing-done did not arrive within 100 ms\n");
    } else {
        UInt32 us;
        if (!(*gFreq & 0x01) && !(*gVolt & 0x01)) {
            out("already at reduced clock\n");
            return 1;
        }
        gpio_w(gFreq, 0x04);
        delay_us(200);
        us = wait_slew();
        out("reduced clock: slewing-done %s %lu us\n", us ? "after" : "did not arrive,", us);
        gpio_w(gVolt, 0x04);                        /* voltage only after lowering the frequency */
        delay_us(10000);
    }
    state("after");
    state("+2 s");
    state("+4 s");
    return 1;
}

int main(void)
{
    unsigned long secs;
    gLog = fopen("NV34 CPU.txt", "a");
    GetDateTime(&secs);
    out("\n==== NV34 CPU 4 — %lu s od 1904, %lu s od startu (Option = znizeny takt)\n", secs,
        TickCount() / 60);
    run();
    if (gLog)
        fclose(gLog);
    printf("(click to quit, otherwise quits by itself in 10 s)\n");
    {
        UInt32 t0 = TickCount();
        while (!Button() && TickCount() - t0 < 10 * 60)
            ;
    }
    return 0;
}

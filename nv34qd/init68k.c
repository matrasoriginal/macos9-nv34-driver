/*
 * NV34 QD Accel — 68k INIT stub.
 *
 * Start Manager spúšťa iba 68k kód INIT resource. Stub načíta PPC fragment z resource
 * 'nvqd' 128 (sysHeap, locked) cez Code Fragment Manager a zavolá jeho export
 * nv34qd_install() cez routine descriptor (Mixed Mode: 68k → PPC).
 * Všetko beží počas INIT fázy, takže patch trapu je globálny pre všetky procesy.
 */
#include <Multiverse.h>
#include "Retro68Runtime.h"

#ifndef kPrivateCFragCopy
#define kPrivateCFragCopy 5
#endif

/* long f(void), C konvencia: kCStackBased | RESULT_SIZE(kFourByteCode) */
#define kInstallProcInfo 0x00000031

void _start(void)
{
    Handle h;
    ConnectionID conn;
    Ptr mainAddr, sym;
    SymClass cls;
    Str255 errName;
    UniversalProcPtr upp;
    THz oldZone;

    RETRO68_RELOCATE();

    h = Get1Resource('nvqd', 128);
    if (!h)
        return;
    DetachResource(h);
    HLock(h);

    oldZone = GetZone();
    SetZone(LMGetSysZone());     /* SystemZone() nemá 68k inline */
    if (GetMemFragment(*h, GetHandleSize(h), "\pNV34QD", kPrivateCFragCopy, &conn, &mainAddr, errName) == noErr &&
        FindSymbol(conn, "\pnv34qd_install", &sym, &cls) == noErr) {
        /* routine descriptor cez Mixed Mode Manager (68k → PPC TVector) */
        upp = NewRoutineDescriptor((ProcPtr)sym, kInstallProcInfo, kPowerPCISA);
        if (upp)
            ((long (*)(void))upp)();
    }
    SetZone(oldZone);
    /* fragment ani handle neuvoľňujeme — patch v nich žije do vypnutia */
}

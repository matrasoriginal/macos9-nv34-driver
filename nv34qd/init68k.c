/*
 * NV34 QD Accel — 68k INIT stub.
 *
 * The Start Manager only runs the 68k code of an INIT resource. The stub loads the PPC fragment from resource
 * 'nvqd' 128 (sysHeap, locked) via the Code Fragment Manager and calls its export
 * nv34qd_install() through a routine descriptor (Mixed Mode: 68k → PPC).
 * Everything runs during the INIT phase, so the trap patch is global for all processes.
 */
#include <Multiverse.h>
#include "Retro68Runtime.h"

#ifndef kPrivateCFragCopy
#define kPrivateCFragCopy 5
#endif

/* long f(void), C convention: kCStackBased | RESULT_SIZE(kFourByteCode) */
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
    SetZone(LMGetSysZone());     /* SystemZone() has no 68k inline */
    if (GetMemFragment(*h, GetHandleSize(h), "\pNV34QD", kPrivateCFragCopy, &conn, &mainAddr, errName) == noErr &&
        FindSymbol(conn, "\pnv34qd_install", &sym, &cls) == noErr) {
        /* routine descriptor via the Mixed Mode Manager (68k → PPC TVector) */
        upp = NewRoutineDescriptor((ProcPtr)sym, kInstallProcInfo, kPowerPCISA);
        if (upp)
            ((long (*)(void))upp)();
    }
    SetZone(oldZone);
    /* neither the fragment nor the handle is released — the patch lives in them until shutdown */
}

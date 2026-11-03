/*
 * Shared GPU state between NV34 QD Accel (the INIT, owner of the PFIFO channel) and other clients
 * (NV34 RAVE engine, tests). The INIT publishes the struct's address in the Name Registry as the property
 * "NV34ACCEL,shared" on NVDA,Display-A. All clients use the same push buffer —
 * calls are cooperative (no preemption), so it is enough for each client to wait for the GPU
 * (nva_sync) before CPU access to VRAM and to set up its own 3D state.
 *
 * Global things (VRAM allocator, log, context ids) live here because every process gets its own
 * copy of a shared library's data (RAVE engine).
 */
#ifndef NV34_SHARED_H
#define NV34_SHARED_H

#include "nv34_accel.h"
#include "nv34_3d.h"

#define NV34_SHARED_MAGIC   0x4E563353      /* 'NV3S' */
#define NV34_SHARED_VERSION 2

#define NV34_LOG_SIZE       32768

typedef struct NV34Shared {
    NVU32       magic, version;
    NVAccel    *a;
    NV3D       *g;
    /* Prepares the 3D engine (once). 0 = OK. */
    long      (*ensure3D)(void);
    /* Rebinds the 2D objects and sets the QD Accel 2D surface. */
    void      (*restore2D)(void);
    volatile int *busy;                     /* ≠ 0: QD Accel is drawing right now */
    NVU32       state3D;                    /* 0 nothing, 1 ready, 2 error */
    NVU32       generation;                 /* +1 on every 3D (re)initialization */

    /* VRAM for 3D (back buffers, z-buffers, textures): offset in VRAM, 0 = no room */
    NVU32     (*vramAlloc)(NVU32 size, NVU32 align);
    void      (*vramFree)(NVU32 offset);
    NVU32     (*vramFreeBytes)(void);

    NVU32       nextCtxId;                  /* context id allocation across processes */
    NVU32       boundCtxId;                 /* context whose state is currently in the GPU */

    /* text log of the clients (RAVE engine): ring buffer, logPos = total bytes written */
    char       *log;
    NVU32       logSize, logPos;
} NV34Shared;

#endif

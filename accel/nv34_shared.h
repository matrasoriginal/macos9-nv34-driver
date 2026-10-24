/*
 * Zdieľaný stav GPU medzi NV34 QD Accel (INIT, vlastník PFIFO kanála) a ďalšími klientmi
 * (NV34 RAVE engine, testy). INIT zverejní adresu štruktúry v Name Registry ako property
 * "NV34ACCEL,shared" na NVDA,Display-A. Všetci klienti používajú ten istý push buffer —
 * volania sú kooperatívne (bez prerušení), takže stačí, aby každý pred CPU prístupom do VRAM
 * počkal na GPU (nva_sync) a 3D stav si nastavil sám.
 *
 * Globálne veci (alokátor VRAM, log, id kontextov) sú tu, lebo dáta zdieľanej knižnice
 * (RAVE engine) má každý proces vlastné.
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
    /* Pripraví 3D engine (raz). 0 = OK. */
    long      (*ensure3D)(void);
    /* Znova naviaže 2D objekty a nastaví 2D povrch QD Accel. */
    void      (*restore2D)(void);
    volatile int *busy;                     /* ≠ 0: QD Accel práve kreslí */
    NVU32       state3D;                    /* 0 nič, 1 pripravený, 2 chyba */
    NVU32       generation;                 /* +1 pri každej (re)inicializácii 3D */

    /* VRAM pre 3D (zadné buffre, z-buffre, textúry): offset vo VRAM, 0 = nie je miesto */
    NVU32     (*vramAlloc)(NVU32 size, NVU32 align);
    void      (*vramFree)(NVU32 offset);
    NVU32     (*vramFreeBytes)(void);

    NVU32       nextCtxId;                  /* pridelenie id kontextov naprieč procesmi */
    NVU32       boundCtxId;                 /* kontext, ktorého stav je práve v GPU */

    /* textový log klientov (RAVE engine): kruhový, logPos = celkový počet zapísaných bajtov */
    char       *log;
    NVU32       logSize, logPos;
} NV34Shared;

#endif

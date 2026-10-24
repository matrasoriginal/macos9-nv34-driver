/*
 * NV34 (GeForce FX Go5200) display engine — iba to, čo NDRV potrebuje.
 * Registre a bity podľa nouveau drivers/gpu/drm/nouveau/dispnv04 + nvreg.h
 * a xf86-video-nv (nv_hw.c). NV04–NV36 zdieľajú rovnaký CRTC/RAMDAC/FP blok.
 */
#ifndef NV34_H
#define NV34_H

#include "ndrv_api.h"

/* ---- MMIO (BAR0) ---------------------------------------------------- */

#define NV_PMC_BOOT_0               0x000000
#define NV_PMC_BOOT_1               0x000004    /* 0 = LE, 0x01000001 = BE */

#define NV_PCRTC_START              0x600800
#define NV_PCRTC_CONFIG             0x600804

#define NV_PRMCIO_CRX               0x6013d4    /* CRTC index */
#define NV_PRMCIO_CR                0x6013d5    /* CRTC data */

#define NV_PRAMDAC_GENERAL_CONTROL  0x680600
#define   GENCTRL_PIXMIX_ON           (3 << 4)
#define   GENCTRL_VGA_STATE_SEL       (1 << 8)
#define   GENCTRL_ALT_MODE_SEL        (1 << 12)   /* 16 bpp = 565; bez neho 555 */
#define   GENCTRL_BPC_8BITS           (1 << 20)   /* 8-bit CLUT/gamma */

#define NV_PRAMDAC_FP_TG_CONTROL    0x680848
#define   FP_TG_MODE_MASK             (3 << 8)
#define   FP_TG_MODE_SCALE            (0 << 8)
#define   FP_TG_MODE_CENTER           (1 << 8)
#define   FP_TG_MODE_NATIVE           (2 << 8)

#define NV_PRAMDAC_FP_DEBUG_0       0x680880
#define   FP_DEBUG0_XSCALE_ENABLE     (1 << 0)
#define   FP_DEBUG0_YSCALE_ENABLE     (1 << 4)
#define   FP_DEBUG0_XINTERP_BILINEAR  (1 << 8)
#define   FP_DEBUG0_YINTERP_BILINEAR  (1 << 12)
#define NV_PRAMDAC_FP_DEBUG_1       0x680884    /* 0 = automatický scaler */

#define NV_PRMDIO_PIXEL_MASK        0x6813c6
#define NV_PRMDIO_WRITE_ADDR        0x6813c8
#define NV_PRMDIO_PALETTE_DATA      0x6813c9

#define NV_HEAD_STRIDE              0x2000      /* hlava 1 = +0x2000 v PCRTC/PRMCIO/PRAMDAC/PRMDIO */

/* ---- CRTC indexy ---------------------------------------------------- */

#define CR_HDE          0x01    /* horizontal display end (znaky - 1) */
#define CR_HBS          0x02
#define CR_OVL          0x07    /* overflow: b1 VDE8, b3 VBS8, b6 VDE9 */
#define CR_CELL_HT      0x09    /* b5 VBS9 */
#define CR_VRE          0x11    /* b7 = write-protect CR00..CR07 */
#define CR_VDE          0x12
#define CR_OFFSET       0x13    /* pitch / 8, bity 7:0 */
#define CR_VBS          0x15
#define CR_RPC0         0x19    /* b7:5 = pitch bity 10:8 */
#define CR_FF           0x1b    /* FIFO burst */
#define CR_LOCK         0x1f    /* 0x57 odomkne rozšírené registre */
#define CR_FFLWM        0x20    /* FIFO low watermark */
#define CR_LSR          0x25    /* b1 VDE10, b3 VBS10 */
#define CR_PIXEL        0x28    /* b1:0 = 1/2/3 → 8/16/32 bpp, b7 = slave (FP) */
#define CR_HEB          0x2d    /* b1 HDE8, b2 HBS8 */
#define CR_42           0x42    /* b6 = pitch bit 11 */
#define CR_LFB_SWAP     0x46    /* b7 = byte swap cez LFB apertúru (big endian CPU) */
#define CR_OWNER        0x44    /* na hlave 0: 0 = VGA vlastní hlava A, 3 = hlava B */
#define CR_47           0x47    /* FIFO lwm bity 15:8 */

#define NV_CR_UNLOCK    0x57

/* ---- stav ----------------------------------------------------------- */

typedef struct NVNativeState {
    UInt8   cr_pixel;
    UInt8   cr_offset, cr_rpc0, cr_42, cr_lfb;
    UInt8   cr_hde, cr_hbs, cr_heb;
    UInt8   cr_vde, cr_vbs, cr_ovl, cr_cell, cr_lsr;
    UInt8   cr_ff, cr_fflwm, cr_47;
    UInt32  hde, hbs, vde, vbs;
    UInt32  gen_ctrl, fp_tg, fp_debug0, fp_debug1;
    UInt32  start;
} NVNativeState;

typedef struct NVHeadDiag {
    UInt32  cr;         /* CR28 | CR13 << 8 | CR19 << 16 | CR42 << 24 */
    UInt32  start, fp_tg, gen_ctrl, crtc_cfg;
    int     score;
} NVHeadDiag;

typedef struct NVHw {
    volatile UInt8 *mmio;
    volatile UInt8 *vram;       /* logická adresa BAR1 */
    UInt32          vramSize;
    int             bigEndianRegs;
    UInt32          byteXor;            /* XOR pre 8-bit MMIO adresy (0 alebo 3) */
    int             byteLanesVerified;
    int             head;
    UInt32          boot0;
    UInt32          fbOffset;   /* offset scanoutu vo VRAM (NV_PCRTC_START) */
    UInt32          curBpp;     /* aktuálne nastavené v HW */
    UInt32          curPitch;
    int             scaled;
    int             keepEndian; /* štart v 16/32 bpp → endianitu nastavil predchodca */
    UInt32          pcrtcConfig;
    NVNativeState   nat;
    UInt8           cr44;
    NVHeadDiag      headDiag[2];
} NVHw;

UInt32  nv_rd32(NVHw *hw, UInt32 reg);
void    nv_wr32(NVHw *hw, UInt32 reg, UInt32 val);
UInt8   nv_rdcr(NVHw *hw, int head, UInt8 idx);
void    nv_wrcr(NVHw *hw, int head, UInt8 idx, UInt8 val);

/* Detekcia endianity, overenie čipu, výber hlavy, snapshot stavu z OF.
 * Do HW nezapisuje nič okrem odomknutia CR1F. */
OSStatus nv_probe(NVHw *hw, int regHead, UInt32 expBpp, UInt32 expPitch);

/* Aktuálny formát podľa HW (po OF alebo predchádzajúcom driveri). */
UInt32  nv_current_bpp(NVHw *hw);
UInt32  nv_current_pitch(NVHw *hw);

/* Nastaví formát pixelov, pitch a (ak w×h < natívne) FP scaler. */
void    nv_set_mode(NVHw *hw, UInt32 width, UInt32 height, UInt32 nativeW, UInt32 nativeH,
                    UInt32 bpp, UInt32 pitch);

/* Zapíše `count` po sebe idúcich položiek HW palety od `start` (8-bit RGB). */
void    nv_load_palette(NVHw *hw, UInt32 start, UInt32 count, const UInt8 *r, const UInt8 *g, const UInt8 *b);

/* Vráti displej do natívneho stavu, ako ho nechal OF (finalize/superseded). */
void    nv_restore(NVHw *hw);

#endif

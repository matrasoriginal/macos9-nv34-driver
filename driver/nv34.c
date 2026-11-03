#include "nv34.h"

#ifndef NV_SET_ARB
#define NV_SET_ARB 0    /* 1 = FIFO arbitration per nouveau nv20_update_arb; 0 = keep the OF values (already fine for 32 bpp) */
#endif

/* ---- MMIO access ------------------------------------------------------ */

static inline void eieio(void)
{
    __asm__ volatile("eieio" ::: "memory");
}

UInt32 nv_rd32(NVHw *hw, UInt32 reg)
{
    volatile UInt8 *a = hw->mmio + reg;
    UInt32 v;
    if (hw->bigEndianRegs)
        v = *(volatile UInt32 *)a;
    else
        __asm__ volatile("lwbrx %0,0,%1" : "=r"(v) : "r"(a) : "memory");
    eieio();
    return v;
}

void nv_wr32(NVHw *hw, UInt32 reg, UInt32 val)
{
    volatile UInt8 *a = hw->mmio + reg;
    if (hw->bigEndianRegs)
        *(volatile UInt32 *)a = val;
    else
        __asm__ volatile("stwbrx %0,0,%1" : : "r"(val), "r"(a) : "memory");
    eieio();
}

/* Safeguard for big-endian mode (PMC_BOOT_1 = 0x01000001): if the card swapped bytes
 * on 8-bit accesses too, VGA port 0x3d4 would sit at 0x3d7 (byteXor = 3). On the PB 6,2 the
 * echo test gave byteXor = 0 — 8-bit accesses are at their natural addresses. */
static UInt8 rd8(NVHw *hw, UInt32 reg)
{
    UInt8 v = hw->mmio[reg ^ hw->byteXor];
    eieio();
    return v;
}

static void wr8(NVHw *hw, UInt32 reg, UInt8 v)
{
    hw->mmio[reg ^ hw->byteXor] = v;
    eieio();
}

/* The CRTC index register is readable: write two values and check the readback. */
static int index_echo(NVHw *hw, UInt32 x)
{
    hw->byteXor = x;
    wr8(hw, NV_PRMCIO_CRX, 0x28);
    if (rd8(hw, NV_PRMCIO_CRX) != 0x28)
        return 0;
    wr8(hw, NV_PRMCIO_CRX, 0x13);
    return rd8(hw, NV_PRMCIO_CRX) == 0x13;
}

static void detect_byte_lanes(NVHw *hw)
{
    UInt32 first = hw->bigEndianRegs ? 3 : 0;

    hw->byteLanesVerified = 1;
    if (index_echo(hw, first))
        return;
    if (index_echo(hw, first ^ 3))
        return;
    hw->byteLanesVerified = 0;
    hw->byteXor = first;
}

UInt8 nv_rdcr(NVHw *hw, int head, UInt8 idx)
{
    UInt32 base = head * NV_HEAD_STRIDE;
    wr8(hw, NV_PRMCIO_CRX + base, idx);
    return rd8(hw, NV_PRMCIO_CR + base);
}

void nv_wrcr(NVHw *hw, int head, UInt8 idx, UInt8 val)
{
    UInt32 base = head * NV_HEAD_STRIDE;
    wr8(hw, NV_PRMCIO_CRX + base, idx);
    wr8(hw, NV_PRMCIO_CR + base, val);
}

static UInt32 rd_head(NVHw *hw, UInt32 reg)
{
    return nv_rd32(hw, reg + hw->head * NV_HEAD_STRIDE);
}

static void wr_head(NVHw *hw, UInt32 reg, UInt32 val)
{
    nv_wr32(hw, reg + hw->head * NV_HEAD_STRIDE, val);
}

#define CR(idx)         nv_rdcr(hw, hw->head, (idx))
#define SETCR(idx, v)   nv_wrcr(hw, hw->head, (idx), (UInt8)(v))

/* ---- probe ------------------------------------------------------------- */

static void snapshot(NVHw *hw)
{
    NVNativeState *n = &hw->nat;

    n->cr_pixel = CR(CR_PIXEL);
    n->cr_offset = CR(CR_OFFSET);
    n->cr_rpc0 = CR(CR_RPC0);
    n->cr_42 = CR(CR_42);
    n->cr_lfb = CR(CR_LFB_SWAP);
    n->cr_hde = CR(CR_HDE);
    n->cr_hbs = CR(CR_HBS);
    n->cr_heb = CR(CR_HEB);
    n->cr_vde = CR(CR_VDE);
    n->cr_vbs = CR(CR_VBS);
    n->cr_ovl = CR(CR_OVL);
    n->cr_cell = CR(CR_CELL_HT);
    n->cr_lsr = CR(CR_LSR);
    n->cr_ff = CR(CR_FF);
    n->cr_fflwm = CR(CR_FFLWM);
    n->cr_47 = CR(CR_47);

    n->hde = n->cr_hde | ((n->cr_heb >> 1) & 1) << 8;
    n->hbs = n->cr_hbs | ((n->cr_heb >> 2) & 1) << 8;
    n->vde = n->cr_vde | ((n->cr_ovl >> 1) & 1) << 8 | ((n->cr_ovl >> 6) & 1) << 9 | ((n->cr_lsr >> 1) & 1) << 10;
    n->vbs = n->cr_vbs | ((n->cr_ovl >> 3) & 1) << 8 | ((n->cr_cell >> 5) & 1) << 9 | ((n->cr_lsr >> 3) & 1) << 10;

    n->gen_ctrl = rd_head(hw, NV_PRAMDAC_GENERAL_CONTROL);
    n->fp_tg = rd_head(hw, NV_PRAMDAC_FP_TG_CONTROL);
    n->fp_debug0 = rd_head(hw, NV_PRAMDAC_FP_DEBUG_0);
    n->fp_debug1 = rd_head(hw, NV_PRAMDAC_FP_DEBUG_1);
    n->start = rd_head(hw, NV_PCRTC_START);
}

static UInt32 head_bpp(NVHw *hw, int h)
{
    UInt8 px = nv_rdcr(hw, h, CR_PIXEL) & 3;
    return px == 2 ? 16 : px == 3 ? 32 : 8;
}

static UInt32 head_pitch(NVHw *hw, int h)
{
    UInt32 units = nv_rdcr(hw, h, CR_OFFSET) | ((nv_rdcr(hw, h, CR_RPC0) >> 5) & 7) << 8 |
                   ((nv_rdcr(hw, h, CR_42) >> 6) & 1) << 11;
    return units << 3;
}

OSStatus nv_probe(NVHw *hw, int regHead, UInt32 expBpp, UInt32 expPitch)
{
    UInt32 boot1, chipset;
    int h, best = -1, bestScore = -1;

    /* NV_PMC_BOOT_1 is 0 (LE) or 0x01000001 (BE) — symmetric, reads the same in both modes. */
    boot1 = *(volatile UInt32 *)(hw->mmio + NV_PMC_BOOT_1);
    eieio();
    hw->bigEndianRegs = (boot1 == 0x01000001);

    hw->boot0 = nv_rd32(hw, NV_PMC_BOOT_0);
    chipset = (hw->boot0 >> 20) & 0x1ff;
    if (chipset != 0x34)
        return paramErr;

    detect_byte_lanes(hw);
    hw->cr44 = nv_rdcr(hw, 0, CR_OWNER);

    /* Active head = the one whose format and pitch match what OF/boot ROM wrote into
     * `depth`/`linebytes`. PB 6,2: the panel is driven by head 1 (CR28 0x83, pitch 4096), head 0 stays
     * at 8 bpp/1024 from the OF prompt — even though the node is called Display-A and has `reg 0`. */
    for (h = 0; h < 2; h++) {
        NVHeadDiag *d = &hw->headDiag[h];
        int score = 0;

        nv_wrcr(hw, h, CR_LOCK, NV_CR_UNLOCK);
        d->start = nv_rd32(hw, NV_PCRTC_START + h * NV_HEAD_STRIDE);
        d->fp_tg = nv_rd32(hw, NV_PRAMDAC_FP_TG_CONTROL + h * NV_HEAD_STRIDE);
        d->gen_ctrl = nv_rd32(hw, NV_PRAMDAC_GENERAL_CONTROL + h * NV_HEAD_STRIDE);
        d->crtc_cfg = nv_rd32(hw, NV_PCRTC_CONFIG + h * NV_HEAD_STRIDE);
        d->cr = nv_rdcr(hw, h, CR_PIXEL) | nv_rdcr(hw, h, CR_OFFSET) << 8 |
                nv_rdcr(hw, h, CR_RPC0) << 16 | (UInt32)nv_rdcr(hw, h, CR_42) << 24;
        if (head_bpp(hw, h) == expBpp && head_pitch(hw, h) == expPitch)
            score += 8;
        if (d->start == hw->fbOffset)
            score += 4;
        if ((d->fp_tg & (3 << 28)) == (1 << 28))      /* DISPEN_POS = FP timing generator running */
            score += 2;
        if (h == regHead)
            score += 1;
        d->score = score;
        if (score > bestScore) {
            bestScore = score;
            best = h;
        }
    }
    hw->head = best;
    snapshot(hw);
    hw->curBpp = nv_current_bpp(hw);
    hw->curPitch = nv_current_pitch(hw);
    hw->pcrtcConfig = rd_head(hw, NV_PCRTC_CONFIG);
    /* We leave endianness alone if the scanout already handles it (PCRTC_CONFIG bit 31, OF on PB 6,2 — even when
     * starting in 8 bpp) or if the predecessor already displayed a direct mode. Otherwise the CR46 swap would
     * swap the bytes a second time when switching to 16/32 bpp. */
    hw->keepEndian = (hw->curBpp > 8) || (hw->pcrtcConfig & 0x80000000);
    hw->scaled = 0;
    return noErr;
}

UInt32 nv_current_bpp(NVHw *hw)
{
    switch (CR(CR_PIXEL) & 3) {
    case 2:
        return 16;
    case 3:
        return 32;
    default:
        return 8;
    }
}

UInt32 nv_current_pitch(NVHw *hw)
{
    UInt32 units = CR(CR_OFFSET) | ((CR(CR_RPC0) >> 5) & 7) << 8 | ((CR(CR_42) >> 6) & 1) << 11;
    return units << 3;
}

/* ---- modes ------------------------------------------------------------ */

static void set_pitch(NVHw *hw, UInt32 pitch)
{
    UInt32 units = pitch >> 3;

    SETCR(CR_OFFSET, units & 0xff);
    SETCR(CR_RPC0, (CR(CR_RPC0) & 0x1f) | ((units >> 8) & 7) << 5);
    SETCR(CR_42, (CR(CR_42) & ~0x40) | ((units >> 11) & 1) << 6);
}

/* Size of the active CRTC area. Totals/sync stay as OF set them — the CRTC runs as a slave
 * of the FP timing generator (CR28 b7), which holds native 1024×768 @ 65 MHz. */
static void set_display_size(NVHw *hw, UInt32 width, UInt32 height)
{
    NVNativeState *n = &hw->nat;
    UInt32 hde = (width >> 3) - 1;
    UInt32 vde = height - 1;
    UInt8 vre = CR(CR_VRE);
    UInt8 heb = CR(CR_HEB), ovl = CR(CR_OVL), cell = CR(CR_CELL_HT), lsr = CR(CR_LSR);

    SETCR(CR_VRE, vre & 0x7f);     /* unlock CR00–CR07 */

    SETCR(CR_HDE, hde & 0xff);
    heb = (heb & ~0x02) | ((hde >> 8) & 1) << 1;
    if (n->hbs == n->hde) {
        SETCR(CR_HBS, hde & 0xff);
        heb = (heb & ~0x04) | ((hde >> 8) & 1) << 2;
    }
    SETCR(CR_HEB, heb);

    SETCR(CR_VDE, vde & 0xff);
    ovl = (ovl & ~0x42) | ((vde >> 8) & 1) << 1 | ((vde >> 9) & 1) << 6;
    lsr = (lsr & ~0x02) | ((vde >> 10) & 1) << 1;
    if (n->vbs == n->vde) {
        SETCR(CR_VBS, vde & 0xff);
        ovl = (ovl & ~0x08) | ((vde >> 8) & 1) << 3;
        cell = (cell & ~0x20) | ((vde >> 9) & 1) << 5;
        lsr = (lsr & ~0x08) | ((vde >> 10) & 1) << 3;
    }
    SETCR(CR_OVL, ovl);
    SETCR(CR_CELL_HT, cell);
    SETCR(CR_LSR, lsr);

    SETCR(CR_VRE, vre);
}

static void restore_display_size(NVHw *hw)
{
    NVNativeState *n = &hw->nat;
    UInt8 vre = CR(CR_VRE);

    SETCR(CR_VRE, vre & 0x7f);
    SETCR(CR_HDE, n->cr_hde);
    SETCR(CR_HBS, n->cr_hbs);
    SETCR(CR_HEB, n->cr_heb);
    SETCR(CR_VDE, n->cr_vde);
    SETCR(CR_VBS, n->cr_vbs);
    SETCR(CR_OVL, n->cr_ovl);
    SETCR(CR_CELL_HT, n->cr_cell);
    SETCR(CR_LSR, n->cr_lsr);
    SETCR(CR_VRE, vre);
}

void nv_set_mode(NVHw *hw, UInt32 width, UInt32 height, UInt32 nativeW, UInt32 nativeH,
                 UInt32 bpp, UInt32 pitch)
{
    NVNativeState *n = &hw->nat;
    UInt32 gen = rd_head(hw, NV_PRAMDAC_GENERAL_CONTROL);
    int scale = (width != nativeW || height != nativeH);

    SETCR(CR_LOCK, NV_CR_UNLOCK);

    /* Pixel format: (depth + 1) / 8 as in nouveau; keep the slave bit for the FP. */
    SETCR(CR_PIXEL, (n->cr_pixel & 0x80) | (bpp == 8 ? 1 : bpp == 16 ? 2 : 3));

    /* Mac "Thousands" is xRGB 1555 → ALT_MODE_SEL (565) off. Always 8-bit palette. */
    gen = (gen & ~GENCTRL_ALT_MODE_SEL) | GENCTRL_BPC_8BITS;
    wr_head(hw, NV_PRAMDAC_GENERAL_CONTROL, gen);

    set_pitch(hw, pitch);
    wr_head(hw, NV_PCRTC_START, hw->fbOffset);

    /* Big-endian CPU: the LFB aperture swaps bytes according to depth (xf86-video-nv).
     * If we started on top of a driver that already displayed a direct mode, we leave its
     * endianness setting (CR46 / PCRTC_CONFIG) alone. */
    if (!hw->keepEndian)
        nv_wrcr(hw, hw->head, CR_LFB_SWAP, nv_rdcr(hw, hw->head, CR_LFB_SWAP) | 0x80);
    if (!hw->keepEndian && hw->head != 0) {
        nv_wrcr(hw, 0, CR_LOCK, NV_CR_UNLOCK);
        nv_wrcr(hw, 0, CR_LFB_SWAP, nv_rdcr(hw, 0, CR_LFB_SWAP) | 0x80);
    }

#if NV_SET_ARB
    /* nouveau nv20_update_arb: fifo 2048, burst 512 → burst = log2(512 >> 5), lwm = 1536 >> 3 */
    SETCR(CR_FF, 4);
    SETCR(CR_FFLWM, (1536 >> 3) & 0xff);
    SETCR(CR_47, (1536 >> 3) >> 8);
#endif

    if (scale) {
        set_display_size(hw, width, height);
        wr_head(hw, NV_PRAMDAC_FP_DEBUG_0, n->fp_debug0 | FP_DEBUG0_XSCALE_ENABLE | FP_DEBUG0_YSCALE_ENABLE |
                                              FP_DEBUG0_XINTERP_BILINEAR | FP_DEBUG0_YINTERP_BILINEAR);
        wr_head(hw, NV_PRAMDAC_FP_DEBUG_1, 0);
        wr_head(hw, NV_PRAMDAC_FP_TG_CONTROL, (n->fp_tg & ~FP_TG_MODE_MASK) | FP_TG_MODE_SCALE);
    } else if (hw->scaled) {
        restore_display_size(hw);
        wr_head(hw, NV_PRAMDAC_FP_DEBUG_0, n->fp_debug0);
        wr_head(hw, NV_PRAMDAC_FP_DEBUG_1, n->fp_debug1);
        wr_head(hw, NV_PRAMDAC_FP_TG_CONTROL, n->fp_tg);
    }

    hw->scaled = scale;
    hw->curBpp = bpp;
    hw->curPitch = pitch;
}

void nv_load_palette(NVHw *hw, UInt32 start, UInt32 count, const UInt8 *r, const UInt8 *g, const UInt8 *b)
{
    UInt32 base = hw->head * NV_HEAD_STRIDE;
    UInt32 i;

    wr8(hw, NV_PRMDIO_PIXEL_MASK + base, 0xff);
    wr8(hw, NV_PRMDIO_WRITE_ADDR + base, (UInt8)start);
    for (i = 0; i < count; i++) {
        wr8(hw, NV_PRMDIO_PALETTE_DATA + base, r[i]);
        wr8(hw, NV_PRMDIO_PALETTE_DATA + base, g[i]);
        wr8(hw, NV_PRMDIO_PALETTE_DATA + base, b[i]);
    }
}

void nv_restore(NVHw *hw)
{
    NVNativeState *n = &hw->nat;

    SETCR(CR_LOCK, NV_CR_UNLOCK);
    if (hw->scaled)
        restore_display_size(hw);
    SETCR(CR_PIXEL, n->cr_pixel);
    SETCR(CR_OFFSET, n->cr_offset);
    SETCR(CR_RPC0, n->cr_rpc0);
    SETCR(CR_42, n->cr_42);
    SETCR(CR_LFB_SWAP, n->cr_lfb);
    SETCR(CR_FF, n->cr_ff);
    SETCR(CR_FFLWM, n->cr_fflwm);
    SETCR(CR_47, n->cr_47);
    wr_head(hw, NV_PRAMDAC_GENERAL_CONTROL, n->gen_ctrl);
    wr_head(hw, NV_PRAMDAC_FP_DEBUG_0, n->fp_debug0);
    wr_head(hw, NV_PRAMDAC_FP_DEBUG_1, n->fp_debug1);
    wr_head(hw, NV_PRAMDAC_FP_TG_CONTROL, n->fp_tg);
    wr_head(hw, NV_PCRTC_START, n->start);
    hw->scaled = 0;
    hw->curBpp = nv_current_bpp(hw);
    hw->curPitch = nv_current_pitch(hw);
}

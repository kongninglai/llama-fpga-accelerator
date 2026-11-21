#ifndef TILED_GEMV_H
#define TILED_GEMV_H

#include <stdint.h>
#include <ap_int.h>
#include "tiled_gemv_regs.h"

typedef ap_uint<128> tg_word_t;
/* Co-simulation adapter capacities, not synthesized runtime limits. */
#define TG_DEPTH_W 17408
#define TG_DEPTH_Y 2176
#define TG_DEPTH_OUT 64

/* Q8_0: little-endian FP16 scale followed by 32 signed bytes, no row padding.
 * Input bases must be 16-byte aligned, allocations rounded up to 16 bytes.
 * rows0*nb must be a multiple of 8 when w1 points immediately after w0.
 * Output bases must be 4-byte aligned. Zero rows disables an engine.
 * Any invalid dimension rejects BOTH engines without memory accesses.
 * Poll AP_CTRL.ap_done; status is a result, not a completion doorbell.
 */
extern "C" void tiled_gemv_accel(
    const tg_word_t *w0, const tg_word_t *y0, float *out0,
    const tg_word_t *w1, const tg_word_t *y1, float *out1,
    uint32_t nb, uint32_t rows0, uint32_t rows1, uint32_t *status);

#endif

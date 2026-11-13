#ifndef DOTPROD_H
#define DOTPROD_H
#include <stdint.h>

enum {
    DOTPROD_BLOCK_ELEMS = 32,
    DOTPROD_BLOCK_BYTES = 34,
    DOTPROD_MAX_BLOCKS = 1024,
    DOTPROD_MAX_ELEMS = 32768,
    DOTPROD_INPUT_WORDS = 8704, // 1024 * 34 / 4
    DOTPROD_CTRL = 0x00,
    DOTPROD_GIE = 0x04,
    DOTPROD_IER = 0x08,
    DOTPROD_ISR = 0x0c,
    DOTPROD_LENGTH = 0x20,
    DOTPROD_RESULT = 0x30,
    DOTPROD_STATUS = 0x40,
    DOTPROD_X = 0x10000,
    DOTPROD_Y = 0x20000,
    DOTPROD_MAP_BYTES = 0x40000,
    DOTPROD_DONE = 2,
    DOTPROD_ERROR = 4
};

#ifdef __cplusplus
extern "C" {
#endif
// Each word contains four consecutive little-endian bytes of packed Q8_0.
// These are AXI-Lite memory windows, NOT DDR addresses or pointer registers.
void dotprod_accel(uint32_t n, uint32_t x[DOTPROD_INPUT_WORDS],
                   uint32_t y[DOTPROD_INPUT_WORDS],
                   float *result, uint32_t *status);
#ifdef __cplusplus
}
#endif
#endif

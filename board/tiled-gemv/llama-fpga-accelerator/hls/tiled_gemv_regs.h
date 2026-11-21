#ifndef TILED_GEMV_REGS_H
#define TILED_GEMV_REGS_H

/* Explicit HLS offsets; verify against xtiled_gemv_accel_hw.h after export. */
#define TG_REG_CTRL    0x00
#define TG_REG_GIE     0x04
#define TG_REG_IER     0x08
#define TG_REG_ISR     0x0c
#define TG_REG_W0      0x10
#define TG_REG_Y0      0x20
#define TG_REG_OUT0    0x30
#define TG_REG_W1      0x40
#define TG_REG_Y1      0x50
#define TG_REG_OUT1    0x60
#define TG_REG_NB      0x70
#define TG_REG_ROWS0   0x80
#define TG_REG_ROWS1   0x90
#define TG_REG_STATUS  0xa0
#define TG_CTRL_START  1u
#define TG_CTRL_DONE   2u
#define TG_CTRL_IDLE   4u
#define TG_STATUS_DONE 2u
#define TG_STATUS_ERROR 4u
#define TG_QK         32u
#define TG_BLOCK_BYTES 34u
#define TG_MAX_BLOCKS 1024u
#define TG_MAX_ROWS   262144u

#endif

// ==============================================================
// Vitis HLS - High-Level Synthesis from C, C++ and OpenCL v2022.2 (64-bit)
// Tool Version Limit: 2019.12
// Copyright 1986-2022 Xilinx, Inc. All Rights Reserved.
// ==============================================================
// CTRL
// 0x00000 : Control signals
//           bit 0  - ap_start (Read/Write/COH)
//           bit 1  - ap_done (Read/COR)
//           bit 2  - ap_idle (Read)
//           bit 3  - ap_ready (Read/COR)
//           bit 7  - auto_restart (Read/Write)
//           bit 9  - interrupt (Read)
//           others - reserved
// 0x00004 : Global Interrupt Enable Register
//           bit 0  - Global Interrupt Enable (Read/Write)
//           others - reserved
// 0x00008 : IP Interrupt Enable Register (Read/Write)
//           bit 0 - enable ap_done interrupt (Read/Write)
//           bit 1 - enable ap_ready interrupt (Read/Write)
//           others - reserved
// 0x0000c : IP Interrupt Status Register (Read/TOW)
//           bit 0 - ap_done (Read/TOW)
//           bit 1 - ap_ready (Read/TOW)
//           others - reserved
// 0x00020 : Data signal of n
//           bit 31~0 - n[31:0] (Read/Write)
// 0x00024 : reserved
// 0x00030 : Data signal of result
//           bit 31~0 - result[31:0] (Read)
// 0x00034 : Control signal of result
//           bit 0  - result_ap_vld (Read/COR)
//           others - reserved
// 0x00040 : Data signal of status
//           bit 31~0 - status[31:0] (Read)
// 0x00044 : Control signal of status
//           bit 0  - status_ap_vld (Read/COR)
//           others - reserved
// 0x10000 ~
// 0x1ffff : Memory 'x' (8704 * 32b)
//           Word n : bit [31:0] - x[n]
// 0x20000 ~
// 0x2ffff : Memory 'y' (8704 * 32b)
//           Word n : bit [31:0] - y[n]
// (SC = Self Clear, COR = Clear on Read, TOW = Toggle on Write, COH = Clear on Handshake)

#define XDOTPROD_ACCEL_CTRL_ADDR_AP_CTRL     0x00000
#define XDOTPROD_ACCEL_CTRL_ADDR_GIE         0x00004
#define XDOTPROD_ACCEL_CTRL_ADDR_IER         0x00008
#define XDOTPROD_ACCEL_CTRL_ADDR_ISR         0x0000c
#define XDOTPROD_ACCEL_CTRL_ADDR_N_DATA      0x00020
#define XDOTPROD_ACCEL_CTRL_BITS_N_DATA      32
#define XDOTPROD_ACCEL_CTRL_ADDR_RESULT_DATA 0x00030
#define XDOTPROD_ACCEL_CTRL_BITS_RESULT_DATA 32
#define XDOTPROD_ACCEL_CTRL_ADDR_RESULT_CTRL 0x00034
#define XDOTPROD_ACCEL_CTRL_ADDR_STATUS_DATA 0x00040
#define XDOTPROD_ACCEL_CTRL_BITS_STATUS_DATA 32
#define XDOTPROD_ACCEL_CTRL_ADDR_STATUS_CTRL 0x00044
#define XDOTPROD_ACCEL_CTRL_ADDR_X_BASE      0x10000
#define XDOTPROD_ACCEL_CTRL_ADDR_X_HIGH      0x1ffff
#define XDOTPROD_ACCEL_CTRL_WIDTH_X          32
#define XDOTPROD_ACCEL_CTRL_DEPTH_X          8704
#define XDOTPROD_ACCEL_CTRL_ADDR_Y_BASE      0x20000
#define XDOTPROD_ACCEL_CTRL_ADDR_Y_HIGH      0x2ffff
#define XDOTPROD_ACCEL_CTRL_WIDTH_Y          32
#define XDOTPROD_ACCEL_CTRL_DEPTH_Y          8704


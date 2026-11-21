# Tiled Q8_0 GEMV v1

This IP is derived from `../bus-mastering-gemv/gemv_q8_0.cpp`. It keeps the
two-engine arithmetic/dataflow pipeline and the existing register layout.
Software submits a resident matrix or a row tile, then prepares another tile
while this IP runs. There is no hardware command queue and no separate AXI DMA IP.

Changes from the original IP:

- Top function: `tiled_gemv_accel`.
- Validate the entire request before either engine accesses memory.
- Publish result status after the dataflow helper returns. The host still waits
  for `AP_CTRL.ap_done`, not the status payload or its valid bit.
- Include a C/RTL testbench, a 150 MHz synthesis target, and register-map checking.

## Build

Using Vitis HLS 2022.2:

```sh
cd hls/tiled-gemv
vitis_hls -f run_hls.tcl
python3 check_registers.py build/tiled_gemv/solution1/impl/ip/drivers/tiled_gemv_accel_v1_0/src/xtiled_gemv_accel_hw.h
```

The script runs C simulation, synthesis, RTL co-simulation and IP export. It uses
`xczu3eg-sbva484-1-e` and a 6.667 ns target. Override with `TG_PART` and
`TG_PERIOD_NS` if needed. The generated project is under `build/`.
Adapter depths in the interface pragmas bound the co-simulation testbench only;
they do not restrict the runtime matrix size. Extend those depths and the
testbench allocations together when adding larger co-simulation cases.

Review achieved II in fetch, unpack and compute, dataflow stalls, resource use,
and post-route timing. `PIPELINE II=1` is a request. At the specified input width,
compute II <= 2 is sufficient to keep up with an ideal continuous read stream;
the unpacker and fetch stage must sustain one word per cycle.

## Vivado integration

1. Add the exported IP to the Ultra96 project and instantiate it in place of the
   prior accelerator.
2. Connect `s_axi_CTRL` to the PS AXI master through the control interconnect.
   Assign the existing 256 KiB segment at `0xA0000000`.
3. Connect `m_axi_gmem0` through one SmartConnect to `S_AXI_HP0_FPD`, and
   `m_axi_gmem1` through another to `S_AXI_HP1_FPD`, both with 128-bit data paths.
   Make the DMA DDR region accessible in both masters' address maps.
4. Connect the IP, interconnect and PS AXI clocks to the same 150 MHz PL clock,
   with a synchronized active-low reset for `ap_rst_n`.
5. The v1 driver polls; the interrupt output is unused. Generate the bitstream
   and export matching `.bit` and `.hwh` files.

The HWH clock setting is not proof of the live board clock. Ensure the loader
configures PL0 to 150 MHz. The driver logs the clock rate returned by Linux.
Do not mark the device `dma-coherent` for noncoherent HP ports. The driver uses
the Linux coherent DMA allocation/mmap APIs to provide suitable CPU mappings.

## Interface

Inputs use the native little-endian Q8_0 layout: 2-byte FP16 scale followed by
32 signed INT8 values. Each row contains `nb` consecutive 34-byte blocks with
no inter-row padding. Each engine caches its activation vector on chip.

Input bases must be 16-byte aligned and readable through the final rounded-up
16-byte beat. Output bases are FP32 and require 4-byte alignment. Splitting a
single contiguous matrix at a multiple of eight rows makes engine 1's input
base aligned for every `nb`. The driver handles this split.

`nb` is 1..1024; each engine supports 0..262144 rows, with zero disabling the
engine. The v1 software limits a job to 262144 total rows (1 MiB output buffer).
Invalid dimensions return status 6 without touching any input/output buffers;
success returns status 2. FP32 accumulation order differs from llama.cpp, so
numerical comparisons use a tolerance scaled by the sum of absolute block terms.

See `../../hal/tiled-gemv/README.md` for the asynchronous driver and application.

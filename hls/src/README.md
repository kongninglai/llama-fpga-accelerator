# Ultra96 Q8_0 dot product HLS IP

`dotprod.cpp` is the synthesis source; select **dotprod_accel** as the top.
The original sources in `../hls/` are retained. This implementation uses a
single AXI4-Lite slave, `s_axi_CTRL`, for registers and on-chip operand memory,
like the Board_demo peripheral. It does not fetch operands from DDR, so it
does not need an AXI master, DMA, or cache maintenance for shared DDR buffers.
AXI-Lite operand loading prioritizes simple integration over bandwidth.

## Register interface

This is **not binary compatible with the SystemC MMIO interface**.
HLS generates its own `ap_ctrl_hs` control/interrupt registers; assigning
LENGTH to 0x08 or RESULT to 0x0c would collide with them. The existing driver
must be adapted, or an RTL AXI wrapper must implement the old interface.
Pragmas alone do not reproduce the custom SystemC command/status behavior.

| Offset | Meaning |
| --- | --- |
| 0x00 | HLS control: bit 0 start, bit 1 done (clear on read), bit 2 idle, bit 3 ready, bit 7 auto-restart (leave zero) |
| 0x04 | Global interrupt enable, bit 0 |
| 0x08 | Interrupt enable: enable bit 0 for completion only |
| 0x0c | Interrupt status: completion bit 0, toggle-on-write acknowledgement |
| 0x20 | Element count, 32..32768 inclusive, multiple of 32 |
| 0x30 | Float32 result bits; output-valid control at 0x34 |
| 0x40 | Completion payload: 2 = success, 6 = invalid length; output-valid control at 0x44 |
| 0x10000 | X packed input bytes, 34816 bytes maximum |
| 0x20000 | Y packed input bytes, 34816 bytes maximum |

Array windows reserve power-of-two address space; the whole peripheral is
allocated 0x40000 bytes. **Verify this map against the generated
hardware header (`xdotprod_accel_hw.h`), synthesis
report, and component.xml before building the driver.** HLS versions may
relocate conflicting offsets; a successful synthesis alone is not a map check.

Each block remains exactly 34 bytes: little-endian FP16 scale followed by
32 signed int8 quants. Arrays use 32-bit words to avoid HLS widening each
8-bit array element into a separate AXI-Lite word. Block 1 starts at byte
34, halfway through word 8; no per-block padding is allowed. For an odd
number of blocks, zero-pad only the final transfer to a four-byte boundary.
Use aligned `writel()` transfers assembling four little-endian bytes per word,
including that final padded word; do not rely on arbitrary byte/64-bit
`memcpy_toio()` accesses to the generated memory adapter.

`status` is valid after completion, not a live BUSY/DONE register. The result
is zero for invalid lengths. Accumulation retains the original HLS eight-lane
order, so rounding can differ from SystemC's sequential sum. The testbench
uses the HAL's numerical tolerance, not bitwise equality.

## Driver adaptation

Keep the existing userspace write(X || Y), ioctl, and result/status ABI by
changing the hardware-facing portions of `fpga_drv.c`:

1. Map the new base and 0x40000-byte resource, and update operand, length,
   result and completion-status offsets. Upload operands as described above.
2. At probe, leave GIE/IER disabled until the IRQ handler is installed. Clear
   pending ISR bits by reading ISR and writing back only the observed bits.
   Then write 1 to IER and GIE. Disable them again during removal/error cleanup.
3. Track requests with the driver's existing `pending` state. Use control bit
   2 (`ap_idle`) for live hardware idleness. Do not test status bit 0 for BUSY.
   Reject operand/length writes and second starts while a request is pending.
   Hardware does not enforce SystemC's busy-write rejection.
4. For START, clear stale ISR bits while idle, set pending under the existing
   lock, write length at 0x20, and write 1 to control. Do not write the old
   CONTROL_CLEAR=2 command; HLS does not implement it. Keep auto-restart off.
5. In the interrupt handler, test ISR bit 0, save result at 0x30 and payload
   status at 0x40, then acknowledge ISR by writing back its observed completion
   bit. Keep the driver's saved DONE/ERROR until userspace consumes it. Do not
   use control's clear-on-read DONE bit as the shared IRQ/polling condition.
6. CHECK_STATUS should return saved completion, or software BUSY=1 while a
   request is running. RESET remains an idle acknowledgement/software-state
   clear; it cannot cancel an in-flight operation. Clear observed ISR bits
   instead of writing CONTROL_CLEAR. Hardware reset is the `ap_rst_n` input.

The old SystemC driver and device tree are intentionally still usable for
cosimulation; they have not been overwritten with this different hardware ABI.

## Synthesis and board wiring

Run `vitis_hls -f src/run_hls.tcl` (or `vivado_hls -f src/run_hls.tcl`). It runs
C simulation, synthesis, RTL cosimulation, and exports Vivado IP under
`src/dotprod_hls/solution1/impl/ip/`. The script rebuilds that generated project.
Default target is `xczu3eg-sbva484-1-e` at 100 MHz; set `DOTPROD_PART` to override.
Timing, resources and the requested inner-loop II must be checked in the
synthesis report; no throughput or timing closure is assumed.

In Vivado, add that IP repository and instantiate the exported IP:

1. Configure the Ultra96 Zynq UltraScale+ PS preset, enable PS `M_AXI_HPM0_FPD`,
   PL clock 0 at 100 MHz, and PL-to-PS interrupts.
2. Connect PS `M_AXI_HPM0_FPD` through AXI SmartConnect/Interconnect to
   `dotprod_accel/s_axi_CTRL`. Connect the PS HPM clock, interconnect clock,
   and IP `ap_clk` to the same PL clock.
3. Use Processor System Reset synchronized to that clock. Connect its
   `peripheral_aresetn` to IP `ap_rst_n`, and reset the interconnect as required.
4. Assign an available **256 KiB aligned** address, for example 0xA0040000
   with range 0x40000 after checking the rest of the design. The SystemC base
   0xA0030000 is not aligned for this larger segment. Update the device tree
   and any NO_DTS fallback to the actual Vivado address.
5. Route `interrupt` to bit 5 of PS `pl_ps_irq0[7:0]` through `xlconcat`, with
   unused inputs tied low, to match SystemC's `pl2ps_irq[5]` routing. Obtain
   the Linux IRQ through the device tree; do not hard-code a Linux IRQ number.
   Configure the interrupt as level-high and verify the exported hardware
   interrupt mapping when generating the board device tree.
6. Validate the block design, synthesize/implement, generate the bitstream,
   export the XSA, and build the board software/device tree with the adapted
   driver. HLS pragmas create IP ports; they do not connect the PS or assign
   the physical base address by themselves.

The SystemC top routes dotprod directly through a TLM target socket. Its
separate AXI demo bridges are not the dotprod peripheral's hardware wiring.

## Local numerical test (no AMD tools required)

```sh
clang++ -std=c++11 -O2 -ffp-contract=off -Wno-unknown-pragmas src/dotprod.cpp src/tb_dotprod.cpp -o /tmp/tb_dotprod
/tmp/tb_dotprod
```

This checks computation/packing, not AXI transactions or IRQ semantics.
HLS cosimulation checks generated RTL against C calls; additionally verify
the register map and exercise AXI writes/start/IRQ/acknowledge on the board
or with an AXI bus testbench.

Reference: AMD UG1399,
[AXI-Lite register map](https://docs.amd.com/r/en-US/ug1399-vitis-hls/S_AXILITE-Control-Register-Map),
[explicit offsets](https://docs.amd.com/r/en-US/ug1399-vitis-hls/S_AXILITE-Offset-Option),
and [array protocols](https://docs.amd.com/r/2024.1-English/ug1399-vitis-hls/S_AXILITE-and-Port-Level-Protocols).

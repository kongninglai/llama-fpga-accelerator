# HLS board driver

`fpga_drv.c` adapts the existing SystemC driver for the generated
`../hls/impl/xdotprod_accel_hw.h`, which it includes directly. The original
SystemC driver is untouched. This driver supports one accelerator and native
64-bit ARM userspace, keeping `/dev/fpga`, the existing ioctl numbers, and the
8-byte result/status response used by `fpga_dot_hal.h` and `dotProd_app.c`.

## Fix the exported hardware address range first

The supplied `Ultra96_Design_wrapper.xsa` contains these settings in its HWH:

- `dotprod_accel_0/s_axi_CTRL`: 18 address bits, register block size 262144.
- PS `M_AXI_HPM1_FPD` mapping: **0xA0000000–0xA000FFFF (only 64 KiB)**.
- `dotprod_accel_0/interrupt` connects directly to `pl_ps_irq0[0]`.

The 64 KiB mapping cannot address X at +0x10000 or Y at +0x20000. In Vivado
Address Editor, enlarge that segment to **256 KiB**, keeping base 0xA0000000
if available (new high address 0xA003FFFF). Regenerate the bitstream and XSA,
then rebuild the board device tree. Changing only the Linux mapping cannot
fix the AXI interconnect's hardware decode. The driver rejects device-tree
resources smaller than 256 KiB, but cannot detect an incorrectly enlarged
device tree used with the old bitstream.

Set the generated accelerator node's `compatible` to `"ece382m,dotprod"`.
For a parent with two address cells and two size cells, its relevant properties
after the hardware correction should be:

```dts
compatible = "ece382m,dotprod";
reg = <0x0 0xa0000000 0x0 0x40000>;
interrupt-parent = <&gic>;
interrupts = <0 89 4>;
status = "okay";
```

Modify the generated node, rather than adding a second node claiming the same
resources. Preserve its clock properties and ensure the bitstream, PL clock,
and reset are configured before probing. Use the actual GIC label from your
device tree. IRQ0[0] is GIC ID 121 / SPI index 89, level-high; do not reuse
the SystemC IRQ0[5] routing. The kernel assigns the Linux IRQ number dynamically.
See AMD's [PS interrupt mapping](https://docs.amd.com/r/en-US/pg201-zynq-ultrascale-plus-processing-system/Programmable-Logic-Clocks-and-Interrupts).

## Hardware protocol and preserved userspace ABI

- `write(X || Y)` accepts complete pairs of 34-byte blocks, up to 1024 blocks
  per operand. Only aligned 32-bit `writel()` transfers are used. Odd block
  counts pad just the final word of each operand, preserving packed blocks.
- START (`1U << 27`, argument `unsigned long *`) validates length against the
  uploaded data, writes length at 0x20, then sets AP_CTRL.start at 0x00.
- GIE=1 and IER=1 enable completion interrupts. The handler checks ISR at 0x0c,
  saves result at 0x30 and status at 0x40, then acknowledges only the observed
  ISR bits using toggle-on-write semantics. It never uses clear-on-read
  AP_CTRL.done to decide whether an interrupt belongs to this device.
- CHECK_STATUS (`2U << 27`, argument `unsigned long *`) reports software
  BUSY=1, DONE=2, or DONE|ERROR=6. AP_CTRL.idle/start indicate hardware busy;
  HLS's output status register is only a completion payload.
- `read()` returns `{ uint32_t result_bits; uint32_t status; }`, or `-EAGAIN`
  until the interrupt handler records completion. A failed user copy leaves
  completion available for retry. Existing SIGIO support is retained.
- RESET (`3U << 27`) acknowledges idle completion and clears saved software
  state. It returns `-EBUSY` for an unfinished request; it does not reset or
  cancel the HLS core. Writing the old SystemC CONTROL_CLEAR=2 is removed.
- A pending request blocks another start or upload until read/reset consumes
  completion. Closing does not cancel hardware. Do not unload/reconfigure
  the FPGA while requests are running. Only this driver should access the IP.

Interrupts are disabled before handler registration, enabled before publishing
`/dev/fpga`, and disabled/flushed on probe failure and removal. The driver
retains the original single-device design and disables manual sysfs unbind.

## Build and validation

Build against the **configured and prepared kernel tree used by the board**
(the original project uses a Xilinx Linux kernel):

```sh
make -C llama-fpga-accelerator/hal KDIR=/path/to/linux-xlnx \
    ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
```

On the board, after loading the corrected bitstream/device tree:

```sh
sudo insmod fpga_drv.ko
dmesg | tail
cat /proc/fpga
```

Run the existing dot-product application and llama HAL with validation enabled.
Exercise one block (34-byte tail), two blocks, maximum length, repeated calls,
invalid lengths, and reset while busy. `/proc/fpga` should increment once per
completed request. A device-tree-based probe is preferred; the retained
NO_DTS fallback uses base 0xA0000000 and requires an explicitly supplied Linux
IRQ number.

`make -C llama-fpga-accelerator/hal test` runs host-side tests of actual driver
functions with mocked MMIO and userspace copies. It checks packing, IRQ
acknowledgement, busy detection, and request lifecycle. It does not validate
kernel API compatibility, locking under concurrent execution, or physical AXI
transactions. A real kernel build and board test remain required.

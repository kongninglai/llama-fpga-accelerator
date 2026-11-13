# PYNQ FIT with the dot-product device node

`image.ub` is the original image copied from the board. It is not modified.
`dotprod-fit/image-dotprod.ub` is a separately generated replacement FIT.

Run `python3 repack_dotprod.py` in this directory to reproduce it. Requires
Python 3 and the device-tree tools `dtc`, `fdtget`, and `fdtput`.

The script verifies the original unsigned FIT's SHA-1 payload hashes, preserves
its kernel bytes and all FIT properties except the DT payload and its hash,
and changes only the accelerator node in the embedded device tree. It replaces
`/fabric@A0000000` (generic-uio, 64 KiB, IRQ 89) with
`/dotprod_accel@a0000000` (ece382m,dotprod, 256 KiB, same IRQ). This avoids two
drivers claiming the same hardware. Clock/GIC phandles are resolved from the
original PYNQ tree. All other device-tree nodes, including boot arguments,
memory, root filesystem settings, and PYNQ configuration, are preserved.

This does not substitute the complete PetaLinux 2022.2 device tree into the
existing PYNQ 2022.1 kernel image. The existing board-specific tree is the base.
`dotprod-fit/verification.json` records validation results and SHA-256 hashes.

For this unsigned FIT, the script computes the SHA-1 values and emits the FIT
source with explicit hashes and binary payload references. `dtc` serializes
the FIT, which is then parsed again to verify every property and both hashes.
There are no signatures to regenerate. This workflow must not be applied to
a signed FIT without its signing workflow. See the U-Boot
[FIT format](https://docs.u-boot.org/en/v2024.07/usage/fit/source_file_format.html).

## Deployment

The generated image has not been boot-tested. Keep the original SD boot files
and an SD-card reader available to restore `image.ub` if needed. Copy
`dotprod-fit/image-dotprod.ub` to `/home/xilinx/image-dotprod.ub` on the board.
With the SD boot partition mounted at `/boot`, back up the original image to
a new filename before installing the replacement:

```sh
cp /boot/image.ub /boot/image.ub.backup-$(date +%Y%m%d-%H%M%S)
sha256sum /home/xilinx/image-dotprod.ub
cp /home/xilinx/image-dotprod.ub /boot/image.ub
sync
sha256sum /boot/image.ub
```

Compare both checksums with `verification.json`, then reboot. `boot.scr` and
`BOOT.BIN` remain unchanged: the existing boot script already loads image.ub.
After boot, inspect the live node with:

```sh
dtc -I fs -O dts -o /tmp/running.dts /sys/firmware/devicetree/base
grep -A 15 'dotprod_accel@a0000000' /tmp/running.dts
```

**This FIT contains no FPGA bitstream and does not program the PL.** Program
the matching dotprod bitstream separately, for example through PYNQ Overlay
with its matching HWH, before manually loading `fpga_drv.ko`. Do not configure
automatic loading of this driver before the FPGA has been programmed and its
clock enabled. The module must be built against the running board kernel
`5.15.19-xilinx-v2022.1`. Once this DTB is active, no additional dotprod DT
overlay is needed; adding another node would duplicate the resource claims.

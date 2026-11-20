# Ultra96 PYNQ device-tree overlay workflow

This is the overlay alternative to replacing the complete DTB in image.ub.
Use the **original PYNQ image.ub** copied from the board. Do not install
`dotprod-fit/image-dotprod.ub` or the PetaLinux full system.dtb for this flow.
If you already installed the repacked FIT, back it up, restore the original
`board/image.ub` to the SD boot partition as `image.ub`, and reboot first.

This procedure applies a real `.dtbo` at **Linux startup**, after programming
the FPGA. It is not a U-Boot `fdt apply` procedure. If the course requires the
overlay to be applied before the Linux kernel starts, that is a different boot
script flow; do not assume the systemd procedure satisfies that requirement.

## Files and hardware behavior

| File | Purpose |
| --- | --- |
| dotprod.dtso | Overlay source: disables stock PYNQ UIO and adds accelerator |
| dotprod.dtbo | Compiled device-tree overlay; no FPGA configuration data |
| dotprod.bit | FPGA bitstream extracted from the corrected XSA |
| dotprod.hwh | Matching hardware metadata used by PYNQ for clocks/interfaces |
| load.py | Board startup: preflight, unbind UIO, program PL, apply DTBO, insmod |
| dotprod.service | Optional automatic Linux startup unit |
| verification.json | Offline merge checks and artifact hashes |

The base image has `/fabric@A0000000`, a generic UIO device using the same
address and IRQ as dotprod. `load.py` explicitly unbinds that device before
reconfiguration, and the overlay disables its node. The new platform device
is `/axi/dotprod_accel@a0000000`, compatible `ece382m,dotprod`, with 256 KiB
at 0xA0000000, SPI 89 level-high, and PL clock 0 (clock ID 71).

The overlay uses symbolic `&amba`, `&gic`, and `&zynqmp_clk` references resolved
against the running base tree, not copied numeric phandles. These symbols and
the stock UIO node were verified in your original image.ub. This overlay is
specific to that base image.

## 1. Build the DTBO and prepare the bitstream

Already done in this workspace. To reproduce from the repository root:

```sh
python3 board/overlay/prepare.py
```

This extracts the matching bit/HWH from `hls/impl/Ultra96_Design_wrapper.xsa`,
compiles the overlay, and uses `fdtoverlay` to validate a merge with the
original image.ub's embedded DTB. It does not change the original boot image.
Requires Python 3, dtc and fdtoverlay on the development machine.

To compile just the DTBO after editing its source:

```sh
dtc -@ -I dts -O dtb -o board/overlay/dotprod.dtbo board/overlay/dotprod.dtso
```

`-@` emits the fixup/symbol information used to resolve the overlay. Renaming
system.dtb to system.dtbo does not create an overlay.

## 2. Build the driver and application

The driver must match the board kernel **5.15.19-xilinx-v2022.1**, including
its configuration and module symbol versions. On the build machine:

```sh
make -C hal KDIR=/path/to/prepared/board-kernel \
    ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS=-fno-stack-protector
make -C hal app CROSS_COMPILE=aarch64-linux-gnu- APP_LDFLAGS=-static
```

Use the course-compatible toolchain. A 2022.2 PetaLinux kernel tree is not a
substitute for the running PYNQ kernel's build tree. If compiling natively on
the board with matching headers installed, use:

```sh
make -C hal KDIR=/lib/modules/$(uname -r)/build ARCH=arm64 \
    CROSS_COMPILE= KCFLAGS=-fno-stack-protector
make -C hal app CROSS_COMPILE=
```

If the kernel build directory is absent, obtain the matching prepared PYNQ
kernel tree/headers first. The application was copied from the existing
SystemC application's IRQ test and expects the unchanged /dev/fpga ABI.

## 3. Transfer to the board

For example, from the development machine (replace BOARD_IP):

```sh
scp -r board/overlay root@BOARD_IP:/opt/dotprod
scp hal/fpga_drv.ko hal/dotProd_app root@BOARD_IP:/opt/dotprod/
```

Create `/opt/dotprod` only once with the first command; if it already exists,
copy the overlay directory's contents into it instead of nesting directories.
The final directory must directly contain `load.py`, `dotprod.bit`,
`dotprod.hwh`, `dotprod.dtbo`, `fpga_drv.ko`, and `dotProd_app`.

## 4. Test manually before enabling boot startup

Boot the original PYNQ image and stop notebooks/applications using the FPGA.
Do not pre-load fpga_drv and do not run another overlay loader simultaneously.
On the board, find the Python interpreter providing PYNQ:

```sh
python3 -c 'import pynq; print(pynq.__version__)'
```

If necessary, use `/usr/local/share/pynq-venv/bin/python3` instead. As root,
run the following using the successful interpreter:

```sh
python3 /opt/dotprod/load.py
dmesg | tail -n 30
ls -l /dev/fpga
cat /proc/fpga
/opt/dotprod/dotProd_app
cat /proc/fpga
```

The application should print:

```text
PASS: interrupt received; dot([1 x 32], [2 x 32]) = 64.0
```

The interrupt counter should increase by one for each run. The loader uses
`Overlay(bitfile, dtbo=..., download=False)` to inspect metadata, then
`download()` to configure clocks/FPGA and apply the overlay. Only after that
does it insert fpga_drv. It checks for duplicate dotprod nodes, a preloaded
driver, a wrong kernel release, missing DT symbols, and missing configfs
overlay support. A module's matching release alone does not guarantee matching
kernel configuration/symbol versions; insmod/dmesg must still be checked.

No modifications to /boot are needed for this runtime flow. A reboot resets
it to the original base tree until the service below is enabled.

## 5. Load automatically during Linux boot

After the manual test passes, edit `/opt/dotprod/dotprod.service` so ExecStart
uses the exact Python executable that worked above. It defaults to
`/usr/bin/python3`. `After=pynq.service` orders it after that unit if present;
if your image uses another service to load a base overlay, add that actual
unit name to After as well. No later notebook/startup task should reprogram PL.
The supplied /boot/boot.py contained only commented examples, so it needs no
edit for this workflow.

On the board as root:

```sh
cp /opt/dotprod/dotprod.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable dotprod.service
reboot
```

Enable without `--now` after the manual test: the FPGA/overlay are already
loaded, and the loader intentionally refuses duplicate loading. After reboot:

```sh
systemctl status dotprod.service --no-pager
journalctl -b -u dotprod.service --no-pager
/opt/dotprod/dotProd_app
```

Boot sequence: original image.ub -> PYNQ Linux -> dotprod.service -> bitstream
and clocks -> DTBO -> fpga_drv -> /dev/fpga -> application. Use this device with
the existing FPGA-enabled llama.cpp build after the smoke test passes.

To return to the stock boot behavior, stop accelerator applications, run
`systemctl disable dotprod.service`, then reboot. Do not remove the overlay or
reload a different bitstream while fpga_drv remains attached. The service has
no hot-unload ExecStop; stopping the unit does not undo hardware programming.

## Validation limits and references

The DTBO compiled and merged successfully with your original base DTB. Checks
cover its address/IRQ, clock/GIC fixups, UIO disablement, and preservation of
other existing node properties. Driver host tests also pass. Hardware loading,
module compilation for the PYNQ kernel, and board execution remain untested.

- [Linux device-tree overlays](https://docs.kernel.org/devicetree/overlay-notes.html)
- [PYNQ Overlay download order](https://pynq.readthedocs.io/en/v3.0.0/_modules/pynq/overlay.html)

# Single-thread tiled GEMV v1

Standalone driver, runtime, numerical test and microbenchmark for
`../../hls/tiled-gemv`. This version does not modify or integrate into llama.cpp.
It implements explicit weight plans, persistent resident row prefixes, and
asynchronous double-buffered staging using one CPU thread.

## Files

| File | Purpose |
| --- | --- |
| `tiled_gemv_drv.c` | Linux 5.15 platform driver; INFO, SUBMIT, QUERY, WAIT |
| `tiled_gemv_abi.h` | Fixed-width ABI, descriptor bounds and engine split |
| `tg_runtime.c` | Weight planning and single-thread staging scheduler |
| `tg_backend.c` | Linux device backend and host mock |
| `tiled_gemv_test.c` | Synthetic Q8_0 correctness/performance application |
| `test_runtime.c` | Host tests for numerical and scheduling behavior |
| `tiled-gemv.dtso` | Overlay template for the project's PYNQ image |
| `load.py` | Board loader: program bitstream, apply DTBO, insert prebuilt module |

## Host verification without a board

```sh
cd hal/tiled-gemv
make app test ubsan
make overlay
# Optional, where the compiler's AddressSanitizer runtime works:
make sanitize
./build/tiled_gemv_test --mock --rows 4096 --cols 896 --iterations 3 --cache-mib 1 --stage-kib 64
./build/tiled_gemv_test --mock --rows 4096 --cols 896 --iterations 3 --cache-mib 1 --stage-kib 64 --serial
```

The mock defers computation until WAIT and hashes active inputs to detect buffer
overwrite. Tests exercise fully resident, partly resident and streamed matrices,
odd tails, all supported block widths including 1024, changed activations on
repeated runs, multiple weight plans, descriptor errors, completion sequencing,
lookahead copying and freezing the runtime after a timeout. It creates no helper
threads. Mock times are not predictions of board performance or RTL behavior.

The numerical CPU implementation is a scalar test/reference kernel, **not an
optimized llama.cpp baseline**. Compare eventual performance against llama.cpp
with one thread, separately for decoding and prompt processing.

## Build for the board

Build on PYNQ using the prepared headers for its exact running kernel:

```sh
cd hal/tiled-gemv
make -B app CROSS_COMPILE=
make module KDIR=/lib/modules/$(uname -r)/build ARCH=arm64 CROSS_COMPILE=
make overlay
```

If this kernel header setup still requires the previously used compiler
workaround, append `KCFLAGS=-fno-stack-protector` to the module command.
For cross-compilation, set `CROSS_COMPILE=aarch64-linux-gnu-` and point `KDIR`
to the matching prepared ARM64 kernel build tree. `make` builds the application
only, so the host mock can be used without Linux kernel headers.

Keep the relative `hal/` and `hls/` layout when copying these directories: the
software shares `tiled_gemv_regs.h` with the hardware project.

## Hardware and overlay installation

Copy the matching Vivado exports `tiled_gemv.bit` and `tiled_gemv.hwh` to
`hls/tiled-gemv/impl/` on the board, keeping the repository directory layout.
After manually building the application, module and DTBO above, load once per
clean boot from `hal/tiled-gemv`:

```sh
sudo /usr/local/share/pynq-venv/bin/python3 load.py --dtbo build/tiled-gemv.dtbo
sudo dmesg | tail -20
sudo taskset -c 0 ./build/tiled_gemv_test --rows 17 --cols 96 --iterations 3
```

Without `--dtbo`, the loader uses the already exported
`hls/tiled-gemv/impl/tiled_gemv.dtbo`. Other defaults are that directory's
`tiled_gemv.bit` (with adjacent matching `.hwh`), this directory's
`tiled_gemv_drv.ko`, `--arena-mb 640` and `--engines 2`. Override paths with
`--bitstream`, `--dtbo`, and `--module`; default paths are independent of the
working directory. The script does not build anything or run the application.
It checks the module's kernel release and HWH address map, unbinds the stock
UIO device, programs PL and applies the DTBO through PYNQ, verifies the live DT,
then inserts the module and checks `/dev/tiled_gemv`.

The overlay template expects base-DT symbols `amba` and `zynqmp_clk`. Its second
fragment disables `/fabric@A0000000`, the generic UIO node on the project's PYNQ
image. Omit that fragment if your base tree does not contain that path. Do not
apply it alongside an existing dotprod/GEMV overlay claiming the same address.
Stop FPGA/UIO applications and disable any old accelerator autoload service
before a clean reboot. The loader refuses known existing accelerator modules
or DT nodes. After a failed load, inspect `dmesg` and reboot before retrying.
If you change the base-tree layout, adapt the loader's corresponding checks too.

PYNQ configures the PL clock from the matching HWH; the loader prints the actual
PL0 rate and does not force 150 MHz. The current exported HWH specifies an actual
100 MHz clock despite a requested 150 MHz setting. Use it for functional tests;
correct the Vivado clock configuration and regenerate matching artifacts before
benchmarking at 150 MHz. Merely adding `clocks` to a device node does not set its rate.
The driver enables the named clock and logs the rate without retuning a shared
PS clock. No interrupt wiring is required by this driver.

The driver requests a 640 MiB weight arena by default, reducing the request on
allocation failure down to a minimum of 16 MiB. Its startup log and the app show
the **actual** allocation. This is not a request to change boot-time CMA settings;
available contiguous memory determines the effective cache capacity.

## Board tests, when available

Pin the single-thread application to one allowed core. These examples use core 0:

```sh
# Resident weights where the actual arena permits them:
sudo taskset -c 0 ./build/tiled_gemv_test --rows 4096 --cols 896 --iterations 100 --policy resident

# Same tile sizes, serial staging versus overlapping staging:
sudo taskset -c 0 ./build/tiled_gemv_test --rows 4096 --cols 896 --iterations 100 --cache-mib 0 --stage-kib 1024 --serial
sudo taskset -c 0 ./build/tiled_gemv_test --rows 4096 --cols 896 --iterations 100 --cache-mib 0 --stage-kib 1024

# Partial residency, plus a non-multiple-of-eight final row count:
sudo taskset -c 0 ./build/tiled_gemv_test --rows 4099 --cols 896 --iterations 100 --cache-mib 1 --stage-kib 1024
```

The test always checks warmup and final results against an independent
double-precision reference. Preloading, one warmup execution, and numerical
validation are excluded from execution timing. The report separates activation
copy, staging, submission, exposed WAIT time and output copy. WAIT time is not
hardware execution time: the FPGA may already have executed during the CPU copy.
Effective weight bandwidth is matrix bytes divided by full measured execution
time, not an AXI bus measurement. No tokens/s estimate is produced.

`--policy resident` assigns a partly cached matrix to the scalar CPU reference;
`--policy stream` permits resident plus streamed rows; `--policy cpu` uses no
device at all. `--serial` retains the same tile schedule but moves the next copy
after completion for an A/B comparison.

## Runtime and ownership contract

The application allocates two staging buffers, 4 MiB each by default, at the end
of the selected cache budget. `tg_plan_prepare()` copies the largest fitting row
prefix into the persistent cache, rounded to eight rows when partial. Call it
for all tensors in a deliberate priority order before execution. This v1 exposes
that ordering to the caller; it does not implement an automatic model-wide cache
optimizer or eviction. Original weights stay immutable and alive for streaming.

For a nonresident range:

```text
prepare A -> SUBMIT A -> prepare B -> WAIT A -> copy output A
          -> SUBMIT B -> prepare A -> WAIT B -> copy output B -> ...
```

Only one job is active. Activations are copied once per matrix execution and
remain unchanged across all its tiles. Both engines read the same activation.
Resident ranges require no staging copies after planning. Results use one output
buffer because every result is copied out before the next SUBMIT; output overlap
would require separate output regions and is not part of v1.

The driver uses Linux DMA addresses, coherent allocations/mappings, explicit
memory barriers, exclusive open, checked offsets and monotonic sequence numbers.
Applications must not write active input buffers. Mappings retain the exclusive
file/device reference until unmapped. QUERY latches completion; AP_CTRL's
clear-on-read done bit is owned by the driver.

A WAIT timeout does **not** cancel DMA. The runtime freezes after any execution
failure and never reuses its buffers. A direct ABI client may QUERY/WAIT again to
observe late completion. If an application closes or the device disappears with
unconfirmed DMA, the driver quarantines/retains the buffers rather than returning
them to Linux. A hung accelerator requires board recovery; do not hot-reprogram
the FPGA or remove its overlay while applications or DMA are active.

## Scope and remaining validation

- Provided: HLS source/testbench/build script, asynchronous driver, standalone
  test application, partial residency and ping-pong staging, overlay template.
- Deferred: llama.cpp integration, multi-column GEMM, hardware command queues,
  automatic cache prioritization, asynchronous output copies.
- Host tests cannot establish Linux module compatibility, achieved HLS II,
  routed timing, real DMA coherency or performance. Build the module with the
  target headers and run the provided HLS flow before deployment.

DMA and ABI implementation references:
[Linux 5.15 DMA guide](https://docs.kernel.org/5.15/core-api/dma-api-howto.html),
[Linux 5.15 ioctl guide](https://docs.kernel.org/5.15/driver-api/ioctl.html).

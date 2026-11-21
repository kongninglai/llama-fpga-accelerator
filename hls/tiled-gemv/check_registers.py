#!/usr/bin/env python3
"""Compare the exported HLS register map with the driver contract."""
import pathlib
import re
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: python3 check_registers.py path/to/xtiled_gemv_accel_hw.h")
generated = pathlib.Path(sys.argv[1]).read_text()
contract = pathlib.Path(__file__).with_name("tiled_gemv_regs.h").read_text()
expected = dict(re.findall(r"#define TG_REG_(\w+)\s+(0x[0-9a-fA-F]+)", contract))
found = dict(re.findall(r"#define\s+XTILED_GEMV_ACCEL_CTRL_ADDR_(\w+)\s+(0x[0-9a-fA-F]+)", generated))
errors = []
for name, value in expected.items():
    symbol = "AP_CTRL" if name == "CTRL" else name if name in ("GIE", "IER", "ISR") else name + "_DATA"
    actual = found.get(symbol)
    if actual is None or int(actual, 16) != int(value, 16):
        errors.append(f"{symbol}: expected {value}, found {actual}")
if errors:
    raise SystemExit("Register mismatch:\n" + "\n".join(errors))
print("PASS: exported HLS offsets match tiled_gemv_regs.h")

#!/usr/bin/env python3
"""Board-only startup: program PL, apply DTBO, then insert the kernel driver.
Run as root with the Python interpreter that provides pynq. Run once per boot.
"""
from pathlib import Path
import os
import platform
import shutil
import struct
import subprocess

HERE = Path(__file__).resolve().parent
DT = Path('/sys/firmware/devicetree/base')


def fail(message):
    raise SystemExit(message)


def main():
    if os.geteuid() != 0:
        fail('Run as root, with the Python interpreter that imports pynq.')
    from pynq import Overlay

    for name in ('dotprod.bit', 'dotprod.hwh', 'dotprod.dtbo', 'fpga_drv.ko'):
        if not (HERE / name).is_file():
            fail('Missing file: ' + str(HERE / name))
    if Path('/sys/module/fpga_drv').exists():
        fail('fpga_drv is already loaded. Do not reprogram an active accelerator; reboot for a clean load.')
    for compatible in DT.rglob('compatible'):
        if b'ece382m,dotprod' in compatible.read_bytes().split(b'\0'):
            fail('A dotprod node already exists. Boot the ORIGINAL PYNQ image.ub, with no dotprod overlay applied.')
    old = DT / 'fabric@A0000000'
    if not old.exists() or b'generic-uio' not in (old / 'compatible').read_bytes().split(b'\0'):
        fail('This loader requires the original PYNQ /fabric@A0000000 node.')
    for symbol in ('amba', 'gic', 'zynqmp_clk'):
        if not (DT / '__symbols__' / symbol).is_file():
            fail('Base DT is missing symbol ' + symbol)
    for tool in ('modinfo', 'insmod'):
        if not shutil.which(tool):
            fail('Missing command: ' + tool)
    release = subprocess.check_output(
        ['modinfo', '-F', 'vermagic', str(HERE / 'fpga_drv.ko')], text=True).split()[0]
    if release != platform.release():
        fail('Driver kernel release ' + release + ' differs from running ' + platform.release())
    if not os.path.ismount('/sys/kernel/config'):
        subprocess.run(['mount', '-t', 'configfs', 'configfs', '/sys/kernel/config'], check=True)
    if not Path('/sys/kernel/config/device-tree/overlays').is_dir():
        fail('Kernel configfs device-tree overlay interface is unavailable.')

    # Parse the HWH before unbinding or programming anything.
    overlay = Overlay(str(HERE / 'dotprod.bit'), dtbo=str(HERE / 'dotprod.dtbo'), download=False)
    ip = overlay.ip_dict['dotprod_accel_0']
    if ip['phys_addr'] != 0xa0000000 or ip['addr_range'] != 0x40000:
        fail('HWH address mapping differs from the driver/DTBO.')

    # Stop all FPGA applications before running this script. Unbind stock UIO
    # explicitly so the custom driver can claim both the MMIO region and IRQ.
    for device in Path('/sys/bus/platform/devices').iterdir():
        node = device / 'of_node'
        if node.is_symlink() and node.resolve() == old.resolve():
            driver = device / 'driver'
            if driver.is_symlink():
                print('Unbinding stock device:', device.name, flush=True)
                (driver / 'unbind').write_text(device.name + '\n')

    print('Programming FPGA, then applying dotprod.dtbo...', flush=True)
    overlay.download()
    if not overlay.is_loaded():
        fail('PYNQ did not report the bitstream as loaded.')
    node = DT / 'axi/dotprod_accel@a0000000'
    if not node.exists() or (node / 'reg').read_bytes() != struct.pack('>4I', 0, 0xa0000000, 0, 0x40000):
        fail('Expected live accelerator node is absent or has the wrong address range.')
    print('Loading fpga_drv.ko...', flush=True)
    subprocess.run(['insmod', str(HERE / 'fpga_drv.ko')], check=True)
    if not Path('/dev/fpga').exists():
        fail('Driver did not create /dev/fpga. Check dmesg; reboot before retrying a failed load.')
    print('Ready: /dev/fpga. Run the dot-product test application.', flush=True)


if __name__ == '__main__':
    main()

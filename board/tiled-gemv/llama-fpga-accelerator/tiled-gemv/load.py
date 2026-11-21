#!/usr/bin/env python3
"""Load tiled GEMV once per boot on the original Ultra96 PYNQ image.

Build the kernel module and application separately. Use PYNQ's Python as root.
"""
import argparse
import os
from pathlib import Path
import platform
import shutil
import struct
import subprocess

HERE = Path(__file__).resolve().parent
ASSETS = HERE.parent.parent / 'hls/tiled-gemv/impl'
DT = Path('/sys/firmware/devicetree/base')
MODULES = Path('/sys/module')
DEVICES = Path('/sys/bus/platform/devices')
CONFIGFS = Path('/sys/kernel/config')
DEVICE = Path('/dev/tiled_gemv')
COMPATIBLE = b'ece382m,tiled-gemv-1.0'


def fail(message):
    raise SystemExit(message)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bitstream', type=Path, default=ASSETS / 'tiled_gemv.bit',
                        help='bitstream; matching .hwh must be alongside it')
    parser.add_argument('--dtbo', type=Path, default=ASSETS / 'tiled_gemv.dtbo')
    parser.add_argument('--module', type=Path, default=HERE / 'tiled_gemv_drv.ko')
    parser.add_argument('--arena-mb', type=int, default=640)
    parser.add_argument('--engines', type=int, choices=(1, 2), default=2)
    args = parser.parse_args(argv)
    if not 16 <= args.arena_mb <= 1024:
        parser.error('--arena-mb must be between 16 and 1024')
    if os.geteuid() != 0:
        fail('Run as root using /usr/local/share/pynq-venv/bin/python3.')
    args.bitstream = args.bitstream.resolve()
    args.dtbo = args.dtbo.resolve()
    args.module = args.module.resolve()
    for path in (args.bitstream, args.bitstream.with_suffix('.hwh'), args.dtbo, args.module):
        if not path.is_file():
            fail('Missing file: ' + str(path) + '\nBuild/copy the artifacts first; this script does not compile them.')
    for name in ('fpga_drv', 'gemv_drv', 'tiled_gemv_drv'):
        if (MODULES / name).exists():
            fail(name + ' is already loaded. Reboot with accelerator autoload disabled before running this loader.')
    if DEVICE.exists():
        fail(str(DEVICE) + ' already exists; use a clean boot.')
    for compatible in DT.rglob('compatible'):
        values = compatible.read_bytes().split(b'\0')
        if any(value.startswith((b'ece382m,dotprod', b'ece382m,gemv', b'ece382m,tiled-gemv'))
               for value in values):
            fail('An accelerator DT node already exists: ' + str(compatible.parent) +
                 '\nBoot the original PYNQ image with accelerator autoload disabled.')
    old = DT / 'fabric@A0000000'
    if not (old / 'compatible').is_file() or b'generic-uio' not in (old / 'compatible').read_bytes().split(b'\0'):
        fail('This loader/DTBO requires the original PYNQ /fabric@A0000000 generic-uio node.')
    for symbol in ('amba', 'zynqmp_clk'):
        if not (DT / '__symbols__' / symbol).is_file():
            fail('Base device tree is missing symbol: ' + symbol)
    amba = (DT / '__symbols__/amba').read_bytes().rstrip(b'\0').decode()
    node = DT / amba.lstrip('/') / 'tiled_gemv@a0000000'
    for command in ('modinfo', 'insmod'):
        if not shutil.which(command):
            fail('Missing command: ' + command)
    vermagic = subprocess.check_output(['modinfo', '-F', 'vermagic', str(args.module)], text=True).split()
    if not vermagic or vermagic[0] != platform.release():
        fail('Module vermagic does not match running kernel ' + platform.release() +
             '. Rebuild using its prepared kernel headers.')
    try:
        from pynq import Overlay, Clocks
    except ImportError:
        fail('PYNQ import failed. Use sudo /usr/local/share/pynq-venv/bin/python3 load.py.')

    # Parse metadata before changing the hardware or unbinding stock UIO.
    overlay = Overlay(str(args.bitstream), dtbo=str(args.dtbo), download=False)
    ip = overlay.ip_dict.get('tiled_gemv_accel_0', {})
    if ip.get('phys_addr') != 0xa0000000 or ip.get('addr_range') != 0x40000:
        fail('HWH must contain tiled_gemv_accel_0 at 0xA0000000 with a 256 KiB range.')
    unbind = []
    for device in DEVICES.iterdir():
        of_node = device / 'of_node'
        driver = device / 'driver'
        if of_node.is_symlink() and of_node.resolve() == old.resolve() and driver.is_symlink():
            if driver.resolve().name != 'uio_pdrv_genirq':
                fail('Unexpected driver on stock fabric device: ' + str(driver.resolve()))
            unbind.append((driver / 'unbind', device.name))
    if not os.path.ismount(str(CONFIGFS)):
        if not shutil.which('mount'):
            fail('Missing command: mount')
        subprocess.run(['mount', '-t', 'configfs', 'configfs', str(CONFIGFS)], check=True)
    if not (CONFIGFS / 'device-tree/overlays').is_dir():
        fail('Kernel configfs device-tree overlay interface is unavailable.')

    print('Stop all FPGA/UIO applications before loading. This loader is for a clean boot.', flush=True)
    try:
        for path, name in unbind:
            print('Unbinding stock UIO device:', name, flush=True)
            path.write_text(name + '\n')
        print('Programming', args.bitstream, 'and applying', args.dtbo, flush=True)
        overlay.download()
        if not overlay.is_loaded():
            fail('PYNQ did not report the bitstream as loaded.')
        if (not (node / 'reg').is_file() or
                (node / 'reg').read_bytes() != struct.pack('>4I', 0, 0xa0000000, 0, 0x40000) or
                not (node / 'compatible').is_file() or
                COMPATIBLE not in (node / 'compatible').read_bytes().split(b'\0')):
            fail('Expected live tiled GEMV node is absent or has incorrect registers/compatible.')
        if (node / 'status').exists() and (node / 'status').read_bytes().rstrip(b'\0') not in (b'ok', b'okay'):
            fail('The live tiled GEMV device tree node is disabled.')
        if not (old / 'status').is_file() or (old / 'status').read_bytes().rstrip(b'\0') != b'disabled':
            fail('DTBO did not disable the stock fabric node.')
        print('PL0 clock: %.3f MHz (configured from HWH)' % Clocks.fclk0_mhz, flush=True)
        subprocess.run(['insmod', str(args.module), 'arena_mb=' + str(args.arena_mb),
                        'engines=' + str(args.engines)], check=True)
        if not DEVICE.exists():
            fail('Driver did not create ' + str(DEVICE) + '. Inspect sudo dmesg | tail -40.')
    except (Exception, SystemExit):
        print('Load incomplete: inspect dmesg. Reboot before retrying; no automatic hardware rollback was attempted.', flush=True)
        raise
    print('Ready: ' + str(DEVICE) + '. Run the manually built build/tiled_gemv_test.', flush=True)


if __name__ == '__main__':
    main()

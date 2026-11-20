#!/usr/bin/env python3
"""Prepare bit/HWH/DTBO and check overlay resolution against the original FIT."""
from pathlib import Path
import hashlib
import importlib.util
import json
import struct
import subprocess
import tempfile
import zipfile
import xml.etree.ElementTree as ET

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]


def main():
    spec = importlib.util.spec_from_file_location('fit_reader', HERE.parent / 'repack_dotprod.py')
    reader = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reader)
    fit = reader.parse((HERE.parent / 'image.ub').read_bytes())
    base = fit['/images/fdt-0']['data']
    assert hashlib.sha1(base).digest() == fit['/images/fdt-0/hash-1']['value']
    base_nodes = reader.parse(base)
    assert base_nodes['/fabric@A0000000']['compatible'] == b'generic-uio\0'

    xsa = REPO / 'hls/impl/Ultra96_Design_wrapper.xsa'
    with zipfile.ZipFile(xsa) as archive:
        hwh = archive.read('Ultra96_Design.hwh')
        root = ET.fromstring(hwh)
        maps = [m for m in root.findall('.//MEMRANGE')
                if m.get('INSTANCE') == 'dotprod_accel_0']
        assert maps and all(int(m.get('BASEVALUE'), 0) == 0xa0000000 and
                            int(m.get('HIGHVALUE'), 0) == 0xa003ffff for m in maps)
        (HERE / 'dotprod.bit').write_bytes(archive.read('Ultra96_Design_wrapper.bit'))
        (HERE / 'dotprod.hwh').write_bytes(hwh)

    subprocess.run(['dtc', '-@', '-I', 'dts', '-O', 'dtb', '-o',
                    str(HERE / 'dotprod.dtbo'), str(HERE / 'dotprod.dtso')], check=True)
    with tempfile.TemporaryDirectory(prefix='dotprod-overlay-') as tmp:
        original, merged = Path(tmp) / 'base.dtb', Path(tmp) / 'merged.dtb'
        original.write_bytes(base)
        subprocess.run(['fdtoverlay', '-i', str(original), '-o', str(merged),
                        str(HERE / 'dotprod.dtbo')], check=True)
        nodes = reader.parse(merged.read_bytes())
        ip = nodes['/axi/dotprod_accel@a0000000']
        assert ip['compatible'] == b'ece382m,dotprod\0'
        assert ip['reg'] == struct.pack('>4I', 0, 0xa0000000, 0, 0x40000)
        assert ip['interrupts'] == struct.pack('>3I', 0, 89, 4)
        assert ip['interrupt-parent'] == nodes['/axi/interrupt-controller@f9010000']['phandle']
        assert ip['clocks'] == nodes['/firmware/zynqmp-firmware/clock-controller']['phandle'] + struct.pack('>I', 71)
        assert nodes['/fabric@A0000000']['status'] == b'disabled\0'
        # The old UIO status and symbol-table addition are the only changes to
        # existing nodes. The base DT is not replaced or installed on the board.
        for path, props in base_nodes.items():
            expected = dict(props)
            if path == '/fabric@A0000000':
                expected['status'] = b'disabled\0'
            if path == '/__symbols__':
                expected['dotprod_accel_0'] = b'/axi/dotprod_accel@a0000000\0'
            assert nodes[path] == expected, 'Unexpected base-tree change: ' + path
    report = {
        'base_fit': '../image.ub (original PYNQ)',
        'offline_overlay_merge': 'PASS',
        'register_and_irq_check': 'PASS',
        'clock_and_gic_fixups': 'PASS',
        'generic_uio_disabled': True,
        'board_tested': False,
        'sha256': {name: hashlib.sha256((HERE / name).read_bytes()).hexdigest()
                   for name in ('dotprod.bit', 'dotprod.hwh', 'dotprod.dtbo')},
    }
    (HERE / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Rebuild this unsigned PYNQ FIT, preserving its kernel and board DT settings.
Requires dtc, fdtget, fdtput. The original image.ub is never modified.
"""
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess

HERE = Path(__file__).resolve().parent
SOURCE = HERE / 'image.ub'
OUT = HERE / 'dotprod-fit'


def parse(blob):
    header = struct.unpack_from('>10I', blob)
    assert header[0] == 0xd00dfeed
    pos = header[2]
    strings = blob[header[3]:header[3] + header[8]]
    stack, nodes = [], {}
    while True:
        token, = struct.unpack_from('>I', blob, pos)
        pos += 4
        if token == 1:
            end = blob.index(0, pos)
            stack.append(blob[pos:end].decode())
            pos = (end + 4) & ~3
            nodes['/'.join(stack) or '/'] = {}
        elif token == 2:
            stack.pop()
        elif token == 3:
            size, offset = struct.unpack_from('>II', blob, pos)
            pos += 8
            name = strings[offset:strings.index(0, offset)].decode()
            nodes['/'.join(stack) or '/'][name] = blob[pos:pos + size]
            pos = (pos + size + 3) & ~3
        elif token == 4:
            pass
        elif token == 9:
            return nodes
        else:
            raise ValueError('Unsupported FDT token')


def run(*args):
    return subprocess.check_output(args, text=True).strip()


def main():
    for tool in ('dtc', 'fdtget', 'fdtput'):
        if not shutil.which(tool):
            raise SystemExit('Missing tool: ' + tool)
    original = SOURCE.read_bytes()
    fit = parse(original)
    # This script deliberately accepts only the inspected, inline unsigned FIT.
    assert set(p for p in fit if p.count('/') == 2 and p.startswith('/images/')) == {
        '/images/kernel-0', '/images/fdt-0'}
    assert not any('signature' in p.split('/')[-1] for p in fit)
    assert fit['/configurations/conf-1']['kernel'] == b'kernel-0\0'
    assert fit['/configurations/conf-1']['fdt'] == b'fdt-0\0'
    for name in ('kernel-0', 'fdt-0'):
        payload = fit['/images/' + name]['data']
        hp = fit['/images/' + name + '/hash-1']
        assert hp['algo'] == b'sha1\0'
        assert hp['value'] == hashlib.sha1(payload).digest(), 'Original hash failed'
    OUT.mkdir(exist_ok=True)
    kernel = fit['/images/kernel-0']['data']
    old_dtb = fit['/images/fdt-0']['data']
    (OUT / 'kernel.bin').write_bytes(kernel)
    (OUT / 'original.dtb').write_bytes(old_dtb)
    new_dtb = OUT / 'dotprod.dtb'
    new_dtb.write_bytes(old_dtb)
    old_nodes = parse(old_dtb)
    old_node = '/fabric@A0000000'
    assert old_nodes[old_node]['compatible'] == b'generic-uio\0'
    assert old_nodes[old_node]['reg'] == struct.pack('>4I', 0, 0xa0000000, 0, 0x10000)
    assert not any(old_node.encode() in v for v in old_nodes.get('/__symbols__', {}).values())
    gic = run('fdtget', '-t', 'x', str(new_dtb), '/axi/interrupt-controller@f9010000', 'phandle')
    clk = run('fdtget', '-t', 'x', str(new_dtb), '/firmware/zynqmp-firmware/clock-controller', 'phandle')
    run('fdtput', '-r', str(new_dtb), old_node)
    node = '/dotprod_accel@a0000000'
    run('fdtput', '-c', str(new_dtb), node)
    for prop, val in {
        'compatible': 'ece382m,dotprod', 'status': 'okay',
        'clock-names': 'ap_clk', 'interrupt-names': 'interrupt',
    }.items():
        run('fdtput', '-t', 's', str(new_dtb), node, prop, val)
    for prop, vals in {
        'reg': ['0', 'a0000000', '0', '40000'],
        'interrupt-parent': [gic], 'interrupts': ['0', '59', '4'],
        'clocks': [clk, '47'],
        'xlnx,s-axi-ctrl-addr-width': ['12'],
        'xlnx,s-axi-ctrl-data-width': ['20'],
    }.items():
        run('fdtput', '-t', 'x', str(new_dtb), node, prop, *vals)
    new_bytes = new_dtb.read_bytes()
    new_nodes = parse(new_bytes)
    assert {p: v for p, v in old_nodes.items() if p != old_node} == {
        p: v for p, v in new_nodes.items() if p != node}, 'Unrelated DT properties changed'
    fit['/images/fdt-0']['data'] = new_bytes
    fit['/images/fdt-0/hash-1']['value'] = hashlib.sha1(new_bytes).digest()

    # FIT is an FDT container. For this unsigned FIT we explicitly supply its
    # payload hashes, so dtc can serialize it without mkimage. Preserve every
    # other FIT property (including original timestamp and load/entry values).
    def emit(path, indent=''):
        name = '/' if path == '/' else path.rsplit('/', 1)[1]
        lines = [indent + name + ' {']
        for key, value in fit[path].items():
            if key == 'data':
                filename = 'kernel.bin' if path == '/images/kernel-0' else 'dotprod.dtb'
                lines.append(indent + '  ' + key + ' = /incbin/("' + filename + '");')
            elif not value:
                lines.append(indent + '  ' + key + ';')
            else:
                lines.append(indent + '  ' + key + ' = [' + value.hex(' ') + '];')
        for child in fit:
            if child != '/' and (child.rsplit('/', 1)[0] or '/') == path:
                lines.extend(emit(child, indent + '  '))
        lines.append(indent + '};')
        return lines
    its = OUT / 'image-dotprod.its'
    its.write_text('/dts-v1/;\n' + '\n'.join(emit('/')) + '\n')
    result = OUT / 'image-dotprod.ub'
    run('dtc', '-I', 'dts', '-O', 'dtb', '-o', str(result), str(its))
    actual = parse(result.read_bytes())
    assert actual == fit, 'Rebuilt FIT differs from intended tree'
    assert actual['/images/kernel-0']['data'] == kernel
    for name in ('kernel-0', 'fdt-0'):
        data = actual['/images/' + name]['data']
        assert hashlib.sha1(data).digest() == actual['/images/' + name + '/hash-1']['value']
    assert SOURCE.read_bytes() == original
    report = {
        'original_sha256': hashlib.sha256(original).hexdigest(),
        'repacked_sha256': hashlib.sha256(result.read_bytes()).hexdigest(),
        'kernel_sha256_unchanged': hashlib.sha256(kernel).hexdigest(),
        'device_tree_changed_only': [old_node, node],
        'fit_payload_hashes': 'PASS',
        'original_image_unchanged': True,
        'board_boot_tested': False,
        'bitstream_included': False,
    }
    (OUT / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    print('Created:', result)


if __name__ == '__main__':
    main()

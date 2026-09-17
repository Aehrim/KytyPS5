#!/usr/bin/env python3
"""Turns kyty_samples.txt (KYTY_SAMPLER=1, F4) into self/inclusive time tables.

usage: sampler_report.py kyty_samples.txt path/to/kyty_emulator.exe [llvm-symbolizer] [top]
"""
import collections
import re
import subprocess
import sys

IMAGE_BASE = 0x140000000


def main():
    samples_path, exe = sys.argv[1], sys.argv[2]
    symbolizer = sys.argv[3] if len(sys.argv) > 3 else 'llvm-symbolizer'
    top = int(sys.argv[4]) if len(sys.argv) > 4 else 45
    lines = open(samples_path).read().split('\n')
    base = int(lines[0].split()[1], 16)
    stacks = [[int(a, 16) for a in line.split()] for line in lines[1:] if line.strip()]
    addresses = sorted({a for stack in stacks for a in stack})
    inside = [a for a in addresses if base <= a < base + (1 << 31)]
    query = '\n'.join('0x%x' % (a - base + IMAGE_BASE) for a in inside) + '\n'
    out = subprocess.run([symbolizer, '--obj=' + exe, '--no-inlines'], input=query,
                         capture_output=True, text=True).stdout
    blocks = [b for b in out.split('\n\n') if b.strip()]
    names = {}
    for address, block in zip(inside, blocks):
        name = block.strip().split('\n')[0]
        name = re.sub(r'\(.*', '', name)
        names[address] = re.sub(r'^(Libs::Graphics::|Libs::)', '', name)[:100]
    self_time = collections.Counter()
    inclusive = collections.Counter()
    outside = '<outside>'
    for stack in stacks:
        resolved = [names.get(a, outside) for a in stack]
        leaf = resolved[0]
        if leaf == outside:
            # Time in the driver, the C runtime or the kernel: name the function that called out.
            caller = next((name for name in resolved if name != outside), '?')
            leaf = '%s  [in driver/system]' % caller
        self_time[leaf] += 1
        for name in set(resolved):
            inclusive[name] += 1
    total = len(stacks)
    print('samples: %d' % total)
    for title, table in (('SELF', self_time), ('INCLUSIVE', inclusive)):
        print('\n== %s' % title)
        for name, count in table.most_common(top):
            print('%6.2f%%  %s' % (100.0 * count / total, name))


main()

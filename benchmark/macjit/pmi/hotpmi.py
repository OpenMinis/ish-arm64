#!/usr/bin/env python3
"""Disassemble the generated code that PMI samples land in, hottest segments first.

    hotpmi.py <samples.ktrace> <ISH_JIT_MAP file> <ISH_JIT_DUMP file> [--segments 6] [--comm NAME]

Like hotcode.py, but from sample.sh's INST_ALL samples: each count is a share
of the retired instructions, not of the time. Each instruction is shown with
its role (ISH_JIT_MAP R lines).
"""
import argparse
import bisect
import csv
import struct
import subprocess
from collections import Counter

import capstone


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ktrace')
    ap.add_argument('map')
    ap.add_argument('dump')
    ap.add_argument('--segments', type=int, default=6)
    ap.add_argument('--comm', default='')
    args = ap.parse_args()

    segs, roles, mods = [], {}, {}
    for line in open(args.map, errors='replace'):
        p = line.split()
        if p and p[0] == 'M':
            mods[int(p[1])] = p[-1].rsplit('/', 1)[-1]
        elif p and p[0] == 'S':
            segs.append((int(p[1], 16), int(p[2]), int(p[3]), int(p[4], 16), int(p[5], 16)))
        elif p and p[0] == 'R':
            roles[int(p[1], 16)] = p[2]
    segs.sort()
    starts = [s[0] for s in segs]
    raw = open(args.dump, 'rb').read()
    base, used = struct.unpack('<QQ', raw[:16])
    code = raw[16:16 + used]

    rows = subprocess.run(['ktrace', 'trace', '-R', args.ktrace, '--csv'], capture_output=True, text=True).stdout
    pcs = Counter()
    total = 0
    for r in csv.reader(rows.splitlines()[1:]):
        if r[2] != 'PERF_KPC_Handler' or (args.comm and r[11] != args.comm):
            continue
        total += 1
        pcs[int(r[6])] += 1
    per_seg = Counter()
    for pc, n in pcs.items():
        i = bisect.bisect_right(starts, pc) - 1
        if i >= 0 and pc < segs[i][0] + 4 * segs[i][1]:
            per_seg[i] += n
    print(f'{total} samples, {sum(per_seg.values())} in segments')
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    for i, n in per_seg.most_common(args.segments):
        c, words, mid, off, gpc = segs[i]
        r = roles.get(c, '')
        print(f'\n=== segment {c:#x} {words} words, {mods.get(mid, mid)}+{off:#x} guest {gpc:#x}: {n} samples ({100 * n / total:.1f}%)')
        for k, ins in enumerate(md.disasm(code[c - base:c - base + 4 * words], c)):
            s = pcs.get(ins.address, 0)
            print(f'{s:6d} {r[k] if k < len(r) else "?"} {ins.address:#x}: {ins.mnemonic} {ins.op_str}')


if __name__ == '__main__':
    main()

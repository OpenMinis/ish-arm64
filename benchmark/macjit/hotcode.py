#!/usr/bin/env python3
"""Show where the samples in generated code land, instruction by instruction.

    hotcode.py <sample.txt> <ISH_JIT_MAP file> <ISH_JIT_DUMP file> [--segments 8] [--kinds]

The leaf "???" frames of a macOS `sample` report are looked up in the code map
(S lines: native code, words, module, offset, guest pc) and the region dump
([u64 base][u64 bytes][code]), then the hottest segments are disassembled with
the samples of each instruction next to it. --kinds sums the samples by what
the instruction belongs to (a guess from the instruction stream).
"""
import argparse
import bisect
import re
import struct
from collections import Counter

import capstone

LEAF = re.compile(r'^[ +!:|]*(\d+) \?\?\?  \(in <unknown binary>\)  \[(0x[0-9a-f]+)\]')


ROLES = {'g': 'guest instructions', 't': 'TLB lookup', 's': 'shadow window check', 'r': 'unpinned register load/store',
         'p': 'cold entry', 'd': 'block end: direct successor', 'R': 'block end: ret',
         'I': 'block end: br / blr', 'x': 'bail-out stubs'}


def load_map(path):
    mods, segs, roles = {}, [], {}
    for line in open(path):
        p = line.split()
        if p and p[0] == 'M':
            mods[int(p[1])] = p[-1].rsplit('/', 1)[-1]
        elif p and p[0] == 'S':
            code, words, mid = int(p[1], 16), int(p[2]), int(p[3])
            segs.append((code, words, mid, int(p[4], 16), int(p[5], 16)))
        elif p and p[0] == 'R':
            roles[int(p[1], 16)] = p[2]
    segs.sort()
    return mods, segs, roles


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('sample')
    ap.add_argument('map')
    ap.add_argument('dump')
    ap.add_argument('--segments', type=int, default=8)
    args = ap.parse_args()

    hits = Counter()
    for line in open(args.sample, errors='replace'):
        if line.startswith('Sort by top of stack'):
            break
        m = LEAF.match(line)
        if m:
            hits[int(m.group(2), 16)] += int(m.group(1))
    mods, segs, roles = load_map(args.map)
    starts = [s[0] for s in segs]
    raw = open(args.dump, 'rb').read()
    base, used = struct.unpack('<QQ', raw[:16])
    code = raw[16:16 + used]

    per_seg, outside = Counter(), 0
    for a, n in hits.items():
        i = bisect.bisect_right(starts, a) - 1
        if i >= 0 and a < segs[i][0] + 4 * segs[i][1]:
            per_seg[i] += n
        else:
            outside += n
    total = sum(hits.values())
    print(f'{total} samples in generated code, {outside} outside any segment (stubs, unmapped)')
    by_role = Counter()
    for a, n in hits.items():
        i = bisect.bisect_right(starts, a) - 1
        if i < 0 or a >= segs[i][0] + 4 * segs[i][1]:
            by_role['outside segments (shared stubs)'] += n
            continue
        r = roles.get(segs[i][0])
        k = (a - segs[i][0]) // 4
        by_role[ROLES.get(r[k], r[k]) if r and k < len(r) else '?'] += n
    for r, n in by_role.most_common():
        print(f'  {r:32} {n:7} {100 * n / total:5.1f}%')
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    for i, n in per_seg.most_common(args.segments):
        start, words, mid, off, pc = segs[i]
        print(f'\n=== {n} samples ({100 * n / total:.1f}%)  {mods.get(mid, mid)}+{off:#x}  guest pc {pc:#x}  '
              f'{words} words at {start:#x}')
        lo = start - base
        r = roles.get(start, '')
        for ins in md.disasm(code[lo:lo + 4 * words], start):
            h = hits.get(ins.address, 0)
            mark = f'{h:5}' if h else '     '
            k = (ins.address - start) // 4
            print(f'  {mark} {r[k] if k < len(r) else " "} {ins.address:#x}  {ins.mnemonic:8} {ins.op_str}')
    print('\nsamples outside segments by address (top 10):')
    out = Counter({a: n for a, n in hits.items()
                   if not (0 <= bisect.bisect_right(starts, a) - 1 < len(segs))
                   or a >= segs[bisect.bisect_right(starts, a) - 1][0] + 4 * segs[bisect.bisect_right(starts, a) - 1][1]})
    for a, n in out.most_common(10):
        print(f'  {n:5}  {a:#x}  (region +{a - base:#x})')


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Sum up ISH_XLAT_SITEDUMP files (asbestos/guest-x86_64/xlat.c): how the translated code's memory
accesses check their pages.

  ISH_XLAT_SITEDUMP=C:/tmp/sd.txt ish.exe -f ROOTFS ... ; sitedump.py sd.txt

A line per access site that ran: ip kind base index disp size write lead checks lookups. A check is
the site comparing the address with the page it found last time; a lookup is that failing (another
page, or the first access after a TLB flush) and the TLB being asked. Prints the share of checks
and the miss rate by what the operand is based on, and how the checks are spread over sites by
miss rate: mostly a few sites that nearly always miss, and many that never do.
"""
import collections, sys

NAMES = ['rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi', 'r8', 'r9', 'r10', 'r11', 'r12', 'r13', 'r14', 'r15', 'rip']


def main():
    for path in sys.argv[1:]:
        rows = []
        for line in open(path):
            p = line.split()
            if len(p) < 10:
                continue
            rows.append({'base': int(p[2]), 'index': int(p[3]), 'checks': int(p[8]), 'lookups': int(p[9])})
        checks = sum(r['checks'] for r in rows)
        lookups = sum(r['lookups'] for r in rows)
        if not checks:
            continue
        print(f'📄 {path}: {len(rows)} sites, {checks} checks, {lookups} lookups ({100 * lookups / checks:.1f}%)')
        by = collections.defaultdict(lambda: [0, 0, 0])
        for r in rows:
            k = NAMES[r['base']] if 0 <= r['base'] <= 16 else 'none'
            k += '+index' if r['index'] >= 0 else ''
            b = by[k]
            b[0] += r['checks']; b[1] += r['lookups']; b[2] += 1
        print(f'  {"base":12} {"sites":>7} {"checks":>8} {"miss rate":>10} {"of lookups":>11}')
        for k, b in sorted(by.items(), key=lambda kv: -kv[1][0])[:14]:
            print(f'  {k:12} {b[2]:7} {100 * b[0] / checks:7.1f}% {100 * b[1] / max(b[0], 1):9.1f}% {100 * b[1] / max(lookups, 1):10.1f}%')
        buckets = [('under 1%', 0.01), ('1-5%', 0.05), ('5-20%', 0.2), ('20-50%', 0.5), ('over 50%', 2.0)]
        spread = collections.defaultdict(lambda: [0, 0, 0])
        for r in rows:
            rate = r['lookups'] / r['checks']
            s = spread[next(n for n, hi in buckets if rate < hi)]
            s[0] += r['checks']; s[1] += r['lookups']; s[2] += 1
        print(f'  {"miss rate":12} {"sites":>7} {"checks":>8} {"of lookups":>11}')
        for n, _ in buckets:
            s = spread[n]
            print(f'  {n:12} {s[2]:7} {100 * s[0] / checks:7.1f}% {100 * s[1] / max(lookups, 1):10.1f}%')


if __name__ == '__main__':
    main()

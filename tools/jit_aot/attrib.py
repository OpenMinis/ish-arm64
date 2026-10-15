#!/usr/bin/env python3
"""Attribute the CPU time of one ish process to guest modules.

Input: a macOS `sample <pid> <seconds> 1 -file <sample.txt>` of an ish run with
ISH_JIT_MAP=<map.txt> (the map is written when ish exits). Native code samples are looked up in
the map's segments (S lines) and counted for their module (M lines); the rest is split into
gadgets, ish's translation work, the ish kernel/runtime and host libraries. Threads that wait
(condition variables, sleeps, kevent/poll, blocking reads) are counted apart and left out of the
percentages. Without a map (a pure-AOT build), AOT code shows as "native (unmapped)".

usage: attrib.py <sample.txt> [map.txt] [--min 0.3] [--top-ish 12]
"""
import argparse
import bisect
import re
from collections import Counter

# Leaf symbols of waiting threads.
IDLE = ('__psynch_cvwait', '__psynch_mutexwait', '__semwait_signal', 'kevent', 'wait4', '__select',
        '__workq_kernreturn', 'mach_msg', '__recvfrom', '__read_nocancel', 'nanosleep', '__ulock_wait')
IDLE_IO = re.compile(r'^(read|poll|recvfrom) +\(in libsystem_kernel')     # blocking reads and polls
TRANSLATE = re.compile(r'^(gen_|gen\b|jit_translate|jit_compile|jit_emit|emit_|jit_install|translate|'
                       r'fiber_block_new|fiber_compile|jit_block_new|decode_|jit_segment|install_code|'
                       r'region_alloc|reg_|aot_)')
LINE = re.compile(r'^([\s+!:|]*)(\d+)\s+(.*?)(?:\s+\[(0x[0-9a-f]+)(?:,[^\]]*)?\])?\s*$')


def load_map(path):
    mods, segs = {}, []
    for line in open(path):
        p = line.split()
        if p and p[0] == 'M':
            mods[int(p[1])] = p[6].rsplit('/', 1)[-1]
        elif p and p[0] == 'S':
            code, words, mid = int(p[1], 16), int(p[2]), int(p[3])
            segs.append((code, code + 4 * words, mid))
    segs.sort()
    return mods, segs


def call_graph(path):
    """(depth, count, symbol, address, children's count) for every call graph line."""
    nodes, stack, inside = [], [], False
    for line in open(path):
        if line.startswith('Call graph:'):
            inside = True
            continue
        if inside and (line.startswith('Total number') or line.startswith('Sort by')):
            break
        m = LINE.match(line.rstrip('\n')) if inside else None
        if not m:
            continue
        node = [len(m.group(1)), int(m.group(2)), m.group(3), m.group(4), 0]
        while stack and stack[-1][0] >= node[0]:
            stack.pop()
        if stack:
            stack[-1][4] += node[1]
        stack.append(node)
        nodes.append(node)
    return nodes


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('sample')
    ap.add_argument('map', nargs='?')
    ap.add_argument('--min', type=float, default=0.3, help='hide categories below this share (%%)')
    ap.add_argument('--top-ish', type=int, default=0, help='also list the N hottest ish functions')
    args = ap.parse_args()

    mods, segs = load_map(args.map) if args.map else ({}, [])
    starts = [s[0] for s in segs]

    def module_of(addr):
        i = bisect.bisect_right(starts, addr) - 1
        if i >= 0 and segs[i][0] <= addr < segs[i][1]:
            return mods.get(segs[i][2], '?')
        return None

    cat, ish_syms, idle = Counter(), Counter(), 0
    for _, count, sym, addr, child in call_graph(args.sample):
        own = count - child
        if own <= 0:
            continue
        if any(k in sym for k in IDLE) or IDLE_IO.match(sym):
            idle += own
            continue
        if sym.startswith('???') and addr:
            cat['JIT/AOT ' + (module_of(int(addr, 16)) or 'native (unmapped)')] += own
            continue
        if '(in ish)' not in sym:
            cat['host: ' + re.sub(r'.*\(in ([^)]*)\).*', r'\1', sym)] += own
            continue
        name = sym.split(' ')[0]
        ish_syms[name] += own
        if name.startswith(('gadget_', 'fiber_ret', 'fiber_exit')):
            cat['gadgets'] += own
        elif TRANSLATE.match(name):
            cat['ish: translation / image lookup'] += own
        else:
            cat['ish: kernel, syscalls, runtime'] += own

    busy = sum(cat.values())
    if not busy:
        print('no busy samples')
        return
    print(f'busy samples {busy} (waiting {idle})')
    for k, v in cat.most_common():
        if 100 * v / busy >= args.min:
            print(f'  {100 * v / busy:5.1f}%  {v:7d}  {k}')
    if args.top_ish:
        print('  hottest ish functions:', ', '.join(f'{k} {v}' for k, v in ish_syms.most_common(args.top_ish)))


if __name__ == '__main__':
    main()

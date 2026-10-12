#!/usr/bin/env python3
"""Sum up an ISH_SYSCALL_TIMES file (kernel/sctimes.c): where guest threads spend their time.

  ISH_SYSCALL_TIMES=t.txt ish.exe -f ROOTFS ... ; sctimes.py t.txt [--top 15]

For every guest thread: alive (wall from its first syscall to its exit), cpu (host thread CPU),
and per syscall the calls, wall and CPU inside it. Wall inside a syscall minus its CPU is
waiting in the kernel; CPU outside syscalls is the guest computing (translated code, faults);
the rest of a thread's life (off-cpu) is neither: waiting for a host CPU, or on a lock inside
ish (memory map, translation) while not in a syscall.
Threads run in parallel, so totals over threads can exceed the command's wall time: the main
threads (tid == pid) are listed on their own, they are the critical path.
"""
import argparse, collections, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))


def syscall_names():
    names = {}
    path = os.path.join(HERE, '..', '..', 'kernel', 'arch', 'x86_64', 'calls.c')
    for line in open(path, encoding='utf-8'):
        m = re.match(r'\s*\[(\d+)\]\s*=\s*\(syscall_t\)\s*(\w+)\s*,\s*(?://\s*(\w+))?', line)
        if m:
            names[int(m.group(1))] = m.group(3) or m.group(2).replace('sys_', '')
    # the spans of kernel/sctimes.h, nested in (or outside) the syscalls
    names.update({500: '~fakefs lock', 501: '~db begin', 502: '~db commit', 503: '~host open', 504: '~mem wrlock',
                  505: '~jit compile', 506: '~jit promote', 507: '~jit xlat'})
    return names


def parse(path):
    threads = []
    for line in open(path, encoding='utf-8', errors='replace'):
        if not line.startswith('T '):
            continue
        head, _, rest = line.partition('|')
        p = head.split()
        if len(p) < 6:
            continue
        # a program can rename itself to something with spaces ("npm install x")
        t = {'tid': int(p[1]), 'pid': int(p[2]), 'comm': '_'.join(p[3:-2]), 'alive': int(p[-2]), 'cpu': int(p[-1]), 'sc': {}}
        for item in rest.split():
            nr, calls, wall, cpu = (int(x) for x in item.split(':'))
            t['sc'][nr] = (calls, wall, cpu)
        threads.append(t)
    return threads


def ms(us):
    return f'{us / 1000:9.0f}'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file')
    ap.add_argument('--top', type=int, default=15)
    a = ap.parse_args()
    names = syscall_names()
    threads = parse(a.file)
    if not threads:
        return print('no threads in', a.file)

    def split(t):
        sc_wall = sum(v[1] for v in t['sc'].values())
        sc_cpu = sum(v[2] for v in t['sc'].values())
        return sc_wall, sc_cpu, max(t['cpu'] - sc_cpu, 0), max(sc_wall - sc_cpu, 0)

    print(f'{len(threads)} threads in {len(set(t["pid"] for t in threads))} processes\n')
    print('per program (all its threads), ms:')
    print(f'{"program":16} {"threads":>7} {"alive":>9} {"cpu":>9} {"guest cpu":>9} {"sys cpu":>9} {"sys wait":>9} {"off-cpu":>9}')
    by = collections.defaultdict(lambda: [0, 0, 0, 0, 0, 0, 0])
    for t in threads:
        sc_wall, sc_cpu, guest, wait = split(t)
        b = by[t['comm']]
        b[0] += 1; b[1] += t['alive']; b[2] += t['cpu']; b[3] += guest; b[4] += sc_cpu; b[5] += wait
        b[6] += max(t['alive'] - t['cpu'] - wait, 0)
    for comm, b in sorted(by.items(), key=lambda kv: -kv[1][1])[:a.top]:
        print(f'{comm:16} {b[0]:7} {ms(b[1])} {ms(b[2])} {ms(b[3])} {ms(b[4])} {ms(b[5])} {ms(b[6])}')

    def table(title, ths):
        agg = collections.defaultdict(lambda: [0, 0, 0])
        for t in ths:
            for nr, (calls, wall, cpu) in t['sc'].items():
                g = agg[nr]; g[0] += calls; g[1] += wall; g[2] += cpu
        print(f'\n{title}, ms:')
        print(f'{"syscall":18} {"calls":>8} {"wall":>9} {"cpu":>9} {"wait":>9}')
        for nr, (calls, wall, cpu) in sorted(agg.items(), key=lambda kv: -kv[1][1])[:a.top]:
            print(f'{names.get(nr, str(nr)):18} {calls:8} {ms(wall)} {ms(cpu)} {ms(max(wall - cpu, 0))}')

    mains = [t for t in threads if t['tid'] == t['pid']]
    print('\nmain threads (tid == pid), ms:')
    print(f'{"tid":>6} {"program":16} {"alive":>9} {"cpu":>9} {"guest cpu":>9} {"sys cpu":>9} {"sys wait":>9} {"off-cpu":>9}  top waits')
    for t in sorted(mains, key=lambda t: -t['alive'])[:a.top]:
        sc_wall, sc_cpu, guest, wait = split(t)
        top = sorted(t['sc'].items(), key=lambda kv: -(kv[1][1] - kv[1][2]))[:3]
        tops = ' '.join(f'{names.get(nr, nr)}:{(w - c) / 1000:.0f}' for nr, (n, w, c) in top if w - c > 1000)
        off = max(t['alive'] - t['cpu'] - wait, 0)
        print(f'{t["tid"]:6} {t["comm"]:16} {ms(t["alive"])} {ms(t["cpu"])} {ms(guest)} {ms(sc_cpu)} {ms(wait)} {ms(off)}  {tops}')
    table('syscalls of the main threads', mains)
    table('syscalls of all threads', threads)


if __name__ == '__main__':
    main()

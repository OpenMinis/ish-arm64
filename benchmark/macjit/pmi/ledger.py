#!/usr/bin/env python3
"""Split the instructions an ish run retired by what they were for.

    ledger.py <samples.ktrace> <period> <ish binary> [--map ISH_JIT_MAP file] [--pid PID]
              [--native N] [--top 20]

<samples.ktrace> comes from sample.sh: one PMI sample every <period> retired
instructions (INST_ALL), with the PC and the user call stack. Each sample is
charged to one line of the ledger:
  - generated code: by the role of the word it hit (ISH_JIT_MAP's R lines,
    asbestos/guest-arm64/jit.c ROLE_*);
  - ish's own code: by the closest frame (the PC, then its callers) that a rule
    below knows -- memcpy called from gen_step is the translator's;
  - host kernel PCs: likewise by the user stack, as their own line.
The ish binary must run without ASLR (pmisample does that) so its PCs match
the binary's link-time addresses. --native gives the native instruction count
of the same workload, for the expansion factor.
"""
import argparse
import bisect
import csv
import re
import subprocess
import sys
from collections import Counter, defaultdict

KERNEL = 0xfffffe0000000000
ISH_BASE = 0x100000000

ROLE_LINE = {
    'g': 'real work (guest instructions)',
    's': 'address translation (window check)',
    't': 'address translation (TLB)',
    'r': 'register load/store',
    'p': 'register load/store (cold entry)',
    'd': 'block dispatch and links',
    'R': 'block dispatch and links (ret)',
    'I': 'block dispatch and links (br/blr)',
    'x': 'slow path back to gadgets',
}

# (ledger line, symbol regex), the closest frame that matches wins
RULES = [
    ('measurement (ISH_JIT_MAP)', r'^(cm_write|cm_segment|cm_block|jit_report|rec_write)$'),
    ('translator', r'^(fiber_block_compile|fiber_block_compile_protected|gen_|gen$|jit_block|jit_step_end|'
                   r'jit_translate|jit_native|emit_|finish_native|install_code|reg_|aot_|jit_units_begin|'
                   r'fiber_insert|fiber_resize_hash|jit_link|jit_chain|jit_unlink|patch_insn|claim_u64|'
                   r'jit_segment_installed|slot_|ctx_get|record_segment|one_mapping|jit_locate|cm_locate|'
                   r'mmu_mark_code_page|jit_block_free|fiber_block_free|fiber_free_jetsam|asbestos_drop)'),
    ('address translation (C miss path)', r'^(tlb_handle_miss|tlb_sync|tlb_flush|tlb_refresh|handle_read_miss|'
                                          r'handle_write_miss|handle_miss|sba_populate|mem_mmu_translate|'
                                          r'mem_ptr|mem_pt|crosspage)'),
    ('gadget fallback', r'^(gadget_|back_|bcond_|c_|fiber_ret|fiber_exit|crypto_|tbnz_|tbz_|cbz_|cbnz_)'),
    ('block dispatch and links (run loop)', r'^(cpu_step_to_interrupt|cpu_run_to_interrupt|fiber_enter|'
                                            r'fiber_lookup|world_enter|world_leave)$'),
    ('syscalls and kernel emulation', r'^(handle_interrupt|sys_|do_|task_|mm_|mem_|pt_|sba_|fd_|generic_|'
                                      r'realfs_|fakefs_|path_|__path|vfs_|signal|receive_signals|futex|poll_|'
                                      r'epoll|pipe_|sock|elf_|__do_execve|format_exec|user_|tty|lock|unlock|'
                                      r'wrlock|read_wrlock|write_wrlock|asbestos_invalidate|asbestos_free)'),
]
RULES = [(n, re.compile(r)) for n, r in RULES]


def load_map(path):
    segs, roles = [], {}
    if not path:
        return segs, roles
    for line in open(path, errors='replace'):
        p = line.split()
        if p and p[0] == 'S':
            segs.append((int(p[1], 16), int(p[2])))
        elif p and p[0] == 'R':
            roles[int(p[1], 16)] = p[2]
    segs.sort()
    return segs, roles


def text_end(binary):
    out = subprocess.run(['otool', '-l', binary], capture_output=True, text=True).stdout
    m = re.search(r'segname __TEXT\n\s+vmaddr (0x[0-9a-f]+)\n\s+vmsize (0x[0-9a-f]+)', out)
    return int(m.group(1), 16) + int(m.group(2), 16)


def symbolize(binary, addrs):
    addrs = sorted(addrs)
    names = {}
    for i in range(0, len(addrs), 4000):
        chunk = addrs[i:i + 4000]
        out = subprocess.run(['atos', '-o', binary, '-l', hex(ISH_BASE)] + [hex(a) for a in chunk],
                             capture_output=True, text=True).stdout.splitlines()
        for a, s in zip(chunk, out):
            names[a] = s.split(' (in ')[0].strip()
    return names


def guest_kind(md, code, off, pc):
    ins = next(md.disasm(code[off:off + 4], pc), None)
    if ins is None:
        return '?'
    m, ops = ins.mnemonic, ins.op_str
    if 'nzcv' in ops or ('#0x11c]' in ops and ' [x1' in ops):
        return 'flags save/restore'
    if 'x29' in ops:
        return 'PIC constants (module context)'
    if m.startswith(('ld', 'st')) and '[x1, #' in ops and ops[:1] in ('q', 'd', 's', 'v'):
        return 'FP/SIMD registers in cpu_state'
    if m.startswith(('ld', 'st', 'prfm')) and '[x9' in ops:
        return 'the guest access itself'
    if m in ('add', 'sub', 'mov', 'orr') and ops.startswith('x9,'):
        return 'address computation'
    return 'guest computation'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ktrace')
    ap.add_argument('period', type=int)
    ap.add_argument('binary')
    ap.add_argument('--map', default='')
    ap.add_argument('--pid', type=int, default=0, help='only this process (default: every process named like the binary)')
    ap.add_argument('--native', type=float, default=0, help='native instruction count of the workload')
    ap.add_argument('--top', type=int, default=15)
    ap.add_argument('--dump', default='', help='ISH_JIT_DUMP file: split the guest-instruction role by what the instruction does')
    args = ap.parse_args()

    name = args.binary.rsplit('/', 1)[-1][:16]
    rows = subprocess.run(['ktrace', 'trace', '-R', args.ktrace, '--csv'], capture_output=True, text=True).stdout
    samples = []          # [pc, frames, in_kernel]
    last = {}             # thread -> sample collecting its user stack
    for r in csv.reader(rows.splitlines()[1:]):
        ev, pid, comm = r[2], int(r[10]), r[11]
        if args.pid and pid != args.pid:
            continue
        if not args.pid and comm != name:
            continue
        tid = r[9]
        if ev == 'PERF_KPC_Handler':
            s = [int(r[6]), [], int(r[6]) >= KERNEL]
            samples.append(s)
            last[tid] = s
        elif ev == 'PERF_STK_UData' and tid in last:
            last[tid][1].extend(int(x) for x in r[4:8] if int(x))
        elif ev == 'PERF_STK_UHdr' and tid in last:
            last[tid][1] = []
    if not samples:
        sys.exit('no samples')

    segs, roles = load_map(args.map)
    starts = [s[0] for s in segs]
    region_lo = segs[0][0] if segs else 0
    region_hi = segs[-1][0] + 4 * segs[-1][1] if segs else 0
    ish_hi = text_end(args.binary)

    def in_ish(a):
        return ISH_BASE <= a < ish_hi

    wanted = set()
    for pc, frames, _ in samples:
        for a in [pc] + frames:
            if in_ish(a):
                wanted.add(a if a == pc else a - 4)
    names = symbolize(args.binary, wanted)

    code, code_base = b'', 0
    if args.dump:
        import struct
        raw = open(args.dump, 'rb').read()
        code_base, used = struct.unpack('<QQ', raw[:16])
        code = raw[16:16 + used]
    md = None
    if code:
        import capstone
        md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    gsplit = Counter()

    ledger, detail = Counter(), defaultdict(Counter)
    for pc, frames, kernel in samples:
        line = None
        if not kernel and region_lo <= pc < region_hi + 0x1000000:
            i = bisect.bisect_right(starts, pc) - 1
            if i >= 0 and pc < segs[i][0] + 4 * segs[i][1]:
                r = roles.get(segs[i][0], '')
                k = (pc - segs[i][0]) // 4
                line = ROLE_LINE.get(r[k] if k < len(r) else '?', 'generated code (no role)')
                if md is not None and r[k:k + 1] == 'g' and 0 <= pc - code_base < len(code):
                    gsplit[guest_kind(md, code, pc - code_base, pc)] += 1
            else:
                line = 'block dispatch and links (shared stubs)'
            ledger[line] += 1
            continue
        chain = ([] if kernel else [pc]) + frames
        leaf = names.get(pc, '') if in_ish(pc) else ('host kernel' if kernel else 'host library')
        for j, a in enumerate(chain):
            if not in_ish(a):
                continue
            sym = names.get(a if (j == 0 and not kernel) else a - 4, '')
            for n, rx in RULES:
                if rx.search(sym):
                    line = n
                    break
            if line:
                break
        if line is None:
            line = 'other (ish runtime)' if in_ish(pc) else 'other (host)'
        if kernel:
            line += ' [host kernel]'
        ledger[line] += 1
        detail[line][leaf] += 1

    measure = sum(v for k, v in ledger.items() if k.startswith('measurement'))
    total = sum(ledger.values()) - measure
    print(f'samples {sum(ledger.values())} x {args.period} = {sum(ledger.values()) * args.period / 1e9:.2f}G instructions'
          f' ({measure * args.period / 1e9:.2f}G of them writing the JIT map, left out below)')
    print(f'ish instructions: {total * args.period / 1e9:.2f}G', end='')
    if args.native:
        print(f', native {args.native / 1e9:.2f}G, expansion {total * args.period / args.native:.2f}x')
    else:
        print()
    print('\n| line | instructions (G) | share |')
    print('|---|---|---|')
    for k, v in sorted(ledger.items(), key=lambda kv: -kv[1]):
        if k.startswith('measurement'):
            continue
        print(f'| {k} | {v * args.period / 1e9:.2f} | {100 * v / total:.1f}% |')
    if gsplit:
        g = sum(gsplit.values())
        print('\nguest-instruction role, by instruction (sampled PC):')
        for k, v in gsplit.most_common():
            print(f'  {k:34} {v * args.period / 1e9:6.2f}G  {100 * v / g:5.1f}% of the role')
    if args.top:
        for k, c in sorted(detail.items(), key=lambda kv: -sum(kv[1].values())):
            if k.startswith('measurement') or sum(c.values()) < total * 0.01:
                continue
            print(f'\n{k}: ' + ', '.join(f'{s or "?"} {100 * n / total:.1f}%' for s, n in c.most_common(args.top)))


if __name__ == '__main__':
    main()

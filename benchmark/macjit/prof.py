#!/usr/bin/env python3
"""Attribute the samples of a macOS `sample` report of ish to categories.

    prof.py <sample.txt> [--threads REGEX] [--top N] [--detail CATEGORY]

Every sample is charged to the outermost frame on its stack that a rule
matches (so a TLB miss taken inside a compile counts as compile), and samples
with no matching frame to the leaf: generated code has no unwind info, so it
shows up as a bare "???" frame at the top of a thread.
"""
import argparse
import re
import sys
from collections import Counter

# (category, frame regex), checked from the root of each stack down.
RULES = [
    ('wait', r'^(__semwait_signal|__psynch_cvwait|__psynch_mutexwait|__ulock_wait|__select|__wait4|kevent|'
             r'poll|read|__read_nocancel|recvfrom|__recvfrom|nanosleep|__workq_kernreturn|mach_msg2_trap|'
             r'__pselect|__sigsuspend|__accept)$'),
    ('exit/mm teardown', r'^(do_exit|mm_release_from|mem_destroy)\b'),
    ('exec', r'^(__do_execve|sys_execve|elf_exec)\b'),
    ('fork', r'^(sys_clone|do_clone|copy_mm|mm_copy|sys_fork|sys_vfork)\b'),
    ('mmap/munmap', r'^(sys_mmap|sys_mmap64|sys_munmap|sys_mprotect|sys_brk|sys_madvise)\b'),
    ('syscall', r'^(handle_interrupt|handle_syscall)\b'),
    ('compile:jit', r'^(jit_block|jit_step_end)\b'),
    ('compile:gadget', r'^(fiber_block_compile|gen_step|gen_start|gen_end)\b'),
    ('block lookup', r'^(fiber_lookup|fiber_insert|fiber_resize_hash)\b'),
    ('invalidate', r'^(asbestos_invalidate|jit_unlink)'),
    ('tlb', r'^(tlb_|handle_read_miss|handle_write_miss|handle_miss|mem_mmu_translate|mem_pt|mem_ptr)'),
    ('signal', r'^(receive_signals|send_signal|deliver_signal)'),
    ('gadgets', r'^(gadget_|back_|bcond_|c_stxr|fiber_ret|fiber_exit|crypto_)'),
    ('dispatch', r'^(cpu_step_to_interrupt|cpu_run_to_interrupt|fiber_enter)\b'),
]
RULES = [(c, re.compile(r)) for c, r in RULES]
LINE = re.compile(r'^([ +!:|]*)(\d+) (.+?)  \(in ([^)]+)\)')
THREAD = re.compile(r'^\s+(\d+) Thread_\d+(?::\s*(.*))?')


FS = re.compile(r'^(realfs_|fakefs_|generic_|path_|__path|sys_open|sys_openat|sys_stat|sys_fstat|sys_lstat|'
                r'sys_newfstatat|sys_statx|sys_read|sys_pread|sys_write|sys_pwrite|sys_getdents|sys_readlink|'
                r'sys_access|sys_faccessat|sys_close|sys_unlink|sys_rename|sys_mkdir|sys_rmdir|fd_|file_|'
                r'mount_|sqlite3)')


FALLBACK = [(c, re.compile(r)) for c, r in [
    ('exit/mm teardown', r'^(reaper|region_free|do_exit_group|sys_exit_group|sys_exit)$'),
    ('exec', r'^(format_exec|load_entry)$'),
    ('mmap/munmap', r'^(pt_|sba_|do_mmap|asbestos_free|mmu_mark_code_page|mem_remove_reservations)'),
    ('compile:jit', r'^(reg_install|reg_|emit_|ctx_get|jit_)'),
    ('compile:gadget', r'^(gen|gen_)'),
    ('syscall', r'^(sys_|generic_|realfs_|fakefs_)'),
    ('dispatch', r'^(task_run_current|task_thread)$'),
]]


def classify(stack):
    if RULES[0][1].search(stack[-1][0]):
        return 'wait'
    for i, (fn, lib) in enumerate(stack):
        for cat, rx in RULES:
            if rx.search(fn):
                # file system work under a syscall: its own line
                if cat == 'syscall' and any(FS.search(f) for f, _ in stack[i + 1:]):
                    return 'syscall:fs'
                return cat
    # handle_interrupt and other frames inlined away: go by what is left
    for i, (fn, lib) in enumerate(stack):
        for cat, rx in FALLBACK:
            if rx.search(fn):
                if cat == 'syscall' and any(FS.search(f) for f, _ in stack[i:]):
                    return 'syscall:fs'
                return cat
    fn, lib = stack[-1]
    if fn == '???':
        return 'generated code'
    if lib == 'ish':
        return 'other ish'
    return f'other {lib}'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file')
    ap.add_argument('--threads', default='', help='only threads whose name matches')
    ap.add_argument('--top', type=int, default=0, help='also list the N hottest leaf frames')
    ap.add_argument('--detail', default='', help='list the hottest frames inside this category')
    args = ap.parse_args()
    want = re.compile(args.threads) if args.threads else None

    cats, leaves, detail = analyze(args.file, want, args.detail)
    report(cats, leaves, detail, args)


def analyze(path, want=None, want_detail=''):
    """Samples per category, per leaf frame, and per leaf in want_detail."""
    cats, leaves, detail = Counter(), Counter(), Counter()
    stack, counts, keep = [], [], False
    text = open(path, errors='replace').read()
    text = text.split('Sort by top of stack')[0]
    for line in text.splitlines():
        t = THREAD.match(line)
        if t:
            name = t.group(2) or ''
            while stack:
                flush(stack, counts, cats, leaves, detail, want_detail)
            keep = not want or bool(want.search(name))
            continue
        m = LINE.match(line)
        if not m or not keep:
            continue
        depth = len(m.group(1))
        n = int(m.group(2))
        fn = m.group(3).split(' + ')[0].strip()
        lib = m.group(4)
        # Close the frames this line is not inside of; their self time is
        # what their children do not account for.
        while counts and counts[-1][2] >= depth:
            flush(stack, counts, cats, leaves, detail, want_detail)
        if counts:
            counts[-1][1] += n
        stack.append((fn, lib))
        counts.append([n, 0, depth])
    while stack:
        flush(stack, counts, cats, leaves, detail, want_detail)
    return cats, leaves, detail


def report(cats, leaves, detail, args):
    total = sum(v for c, v in cats.items() if c != 'wait')
    if not total:
        sys.exit('no samples')
    print(f'{total} busy samples (wait: {cats["wait"]})')
    for c, v in cats.most_common():
        if c != 'wait':
            print(f'  {c:20} {v:7} {100 * v / total:5.1f}%')
    for title, ctr in ((f'top frames in {args.detail}', detail), ('top leaf frames', leaves)):
        n = args.top if ctr is leaves else 25
        if not ctr or not n:
            continue
        print(f'\n{title}:')
        for f, v in ctr.most_common(n):
            print(f'  {v:7} {100 * v / total:5.1f}%  {f}')


def flush(stack, counts, cats, leaves, detail, want_detail):
    total, children, _ = counts.pop()
    own = total - children
    if own > 0 and stack:
        cat = classify(stack)
        cats[cat] += own
        leaf = stack[-1][0]
        if cat != 'wait':
            leaves[leaf if leaf != '???' else '??? (generated code)'] += own
        if cat == want_detail:
            detail[leaf] += own
    stack.pop()


if __name__ == '__main__':
    main()

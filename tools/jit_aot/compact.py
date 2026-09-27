#!/usr/bin/env python3
"""Rewrite an AOT image made in the old 64-bit table layout (gen.py before the compact
layout, abi daf8fcc7) into the compact table layout of
asbestos/guest-arm64/aot.h: 32-bit self-relative pointers instead of 64-bit absolute ones
(Ltrans, Lg, Ls), and keys without the words that are always 0 in an image (per unit the
high dpc word and the two gadget words). The native code is left byte for byte as it is.

usage: compact.py <in.S> <out.S> --abi <hex abi of the ish that loads it>
"""
import argparse, re, sys

KEY_HDR, KEY_UNIT, DROP = 7, 9, (4, 7, 8)
LABEL = re.compile(r'^(L[kgs])(\d+):$')


def fail(msg):
    sys.exit(f'❌ {msg}')


def rel(expr):
    expr = expr.strip()
    return '    .long 0' if expr == '0' else f'    .long {expr} - .'


def compact_key(ti, words):
    if len(words) != KEY_HDR + KEY_UNIT * words[4]:
        fail(f'Lk{ti}: {len(words)} words for {words[4]} units')
    out = words[:KEY_HDR]
    for u in range(words[4]):
        unit = words[KEY_HDR + KEY_UNIT * u: KEY_HDR + KEY_UNIT * (u + 1)]
        if any(unit[j] for j in DROP):
            fail(f'Lk{ti} unit {u}: a word the compact key leaves out is not 0: {unit}')
        out += [w for j, w in enumerate(unit) if j not in DROP]
    return [f'    .long ' + ', '.join(str(x) for x in out[i:i + 8]) for i in range(0, len(out), 8)]


def compact_lines(lines, abi):
    """The compact image for the lines (no line ends) of one in the 64-bit layout, and stats."""
    out, held, mode, key, ti, trans_line, in_data, after_ltrans_ptr = [], None, None, [], None, 0, False, False
    stats = {'k_in': 0, 'k_out': 0, 'ptr': 0}

    def flush_key():
        nonlocal key
        if mode == 'k':
            lines = compact_key(ti, key)
            stats['k_in'] += len(key); stats['k_out'] += sum(len(l.split(',')) for l in lines)
            out.extend(lines)
        key = []

    for line in lines:
        t = line.strip()
        if t.startswith('.section'):
            in_data = '__DATA,__const' in t
        if not in_data:
            out.append(line); continue
        if mode == 'k' and not t.startswith('.long'):
            flush_key(); mode = None
        if t == '.p2align 3' and held is None:
            held = line; continue
        m = LABEL.match(t)
        if held is not None:
            out.append('    .p2align 2' if m else held); held = None
        if m:
            mode, ti = {'Lk': 'k', 'Lg': 'g', 'Ls': 's'}[m.group(1)], m.group(2)
            out.append(line); continue
        if t == 'Ltrans:':
            mode, trans_line = 'trans', 0; out.append(line); continue
        if t.endswith(':'):
            mode = None
            out.append(line); continue
        if mode == 'k':
            key += [int(x) for x in t[len('.long'):].split(',')]; continue
        if mode == 'g' and t.startswith('.quad'):
            out.append(rel(t[len('.quad'):])); stats['ptr'] += 1; continue
        if mode == 's' and t.startswith('.quad'):
            out.append(rel(t[len('.quad'):])); stats['ptr'] += 1; continue
        if mode == 'trans' and t.startswith(('.quad', '.long')):
            k = trans_line % 5; trans_line += 1
            if k in (2, 4):
                for e in t[len('.quad'):].split(','):
                    out.append(rel(e)); stats['ptr'] += 1
                continue
            out.append(line); continue
        # the module header: '.quad Ltrans' then '.long prologue, entry_off, n_pinned, abi'
        if t == '.quad Ltrans':
            after_ltrans_ptr = True; out.append(line); continue
        if after_ltrans_ptr and t.startswith('.long'):
            v = [x.strip() for x in t[len('.long'):].split(',')]
            v[3] = str(abi); out.append('    .long ' + ', '.join(v)); after_ltrans_ptr = False; continue
        out.append(line)
    flush_key()
    if held is not None:
        out.append(held)
    return out, stats


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('src'); ap.add_argument('dst'); ap.add_argument('--abi', required=True)
    args = ap.parse_args()
    abi = int(args.abi, 16)
    out, stats = compact_lines((line.rstrip('\n') for line in open(args.src)), abi)
    open(args.dst, 'w').write('\n'.join(out) + '\n')
    print(f'✅ {args.dst}: key words {stats["k_in"]} -> {stats["k_out"]}, {stats["ptr"]} pointers made relative, abi {abi:08x}')


if __name__ == '__main__':
    main()

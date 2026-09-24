#!/usr/bin/env python3
"""Turn an ISH_JIT_RECORD recording into an AOT image (.S) for one guest module.

The recording holds the position-independent (ISH_JIT_PIC=1) translations of
one module, with every host address the code depends on named. This script
lays them out as assembly that links into ish:

- code in __TEXT,__ish_aot: each host symbol (movz + 3 movk in the recording)
  becomes adrp/add + 2 nops, each branch to the JIT's exit stub a branch to
  the image's own stub, and each direct link the JIT had made a static branch
  whose literal holds the assemble-time offset target - branch;
- tables in __DATA,__const, described by `struct aot_module` (asbestos/guest-
  arm64/aot.h): module identity (path, size, sha256), translations sorted by
  file offset with their validation key, gadget per unit, segments, links and
  self-loop register maps;
- a constructor that hands the image to ish_aot_register() at load time.

An image is not part of ish: a target links it in as an extra object (the
macOS CLI with meson -Dcli_aot=aot_<name>.S, the iOS app from its own
target), and ish must be built with -Djit=true.

usage: gen.py <recording> <ish binary> <rootfs> aot_<name>.S [--name <name>]
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys

from tqdm import tqdm

KEY_HDR = 7          # key words before the units: mod, off lo/hi, page offset, units, 2 slots
KEY_UNIT = 9         # words per unit; words 7 and 8 of a unit are its gadget pointer
LINK_UNSET = 1


def load_recording(path):
    header, trans = None, []
    with open(path) as f:
        for line in f:
            obj = json.loads(line)
            if 'header' in obj:
                header = obj['header']
                continue
            trans.append(obj)
    if header is None:
        sys.exit(f"❌ {path}: no header line (record with the current ish)")
    return header, trans


def host_symbols(binary):
    """Recorded names are dladdr names (no leading underscore); map them to
    the names the linker sees: `_gadget_x` for C/.gadget symbols, `fiber_ret`
    for plain assembly labels."""
    out = subprocess.run(['nm', '-g', binary], capture_output=True, text=True, check=True).stdout
    names = {line.split()[-1] for line in out.splitlines() if line.strip()}
    table = {}
    for n in names:
        table.setdefault(n[1:] if n.startswith('_') else n, n)
    for n in names:   # an exact (underscore-less) definition wins
        if not n.startswith('_'):
            table[n] = n
    return table


def link_name(syms, name):
    if name == '@region':
        return 'Laot_text_start'
    if '+' in name or name.startswith('?'):
        return None
    return syms.get(name)


def seg_label(t, s):
    return f'Lt{t}s{s}'


def emit_segment(out, syms, t, s, seg, stats):
    words = seg['words']
    special = {}
    for rel in seg['rel']:
        at, kind = rel[0], rel[1]
        if kind == 'exit':
            special[at] = ('exit',)
            continue
        target = link_name(syms, rel[2])
        if target is None:
            return False          # a host address we cannot name: leave this translation out
        special[at] = ('sym', target, words[at] & 31)
    for slot, at, tt, ts, tw in seg['links']:
        special[at] = ('link', slot, tt, ts, tw)

    out.append('    .p2align 4')
    out.append(f'{seg_label(t, s)}:')
    i = 0
    while i < len(words):
        sp = special.get(i)
        if sp is None:
            out.append(f'    .inst 0x{words[i]:08x}')
            i += 1
            continue
        if sp[0] == 'exit':
            out.append('    b Laot_exit_stub')
            stats['exit'] += 1
            i += 1
            continue
        if sp[0] == 'sym':
            _, target, rd = sp
            out.append(f'    adrp x{rd}, {target}@PAGE')
            out.append(f'    add x{rd}, x{rd}, {target}@PAGEOFF')
            out.append('    nop')
            out.append('    nop')
            stats['sym'] += 1
            i += 4
            continue
        _, slot, tt, ts, tw = sp
        site = f'{seg_label(t, s)}_l{slot}'
        if tt >= 0 and (tt, ts) in stats['labels']:
            dest = f'{seg_label(tt, ts)} + {4 * tw}'
            out.append(f'{site}:')
            out.append(f'    b {dest}')
            out.append(f'    .quad ({dest}) - {site}')
            stats['linked'] += 1
        else:
            out.append(f'{site}:')
            out.append(f'    .inst 0x{words[i]:08x}')                  # unlinked: skips the literal
            out.append(f'    .quad {LINK_UNSET}')
            stats['unlinked'] += 1
        i += 3
    return True


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.digest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('recording')
    ap.add_argument('ish')
    ap.add_argument('rootfs')
    ap.add_argument('out')
    ap.add_argument('--name', default='musl')
    args = ap.parse_args()

    header, trans = load_recording(args.recording)
    mods = {t['mod'] for t in trans}
    if len(mods) != 1:
        sys.exit(f"❌ expected one module in the recording, got {sorted(mods)}")
    mod = mods.pop()
    mod_file = os.path.join(args.rootfs, mod.lstrip('/'))
    if not os.path.isfile(mod_file):
        sys.exit(f"❌ {mod_file}: module not found in the rootfs")
    syms = host_symbols(args.ish)
    print(f"📼 {len(trans)} translations of {mod}, prologue {header['prologue_words']} words")

    # Stable order: by file offset (the loader binary-searches it), then key.
    order = sorted(range(len(trans)), key=lambda i: (trans[i]['off'], trans[i]['key']))
    new_id = {old: new for new, old in enumerate(order)}
    trans = [trans[i] for i in order]
    for t in trans:
        for seg in t['segs']:
            seg['links'] = [[slot, at, new_id[tt] if tt >= 0 else -1, ts, tw] for slot, at, tt, ts, tw in seg['links']]

    stats = {'sym': 0, 'exit': 0, 'linked': 0, 'unlinked': 0, 'dropped': 0, 'labels': set()}
    for ti, t in enumerate(trans):
        for si in range(len(t['segs'])):
            stats['labels'].add((ti, si))

    code, keep = [], []
    for ti, t in enumerate(tqdm(trans, desc='🛠  code', unit='tr')):
        body = []
        ok = all(emit_segment(body, syms, ti, si, seg, stats) for si, seg in enumerate(t['segs']))
        if not ok:
            stats['dropped'] += 1
            continue
        code += body
        keep.append(ti)
    # A dropped translation's code is gone: branches into it would dangle.
    dropped = set(range(len(trans))) - set(keep)
    if dropped:
        sys.exit(f"❌ {len(dropped)} translations reference unnamed host addresses (first: {trans[min(dropped)]['off']:#x})")

    exit_stub = header['exit_stub']
    data = []
    for ti, t in enumerate(tqdm(trans, desc='📋 tables', unit='tr')):
        key = list(t['key'])
        key[0] = 0                                     # module word: filled in by the loader
        nunits = key[4]
        gadgets = []
        for u in range(nunits):
            g = KEY_HDR + KEY_UNIT * u + 7
            name = t['keysym'].get(str(g))
            key[g] = key[g + 1] = 0
            gadgets.append(link_name(syms, name) if name else None)
        data.append('    .p2align 3')
        data.append(f'Lk{ti}:')
        for i in range(0, len(key), 8):
            data.append('    .long ' + ', '.join(str(x) for x in key[i:i + 8]))
        data.append('    .p2align 3')
        data.append(f'Lg{ti}:')
        for g in gadgets:
            data.append(f'    .quad {g}' if g else '    .quad 0')
        data.append(f'Ls{ti}:')
        for si, seg in enumerate(t['segs']):
            data.append(f'    .quad {seg_label(ti, si)}')
            data.append(f'    .long {seg["pos"]}, {len(seg["words"])}')
        loop = next((seg['loop'] for seg in t['segs'] if seg['loop']), None)
        if loop:
            si = next(i for i, seg in enumerate(t['segs']) if seg['loop'])
            head, end, g, d, h = loop
            pad = lambda xs: xs + [0] * (8 - len(xs))
            data.append('    .p2align 3')
            data.append(f'Ll{ti}:')
            data.append(f'    .quad {seg_label(ti, si)} + {4 * head}, {seg_label(ti, si)} + {4 * end}')
            data.append(f'    .byte {len(g)}, 0, 0, 0, 0, 0, 0, 0')
            data.append('    .byte ' + ', '.join(str(x) for x in pad(g) + pad(d) + pad(h)))
        t['_loop'] = bool(loop)

    lines = [f'// AOT image of {mod}: {len(trans)} translations, generated by tools/jit_aot/gen.py.',
             '// Do not edit.', '',
             '    .section __TEXT,__ish_aot,regular,pure_instructions',
             '    .p2align 14',
             'Laot_text_start:',
             'Laot_exit_stub:']
    lines += [f'    .inst 0x{w:08x}' for w in exit_stub]
    lines += code
    lines += ['    .p2align 2', 'Laot_text_end:', '', '    .section __DATA,__const']
    lines += data
    lines.append('    .p2align 3')
    lines.append('Ltrans:')
    for ti, t in enumerate(trans):
        links = []
        for slot in range(2):
            site = next((f'{seg_label(ti, si)}_l{slot}' for si, seg in enumerate(t['segs'])
                         for l in seg['links'] if l[0] == slot), None)
            links.append(site or '0')
        lines.append(f'    .quad {t["off"]}')
        lines.append(f'    .long {t["idx"]}, {len(t["key"])}')
        lines.append(f'    .quad Lk{ti}, Lg{ti}, Ls{ti}')
        lines.append(f'    .long {len(t["segs"])}, {t["key"][4]}')
        lines.append(f'    .quad {links[0]}, {links[1]}, {f"Ll{ti}" if t["_loop"] else "0"}')
    digest = sha256_of(mod_file)
    lines += ['Lpath:', f'    .asciz "{mod}"', '    .p2align 3',
              f'    .globl _ish_aot_module_{args.name}',
              f'_ish_aot_module_{args.name}:',
              '    .quad Lpath',
              f'    .quad {os.path.getsize(mod_file)}',
              '    .byte ' + ', '.join(str(b) for b in digest),
              f'    .long {len(trans)}, {max(t["idx"] for t in trans) + 1}',
              '    .quad Ltrans',
              f'    .long {header["prologue_words"]}, {header["entry_off"]}, {header["n_pinned"]}, 0',
              '    .quad Laot_text_start, Laot_text_end', '',
              # Register the image when the binary that links it is loaded.
              '    .text',
              '    .p2align 2',
              'Laot_register:',
              f'    adrp x0, _ish_aot_module_{args.name}@PAGE',
              f'    add x0, x0, _ish_aot_module_{args.name}@PAGEOFF',
              '    b _ish_aot_register',
              '    .section __DATA,__mod_init_func,mod_init_funcs',
              '    .p2align 3',
              '    .quad Laot_register', '']
    with open(args.out, 'w') as f:
        f.write('\n'.join(lines))
    print(f"✅ {args.out}: {stats['sym']} symbols, {stats['exit']} exits, "
          f"{stats['linked']} static links ({stats['unlinked']} unlinked), sha256 {digest.hex()[:16]}…")


if __name__ == '__main__':
    main()

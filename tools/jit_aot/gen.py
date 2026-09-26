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
import re
import struct
import subprocess
import sys

from tqdm import tqdm

NO_LINKS = False     # --no-links: every block end takes the generic path (successor native or not)
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
        if tt >= 0 and (tt, ts) in stats['labels'] and not NO_LINKS:
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


def segment_offsets(t):
    """File offset of the first guest instruction of each segment."""
    key, offs = t['key'], []
    for seg in t['segs']:
        for u in range(key[4]):
            k = KEY_HDR + KEY_UNIT * u
            if key[k] == seg['pos']:
                dpc = key[k + 3] | key[k + 4] << 32
                offs.append(t['off'] + dpc)
                break
    return offs


def build_id_of(path):
    """NT_GNU_BUILD_ID of a 64-bit little-endian ELF file, b'' if none."""
    with open(path, 'rb') as f:
        data = f.read(1 << 16)
        if data[:6] != b'\x7fELF\x02\x01':
            return b''
        phoff, = struct.unpack_from('<Q', data, 0x20)
        phentsize, phnum = struct.unpack_from('<HH', data, 0x36)
        for i in range(phnum):
            f.seek(phoff + i * phentsize)
            ptype, _, noff, _, _, nsize = struct.unpack('<IIQQQQ', f.read(40))
            if ptype != 4:   # PT_NOTE
                continue
            f.seek(noff)
            notes, p = f.read(nsize), 0
            while p + 12 <= len(notes):
                namesz, descsz, ntype = struct.unpack_from('<III', notes, p)
                name, desc = p + 12, p + 12 + ((namesz + 3) & ~3)
                if ntype == 3 and notes[name:name + namesz] == b'GNU\0' and 0 < descsz <= 20:
                    return notes[desc:desc + descsz]
                p = desc + ((descsz + 3) & ~3)
    return b''


def insn_norm(w):
    """insn_norm() of jit.c: a guest word without the pc-relative immediate the translation does not
    depend on (branch targets come from the block's code stream, adr / adrp targets from dbase)."""
    if (w & 0x7C000000) == 0x14000000: return w & 0xFC000000            # b, bl
    if (w & 0xFF000010) == 0x54000000: return w & 0xFF00001F            # b.cond
    if (w & 0x7E000000) == 0x34000000: return w & 0xFF00001F            # cbz / cbnz
    if (w & 0x7E000000) == 0x36000000: return w & 0xFFF8001F            # tbz / tbnz
    if (w & 0x1F000000) == 0x10000000: return w & 0x9F00001F            # adr / adrp
    return w


def content_hash(key):
    """key_content_hash() of jit.c: FNV-1a over the key words after the module, file offset and page
    offset (instruction words normalized), skipping the gadget words. Finds a block whose code moved
    or whose pc-relative immediates changed in another module version."""
    h = 0xcbf29ce484222325
    for i in range(4, len(key)):
        if i >= KEY_HDR and (i - KEY_HDR) % KEY_UNIT >= 7:
            continue
        w = insn_norm(key[i]) if i >= KEY_HDR and (i - KEY_HDR) % KEY_UNIT in (5, 6) else key[i]
        h = ((h ^ w) * 0x100000001b3) & 0xffffffffffffffff
    return h


def default_family(mod):
    """The file names an image also serves: other versions of a library keep its soname prefix
    (libz.so.1.3.2 -> libz.so.1*), a program its name."""
    name = os.path.basename(mod)
    m = re.match(r'^(.*\.so\.\d+)(\.[\d.]+)?$', name)
    return m.group(1) + '*' if m else name


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
    ap.add_argument('--keep', help='only translations with a segment at one of these file offsets '
                    '(one per line, hex or decimal): the hot code from a profile; the rest runs as gadgets')
    ap.add_argument('--family', help='fnmatch() pattern of the file names this image also serves (other '
                    'versions of the module, matched block by block); default from the file name, '
                    '"" for none')
    ap.add_argument('--abi', help='abi of the ish that recorded, for a recording made before the header had it '
                    '(the "abi" of /proc/ish/jit in that same build)')
    ap.add_argument('--no-links', action='store_true', help='no static links between translations: a block '
                    'end enters its successor through the generic path, so it chains to any successor')
    args = ap.parse_args()
    global NO_LINKS
    NO_LINKS = args.no_links

    header, trans = load_recording(args.recording)
    abi = header.get('abi') or (int(args.abi, 16) if args.abi else None)
    if not abi:
        sys.exit(f"❌ {args.recording}: no abi in the header; re-record, or pass --abi of the ish that made it")
    mods = {t['mod'] for t in trans}
    if len(mods) != 1:
        sys.exit(f"❌ expected one module in the recording, got {sorted(mods)}")
    mod = mods.pop()
    mod_file = os.path.join(args.rootfs, mod.lstrip('/'))
    if not os.path.isfile(mod_file):
        sys.exit(f"❌ {mod_file}: module not found in the rootfs")
    syms = host_symbols(args.ish)
    print(f"📼 {len(trans)} translations of {mod}, prologue {header['prologue_words']} words")

    kept = range(len(trans))
    if args.keep:
        hot = {int(x, 0) for x in open(args.keep).read().split()}
        kept = [i for i, t in enumerate(trans) if hot & set(segment_offsets(t))]
        print(f"✂️  keeping {len(kept)} of {len(trans)} translations ({len(hot)} hot offsets)")
    # Stable order: by file offset (the loader binary-searches it), then key.
    order = sorted(kept, key=lambda i: (trans[i]['off'], trans[i]['key']))
    new_id = {old: new for new, old in enumerate(order)}
    trans = [trans[i] for i in order]
    for t in trans:
        for seg in t['segs']:
            # a link into a translation left out stays unlinked
            seg['links'] = [[slot, at, new_id.get(tt, -1), ts, tw] for slot, at, tt, ts, tw in seg['links']]

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
    build_id = build_id_of(mod_file)
    family = default_family(mod) if args.family is None else args.family
    lines.append('    .p2align 3')
    lines.append('Lbyhash:')
    for h, ti in sorted((content_hash(t['key']), ti) for ti, t in enumerate(trans)):
        lines.append(f'    .quad {h}')
        lines.append(f'    .long {ti}, 0')
    if family:
        lines.append(f'Lfamily:\n    .asciz "{family}"')
    lines += ['Lpath:', f'    .asciz "{mod}"', '    .p2align 3',
              f'    .globl _ish_aot_module_{args.name}',
              f'_ish_aot_module_{args.name}:',
              '    .quad Lpath',
              f'    .quad {os.path.getsize(mod_file)}',
              '    .byte ' + ', '.join(str(b) for b in digest),
              f'    .long {len(trans)}, {max(t["idx"] for t in trans) + 1}',
              '    .quad Ltrans',
              f'    .long {header["prologue_words"]}, {header["entry_off"]}, {header["n_pinned"]}, {abi}',
              '    .quad Laot_text_start, Laot_text_end',
              f'    .long {len(build_id)}',
              '    .byte ' + ', '.join(str(b) for b in build_id.ljust(20, b'\0')),
              '    .p2align 3',
              f'    .quad {"Lfamily" if family else "0"}',
              '    .quad Lbyhash',
              f'    .long {len(trans)}, 0', '',
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
          f"{stats['linked']} static links ({stats['unlinked']} unlinked), family {family or '-'}, "
          f"sha256 {digest.hex()[:16]}…")


if __name__ == '__main__':
    main()

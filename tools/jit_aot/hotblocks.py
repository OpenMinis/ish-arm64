#!/usr/bin/env python3
"""Pick the translations of a recording worth keeping in an image, by execution profile.

usage: hotblocks.py <recording.jsonl> <profile.txt>... [--coverage 0.999] [--max N] [--keep out.txt]

profile.txt: the block profiler's "<module> <offset hex> <insns> <count>" lines (ISH_BLOCK_PROF_FILE, run with
ISH_JIT=0 ISH_NO_CHAIN=1 ISH_NO_RETCACHE=1). A translation is worth what it saves, the guest instructions it runs
natively instead of as gadgets (count * insns of its block), and costs its native code: translations are taken by
worth per byte until they cover the given share of the module's executed instructions. With several profiles
(workloads), each is scaled to the same total first, so a short workload counts as much as a long one.

With --max the image keeps that many translations: those for the coverage first, then the rest in the
order the recording first translated them. Code that runs early (start-up, the runtime's core) serves
workloads the profile never saw better than code that is merely rare in it: for node, 128000
translations picked this way ran the node cases 0.9% faster than the first 123464 the recording made,
and 5.9% faster than the 128000 worth most; all 155894 ran them 0.2% faster than those 128000.
"""
import argparse
import json
import sys

LEVELS = [0.9, 0.95, 0.99, 0.995, 0.999, 0.9995, 0.9999, 1.0]


def load(rec, profs):
    trans = []
    mod = None
    for line in open(rec):
        if not line.startswith('{"mod"'):
            continue
        t = json.loads(line)
        mod = t['mod']
        trans.append((t['off'], 4 * sum(len(s['words']) for s in t['segs']), t['idx']))
    if not trans:
        sys.exit(f'❌ {rec}: no translations')
    worth = {}
    for prof in profs:
        one = {}
        for line in open(prof):
            f = line.split()
            if len(f) == 4 and f[0] == mod:
                one[int(f[1], 16)] = int(f[2]) * int(f[3])
        if not one:
            sys.exit(f'❌ {prof}: no blocks of {mod}')
        total = sum(one.values())
        for off, w in one.items():
            worth[off] = worth.get(off, 0) + w / total
    return mod, trans, worth


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('recording')
    ap.add_argument('profile', nargs='+')
    ap.add_argument('--coverage', type=float, default=0.999)
    ap.add_argument('--max', type=int, help='keep at most this many translations')
    ap.add_argument('--keep', help='write the kept block offsets here (for gen.py --keep)')
    args = ap.parse_args()

    mod, trans, worth = load(args.recording, args.profile)
    total = sum(worth.values())
    recorded = {t[0] for t in trans}
    covered = sum(w for off, w in worth.items() if off in recorded)
    code = sum(t[1] for t in trans)
    print(f'📼 {mod}: {len(trans)} translations, {code / 2**20:.1f} MB native code')
    print(f'📊 {len(args.profile)} profile(s): {len(worth)} blocks; the recording has blocks for '
          f'{covered / total:.3%} of them ({sum(1 for o in worth if o in recorded)} blocks)')

    order = sorted(trans, key=lambda t: worth.get(t[0], 0) / max(t[1], 1), reverse=True)
    rows, keep = [], order
    acc = size = 0
    levels = list(LEVELS)
    for i, (off, sz, _) in enumerate(order):
        acc += worth.get(off, 0)
        size += sz
        while levels and acc >= levels[0] * covered:
            rows.append((levels.pop(0), i + 1, size))
        if keep is order and acc >= args.coverage * covered:
            keep = order[:i + 1]
    rows.append(('all', len(order), code))
    print('   coverage   translations   native code')
    for lvl, n, sz in rows:
        name = lvl if isinstance(lvl, str) else f'{lvl:.2%}'
        print(f'   {name:>8}   {n:7d} {n / len(order):6.1%}   {sz / 2**20:6.1f} MB {sz / code:6.1%}')
    if args.max:
        keep = keep[:args.max]
        chosen = set(keep)
        keep += [t for t in sorted(trans, key=lambda t: t[2]) if t not in chosen][:args.max - len(keep)]
    got = sum(worth.get(t[0], 0) for t in keep) / covered
    print(f'✂️  keeping {len(keep)} translations, {sum(t[1] for t in keep) / 2**20:.1f} MB: '
          f'{got:.4%} of the executed instructions')
    if args.keep:
        with open(args.keep, 'w') as f:
            f.write('\n'.join(hex(o) for o in sorted(t[0] for t in keep)) + '\n')


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""AOJIT (AOT on JIT code) A/B runner. Runs inside the guest (Mac CLI or the app).

Every case runs with the recorded images on and off (echo on|off > /proc/ish/jit, which
applies to processes started afterwards), alternating the order per round, best of
--repeat. With AOT on it also records how many of the case's blocks the images served,
per module (the "modules with an image" table of /proc/ish/jit, before vs after).

  python3 /tmp/aojit/run_cases.py                    # quick tier, on vs off
  python3 /tmp/aojit/run_cases.py --tier all -r 3    # everything, best of 3
  python3 /tmp/aojit/run_cases.py -g net -g doc      # some groups
  python3 /tmp/aojit/run_cases.py -c 'family\\.'     # ids matching a regex
  python3 /tmp/aojit/run_cases.py --list

Progress goes to stderr; results to <out>.json and <out>.txt (default
/tmp/aojit/results/<time>). Cases whose `needs` are missing are skipped, see cases.json.
Only complete, successful on/off comparisons with stable equal output get a
speedup. Volatile cases retain timings but are unverified, not equality passes.
Exit status: 1 for failed runs/comparisons/setup, 2 for incomplete verification
(skips, volatile output or no cases), 0 for verified comparisons or explicit cur
mode timing. These checks are not an instruction-level correctness proof.
"""
import argparse, hashlib, json, math, os, re, shutil, subprocess, sys, time

A = '/tmp/aojit'
JIT = '/proc/ish/jit'
try:
    from tqdm import tqdm
except ImportError:
    tqdm = None


def log(msg):
    if tqdm:
        tqdm.write(msg, file=sys.stderr)
        return
    print(msg, file=sys.stderr, flush=True)


def read(path):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return ''


def set_mode(mode):
    if mode == 'cur':
        return
    with open(JIT, 'w') as f:
        f.write(mode + '\n')


MOD_ROW = re.compile(r'^\s+(/\S+)\s+(\d+) / (\d+)\s+/ (\d+)')


def module_counts():
    """{module: (blocks, AOT hits, moved)} from /proc/ish/jit."""
    rows = {}
    for line in read(JIT).splitlines():
        m = MOD_ROW.match(line)
        if m:
            rows[m.group(1)] = tuple(int(x) for x in m.groups()[1:])
    return rows


def hit_delta(before, after):
    out = {}
    for mod, (b, h, mv) in after.items():
        b0, h0, mv0 = before.get(mod, (0, 0, 0))
        if b > b0:
            out[mod] = [b - b0, h - h0, mv - mv0]
    return out


def missing_needs(case):
    for need in case.get('needs', []):
        kind, _, what = need.partition(':')
        if kind == 'bin' and not shutil.which(what):
            return need
        if kind == 'file' and not os.path.exists(what):
            return need
        if kind == 'env' and not os.environ.get(what):
            return need
        if kind == 'py' and subprocess.run([sys.executable, '-c', f'import {what}'], capture_output=True).returncode:
            return need
    return None


def run_once(case, port, timeout):
    env = dict(os.environ, PORT=str(port), MPLCONFIGDIR=f'{A}/.mpl')
    t0 = time.perf_counter()
    try:
        p = subprocess.run(['sh', '-c', case['cmd']], cwd=f"{A}/{case['group']}", env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, 'timeout', ''
    dt = time.perf_counter() - t0
    return dt, p.returncode, hashlib.md5(p.stdout).hexdigest()[:12]


def select(cases, args):
    out = []
    for c in cases:
        if args.tier != 'all' and c.get('tier', 'full') != args.tier and not args.case:
            continue
        if args.group and c['group'] not in args.group:
            continue
        if args.case and not re.search(args.case, c['id']):
            continue
        out.append(c)
    return out


def env_info():
    pk = subprocess.run("apk list -I 2>/dev/null | grep -E '^(musl|busybox|python3|zlib|nodejs|py3-numpy|py3-pillow|"
                        "py3-matplotlib|py3-pandas|libssl3|libcrypto3|sqlite-libs|libwebp|libjpeg-turbo)-[0-9]' | cut -d' ' -f1",
                        shell=True, capture_output=True, text=True).stdout.split()
    return {'version': read('/proc/ish/version').strip(), 'date': time.strftime('%Y-%m-%d %H:%M:%S'), 'packages': pk,
            'jit': read(JIT).split('modules with an image')[0].strip()}


def summarize(res, modes):
    lines = [f"{'case':24s} {'title':28s} " + ' '.join(f'{m:>8s}' for m in modes) + '   加速   AOT命中  备注']
    for r in res:
        if r.get('skipped'):
            lines.append(f"{r['id']:24s} {r['title'][:28]:28s} ⏭  skipped ({r['skipped']})")
            continue
        best = r['best']
        cols = ' '.join(f"{best[m]:7.2f}s" if best.get(m) is not None else '     n/a' for m in modes)
        sp = f"{r['speedup']:5.2f}×" if r.get('speedup') else '     -'
        hits = r.get('hits', {})
        b = sum(v[0] for v in hits.values()); h = sum(v[1] for v in hits.values())
        hit = f'{100 * h / b:5.1f}%' if b else '    -'
        note = []
        low = min(hits.items(), key=lambda kv: kv[1][1] / kv[1][0], default=None)
        if low and low[1][0] >= 50:
            note.append(f"min {os.path.basename(low[0]).split('.so')[0]} {100 * low[1][1] / low[1][0]:.0f}%")
        if r.get('family'): note.append('family')
        if r.get('holdout'): note.append('holdout')
        if r.get('same_output') is False: note.append('⚠️ output differs')
        if r.get('status') in ('unverified', 'timing-only'): note.append(r['status'])
        if r.get('errors'): note.append('❌ ' + ','.join(sorted(set(map(str, r['errors'])))))
        lines.append(f"{r['id']:24s} {r['title'][:28]:28s} {cols}  {sp}  {hit}  {' '.join(note)}")
    return '\n'.join(lines)


def run_case(c, args, modes, port):
    rec = {'id': c['id'], 'title': c['title'], 'group': c['group'], 'family': c.get('family', False),
           'holdout': c.get('holdout', False), 'times': {m: [] for m in modes}, 'errors': [], 'outputs': {}}
    hits_total = {}
    for k in range(args.repeat):
        order = modes if k % 2 == 0 else modes[::-1]
        for m in order:
            set_mode(m)
            before = module_counts() if m == 'on' else None
            port += 1
            dt, rc, digest = run_once(c, port, args.timeout)
            if m == 'on':
                for mod, v in hit_delta(before, module_counts()).items():
                    hits_total.setdefault(mod, [0, 0, 0])
                    hits_total[mod] = [a + b for a, b in zip(hits_total[mod], v)]
            if rc != 0:
                rec['errors'].append(rc)
            elif dt is None or not math.isfinite(dt) or dt <= 0:
                rec['errors'].append('invalid-duration')
            else:
                rec['outputs'].setdefault(m, set()).add(digest)
                # Keep sub-millisecond durations; round only for display.
                rec['times'][m].append(dt)
            log(f"   {'🟢' if m == 'on' else '⚪️'} {m:3s} run {k + 1}: " + (f'{dt:7.2f}s' if dt else '  timeout') +
                ('' if rc == 0 else f'  ❌ rc={rc}'))
    rec['best'] = {m: (min(v) if v else None) for m, v in rec['times'].items()}
    complete = args.repeat > 0 and all(len(rec['times'][m]) == args.repeat for m in modes)
    outs = [rec['outputs'].get(m, set()) for m in modes]
    rec['same_output'] = (None if c.get('volatile', False) else
                          bool(complete and outs and all(o == outs[0] and len(o) == 1 for o in outs)))
    if rec['errors'] or not complete or rec['same_output'] is False:
        rec['status'] = 'failed'
    elif rec['same_output'] is None:
        rec['status'] = 'unverified'
    elif set(modes) == {'on', 'off'}:
        rec['status'] = 'passed'
        rec['speedup'] = round(rec['best']['off'] / rec['best']['on'], 3)
    else:
        rec['status'] = 'timing-only'
    rec['outputs'] = {m: sorted(rec['outputs'].get(m, set())) for m in modes}
    rec['hits'] = hits_total
    return rec, port


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--tier', choices=['quick', 'full', 'all'], default='quick')
    ap.add_argument('-g', '--group', action='append')
    ap.add_argument('-c', '--case', help='regex on case ids (any tier)')
    ap.add_argument('-r', '--repeat', type=int, default=2)
    ap.add_argument('--modes', default='on,off', help="comma list of on/off, or 'cur' (leave /proc/ish/jit alone)")
    ap.add_argument('--timeout', type=int, default=900)
    ap.add_argument('--out', help='results path without extension')
    ap.add_argument('--setup', action='store_true', help='(re)generate inputs first')
    ap.add_argument('--list', action='store_true')
    args = ap.parse_args()
    modes = args.modes.split(',')
    if args.repeat < 1 or args.timeout < 1:
        ap.error('--repeat and --timeout must be positive')
    if not (modes == ['cur'] or (len(modes) == 2 and set(modes) == {'on', 'off'})):
        ap.error('--modes must be on,off (either order), or cur')

    with open(f'{A}/cases.json') as f:
        cases = select(json.load(f)['cases'], args)
    if args.list:
        for c in cases:
            flags = ' '.join(f for f in ('family', 'holdout') if c.get(f))
            print(f"{c['id']:24s} {c['tier']:5s} {c['title']:30s} {','.join(c.get('images', [])):30s} {flags}")
        return
    if not cases:
        log('🤷 no case selected'); return 2
    if not os.path.exists(JIT) and modes != ['cur']:
        log(f'⚠️  {JIT} missing: cannot compare AOT; use --modes cur for timing only'); return 2
    if args.setup or not os.path.exists(f'{A}/.setup_done'):
        log('🧰 generating inputs (not timed)…')
        if subprocess.run(['sh', f'{A}/setup.sh']).returncode:
            log('❌ setup failed; no timings collected')
            return 1

    out = args.out or f"{A}/results/{time.strftime('%Y%m%d-%H%M%S')}"
    os.makedirs(os.path.dirname(out), exist_ok=True)
    info = env_info()
    log(f"🔬 AOJIT bench: {len(cases)} cases, modes {modes}, best of {args.repeat}\n   {info['version']}")
    results, port = [], 9300 + os.getpid() % 500 * 10
    bar = tqdm(cases, file=sys.stderr, unit='case', dynamic_ncols=True) if tqdm else cases
    for c in bar:
        if tqdm:
            bar.set_description(c['id'])
        miss = missing_needs(c)
        if miss:
            log(f"⏭  {c['id']}: skipped, needs {miss}")
            results.append({'id': c['id'], 'title': c['title'], 'skipped': miss})
            continue
        log(f"▶ {c['id']}  {c['title']}")
        rec, port = run_case(c, args, modes, port)
        results.append(rec)
        sp = f"  → {rec['speedup']:.2f}×" if rec.get('speedup') else ''
        log('   best ' + ' '.join(f"{m}={v:.2f}s" for m, v in rec['best'].items() if v) + sp +
            ('  ⚠️ output differs' if rec['same_output'] is False else
             '  unverified' if rec['same_output'] is None else ''))
    if 'cur' not in modes:
        set_mode('on')

    text = summarize(results, modes)
    with open(out + '.json', 'w') as f:
        json.dump({'env': info, 'modes': modes, 'repeat': args.repeat, 'cases': results,
                   'jit_after': read(JIT)}, f, ensure_ascii=False, indent=1)
    with open(out + '.txt', 'w') as f:
        f.write(f"{info['version']}\n{info['date']}\n{' '.join(info['packages'])}\n\n{text}\n")
    log('\n' + text + f'\n\n📄 {out}.json / .txt')
    if any(r.get('status') == 'failed' for r in results):
        return 1
    if any(r.get('skipped') or r.get('status') == 'unverified' for r in results):
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())

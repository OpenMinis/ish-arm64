#!/bin/sh
# In the app: fetch the suite, align the packages with the images, run, send the results back.
#
# usage (inside iSH):  scp <host>:<repo>/benchmark/aojit/phone.sh /tmp/ && sh /tmp/phone.sh <host> <repo dir on host> [run_cases.py args]
#   host        ssh destination the suite is copied from and the results go to (<host>:tmp/aojit_results/)
#   OLDLIB=<dir on host>   also fetch older libz/libpython/busybox for the family cases
#   NO_APK=1               keep the installed package versions
# Progress stays on the terminal; the results are also left in /tmp/aojit/results/.
set -e
[ $# -ge 2 ] || { sed -n '2,9p' "$0"; exit 1; }
host=$1 repo=$2; shift 2
echo "📥 fetching the suite from $host…" >&2
mkdir -p /tmp/aojit
scp -q -r "$host:$repo/benchmark/aojit/guest/*" /tmp/aojit/
scp -q "$host:$repo/benchmark/aojit/images.json" /tmp/aojit/
if [ -n "$OLDLIB" ]; then
    mkdir -p /tmp/aojit/oldlib && scp -q "$host:$OLDLIB/*" /tmp/aojit/oldlib/ && chmod +x /tmp/aojit/oldlib/busybox
fi
if [ -z "$NO_APK" ]; then
    echo "📦 aligning packages with the images…" >&2
    apk add -q -u $(python3 -c "import json; print(' '.join(json.load(open('/tmp/aojit/images.json'))['packages']['apk']))") >&2
    for p in $(python3 -c "import json; print(' '.join(json.load(open('/tmp/aojit/images.json'))['packages']['pip']))"); do
        python3 -c "import ${p#python-}" 2>/dev/null || pip install -q --break-system-packages "$p" >&2
    done
fi
out=/tmp/aojit/results/phone-$(date +%Y%m%d-%H%M%S)
python3 /tmp/aojit/run_cases.py --setup --out "$out" "$@"
ssh "$host" mkdir -p tmp/aojit_results && scp -q "$out.json" "$out.txt" "$host:tmp/aojit_results/" && echo "📤 results -> $host:tmp/aojit_results/$(basename $out).{json,txt}" >&2

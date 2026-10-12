#!/bin/bash
# A long-lived ish against a fresh ish per command (what Minis does), on the
# Windows test machine (MSYS2 bash): five commands, ROUNDS times each way.
# Prints "F round name ms" (fresh) and "P round name ms" (long-lived) lines.
#
#   benchmark/daily/persist.sh [ISH.exe] [ROOTFS]     ROUNDS=5
#
# The long-lived run's commands are timed from outside (guest/persist.sh
# prints markers, persist_stamp.py stamps them): busybox date has no %N.
here=$(cd "$(dirname "$0")" && pwd)
ish=${1:-$here/../../build-x86_64-jit/ish.exe}
rootfs=${2:-C:/src/rootfs/daily-x86_64}
export MSYS2_ARG_CONV_EXCL='*' MSYS2_ENV_CONV_EXCL='*'
env="/usr/bin/env -i HOME=/root PATH=/usr/bin:/bin LANG=C.UTF-8"
"$ish" -f "$rootfs" /usr/bin/tee /tmp/daily/persist.sh < "$here/guest/persist.sh" > /dev/null

"$ish" -f "$rootfs" -d /tmp/daily $env ROUNDS=${ROUNDS:-5} /bin/sh /tmp/daily/persist.sh 2>/dev/null |
    /clang64/bin/python3 "$(cygpath -m "$here/persist_stamp.py")"

f() { n=$1; s=$(date +%s%N); "$ish" -f "$rootfs" -d /tmp/daily $env /bin/sh -c "$2" >/dev/null 2>&1; e=$(date +%s%N); echo "F $r $n $(((e - s) / 1000000))"; }
for r in $(seq ${ROUNDS:-5}); do
    f node0 'node -e 0'
    f pypass 'python3 -c pass'
    f pyimport "python3 -c 'import json,re,os,sys,subprocess'"
    f nodejson 'node js/json.js data/data.json'
    f npm 'rm -rf /tmp/np; mkdir /tmp/np && cd /tmp/np && npm install --offline --no-audit --no-fund --loglevel=error /tmp/daily/npm/*.tgz'
done

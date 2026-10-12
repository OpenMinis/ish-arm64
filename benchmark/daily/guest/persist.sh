#!/bin/sh
# One long-lived ish: the same five commands, ROUNDS times; persist_stamp.py times them.
cd /tmp/daily
# the host stamps these lines as they arrive (busybox date has no %N)
t() { n=$1; shift; echo "B $round $n"; "$@" >/dev/null 2>&1; echo "E $round $n"; }
npm_i() { rm -rf /tmp/np; mkdir /tmp/np && cd /tmp/np && npm install --offline --no-audit --no-fund --loglevel=error /tmp/daily/npm/*.tgz; cd /tmp/daily; }
round=1
while [ $round -le ${ROUNDS:-5} ]; do
    t node0 node -e 0
    t pypass python3 -c pass
    t pyimport python3 -c 'import json,re,os,sys,subprocess'
    t nodejson node js/json.js data/data.json
    t npm npm_i
    echo "M $round done"
    round=$((round + 1))
done
echo "M end"; sleep 6

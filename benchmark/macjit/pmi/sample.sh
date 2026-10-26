#!/bin/bash
# sample.sh <out.ktrace> <period> <command...>: PMI samples of the command every <period>
# retired instructions (pmisample.c; PMISAMPLE_EVENT=<kpep event> for another
# event), recorded with ktrace. Needs sudo.
set -e
out=$1; period=$2; shift 2
here=$(cd "$(dirname "$0")" && pwd)
bin=${PMISAMPLE_BIN:-/tmp/pmisample}
[ -x "$bin" ] || cc -O2 -o "$bin" "$here/pmisample.c"
sudo ktrace reset >/dev/null 2>&1 || true
sudo ktrace init -b 4096
sudo ktrace setopt -f C0x25,C0x7
sudo ktrace enable
sudo -E "$bin" "${PMISAMPLE_EVENT:-INST_ALL}" "$period" "$@" || true
sudo ktrace disable
sudo rm -f "$out"
sudo ktrace dump "$out" -E >/dev/null 2>&1
sudo ktrace remove
sudo chown "$USER" "$out"

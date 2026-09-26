#!/bin/sh
# Untimed inputs of every case (deterministic seeds; certificates are fresh
# per install). Runs inside the guest; run_cases.py calls it once and leaves
# /tmp/aojit/.setup_done. usage: setup.sh [group...]
A=/tmp/aojit
say() { echo "🧰 setup: $*" >&2; }
has_py() { python3 -c "import $1" 2>/dev/null; }
want() { [ -z "$GROUPS" ] && return 0; for g in $GROUPS; do [ "$g" = "$1" ] && return 0; done; return 1; }
GROUPS="$*"
set --
export MPLCONFIGDIR=$A/.mpl
mkdir -p $A/busybox $MPLCONFIGDIR
seq 1 60000 > $A/busybox/n60k.txt
if want charts || want shell; then
    has_py numpy && { say charts; (cd $A/charts && python3 gen.py && python3 mpl_gen.py) >&2; }
    # bzip2 input: 8 MB of that JSON (any text of this size does)
    if [ -f $A/charts/mpl_data.json ]; then head -c 8000000 $A/charts/mpl_data.json > $A/shell/bz.in
    else python3 -c "import json; open('$A/shell/bz.in', 'w').write(json.dumps([[i, str(i) * 3] for i in range(400000)])[:8000000])"; fi
fi
if want py || want node; then
    say "py/node inputs"; python3 $A/py/gen_data.py >&2
    has_py PIL && has_py cryptography && python3 $A/py/setup.py >&2
fi
if want net; then command -v openssl > /dev/null && { say certs; sh $A/net/gen_certs.sh >&2; }; fi
if want pil && has_py PIL; then
    say "pil inputs"; rm -rf $A/pil/in $A/pil/train
    python3 $A/pil/gen_inputs.py 4 1200 900 in 1 >&2
    python3 $A/pil/gen_inputs.py 6 900 700 train 2 >&2
fi
touch $A/.setup_done
say "done ✅"

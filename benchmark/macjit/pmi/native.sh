#!/bin/sh
# Native user-mode instruction counts of the ledger workloads, by valgrind
# callgrind on the same Alpine aarch64 binaries ish runs:
#   docker run --rm -v $PWD/benchmark/macjit/pmi:/led macjit-bench sh /led/native.sh
apk add -q valgrind >/dev/null 2>&1
cnt() { # label, command...
  label=$1; shift
  rm -f /tmp/cg.*
  valgrind --tool=callgrind --cache-sim=yes --branch-sim=yes --trace-children=yes --smc-check=all-non-file --callgrind-out-file=/tmp/cg.%p "$@" >/dev/null 2>/tmp/vg.err
  total=$(grep -h "^summary:" /tmp/cg.* | awk "{for(i=2;i<=NF;i++) s[i]+=\$i} END{for(i=2;i<=14;i++) printf \"%s \", s[i]}")
  n=$(ls /tmp/cg.* | wc -l)
  echo "$label $(head -1 /dev/null)$(grep -h "^events:" /tmp/cg.* | head -1 | cut -d" " -f2- | tr " " ","); $total processes=$n"
}
cd /opt/macjit/guest
NODEFLAGS="--no-sparkplug --no-maglev --no-opt --regexp-interpret-all --max-old-space-size=512 --no-concurrent-marking --no-concurrent-recompilation --no-lazy-compile-dispatcher --single-generation"
cnt py_nbody python3 py_compute.py nbody
cnt py_startup sh -c 'for i in $(seq 20); do python3 -c pass; done'
cnt python_c_pass python3 -c pass
cnt node_compute node node_bench.js compute
cnt node_compute_ishflags node $NODEFLAGS node_bench.js compute
cnt coremark_4t_40k /opt/macjit/bin/coremark_mt4 0x0 0x0 0x66 40000
cnt coremark_1t_40k /opt/macjit/bin/coremark_st 0x0 0x0 0x66 40000

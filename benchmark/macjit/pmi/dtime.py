# Wall time of the ledger workloads, run unchanged in docker and in ish:
#   docker run --rm -v $PWD/benchmark/macjit/pmi:/led macjit-bench python3 /led/dtime.py 3
#   ish -r <rootfs> /usr/bin/python3 /tmp/dtime.py 3      (copied into the rootfs)
import subprocess, time, statistics, sys
F = "--no-sparkplug --no-maglev --no-opt --regexp-interpret-all --max-old-space-size=512 --no-concurrent-marking --no-concurrent-recompilation --no-lazy-compile-dispatcher --single-generation"
C = [('py_nbody', 'python3 /opt/macjit/guest/py_compute.py nbody'),
     ('py_startup', 'for i in $(seq 20); do python3 -c pass; done'),
     ('node_compute', 'cd /opt/macjit/guest; node node_bench.js compute'),
     ('node_compute_ishflags', f'cd /opt/macjit/guest; node {F} node_bench.js compute'),
     ('coremark_1t_40k', '/opt/macjit/bin/coremark_st 0x0 0x0 0x66 40000'),
     ('coremark_4t_40k', '/opt/macjit/bin/coremark_mt4 0x0 0x0 0x66 40000')]
for n, c in C:
    ts = []
    for i in range(int(sys.argv[1]) if len(sys.argv) > 1 else 3):
        t = time.perf_counter(); subprocess.run(['/bin/sh', '-c', c], capture_output=True); ts.append(time.perf_counter() - t)
    print(n, ' '.join(f'{x:.3f}' for x in ts), 'median', f'{statistics.median(ts):.3f}', flush=True)

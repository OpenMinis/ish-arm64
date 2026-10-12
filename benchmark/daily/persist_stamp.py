# Stamps the B/E marker lines of persist.sh as they arrive: "P round name ms".
import sys, time
start = {}
for line in sys.stdin:
    now = time.perf_counter()
    p = line.split()
    if len(p) == 3 and p[0] == 'B':
        start[(p[1], p[2])] = now
    elif len(p) == 3 and p[0] == 'E':
        print(f'P {p[1]} {p[2]} {(now - start[(p[1], p[2])]) * 1000:.0f}', flush=True)
    else:
        print(line.rstrip(), flush=True)

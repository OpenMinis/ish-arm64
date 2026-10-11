# Hot-code benchmark: every case runs once to warm up (so the JIT / translator
# has seen all of its code), then N more times in the same process; prints
# "group|case|warm ms per run|total ms|cold ms" lines. Shell/C cases are
# separate processes (translations don't outlive a process): they run three
# times, cold = the first, warm = the best of the other two.
import json, os, re, sqlite3, subprocess, sys, time, random, math

def bench(group, name, n, fn, *a):
    t0 = time.perf_counter(); fn(*a); cold = time.perf_counter() - t0
    t0 = time.perf_counter()
    for _ in range(n):
        fn(*a)
    total = time.perf_counter() - t0
    print(f"{group}|{name}|{total / n * 1000:.3f}|{total * 1000:.1f}|{cold * 1000:.1f}", flush=True)

random.seed(1)
DOC = [{"id": i, "name": f"user{i}", "tags": ["a", "b", str(i % 7)], "score": i * 1.5,
        "nested": {"ok": i % 2 == 0, "v": [i, i + 1, i + 2]}} for i in range(200)]
def t_json():
    json.loads(json.dumps(DOC))

LOG = "\n".join(f"2026-10-11 12:{i % 60:02d}:{i % 59:02d} {'ERROR' if i % 17 == 0 else 'INFO'} "
                f"req={i} ip=10.0.{i % 255}.{(i * 7) % 255} path=/api/v{i % 3}/item/{i * 13} took={i % 997}ms"
                for i in range(12000))
PATS = [re.compile(r"ERROR .*?ip=(\d+\.\d+\.\d+\.\d+)"), re.compile(r"took=(\d{3})ms"),
        re.compile(r"/api/v(\d)/item/(\d+)")]
def t_regex():
    for p in PATS:
        len(p.findall(LOG))

def t_sqlite():
    db = sqlite3.connect(":memory:")
    db.execute("create table t(a integer, b text, c real)")
    db.executemany("insert into t values(?,?,?)", ((i, f"k{i % 50}", i * 0.5) for i in range(5000)))
    db.execute("select b, count(*), sum(c), avg(a) from t group by b order by 3 desc").fetchall()
    db.execute("update t set c = c * 2 where a % 3 = 0")
    db.execute("select count(*) from t where c > 1000").fetchone()
    db.close()

def fib(n): return n if n < 2 else fib(n - 1) + fib(n - 2)
def t_fib(): fib(22)

def t_primes():
    n = 200000; s = bytearray([1]) * (n + 1); s[0:2] = b"\0\0"
    for i in range(2, int(n ** 0.5) + 1):
        if s[i]: s[i * i::i] = bytes(len(range(i * i, n + 1, i)))
    sum(s)

def t_nbody():
    bodies = [[0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 39.47], [4.84, -1.16, -0.10, 0.60, 2.81, -0.02, 0.037],
              [8.34, 4.12, -0.40, -1.01, 1.82, 0.008, 0.011], [12.89, -15.11, -0.22, 1.08, 0.86, -0.01, 0.0017],
              [15.37, -25.91, 0.17, 0.97, 0.59, -0.03, 0.002]]
    dt = 0.01
    for _ in range(1000):
        for i in range(5):
            bi = bodies[i]
            for j in range(i + 1, 5):
                bj = bodies[j]
                dx, dy, dz = bi[0] - bj[0], bi[1] - bj[1], bi[2] - bj[2]
                d2 = dx * dx + dy * dy + dz * dz; mag = dt / (d2 * math.sqrt(d2))
                bi[3] -= dx * bj[6] * mag; bi[4] -= dy * bj[6] * mag; bi[5] -= dz * bj[6] * mag
                bj[3] += dx * bi[6] * mag; bj[4] += dy * bi[6] * mag; bj[5] += dz * bi[6] * mag
        for b in bodies:
            b[0] += dt * b[3]; b[1] += dt * b[4]; b[2] += dt * b[5]

# WARM_ONLY=py|proc runs one half (a JIT code region can fill up over both)
ONLY = os.environ.get("WARM_ONLY", "")
run_py = ONLY in ("", "py")
if run_py:
    bench("Python", "json dumps+loads x1000", 1000, t_json)
    bench("Python", "regex scan x50", 50, t_regex)
    bench("Python", "sqlite batch x20", 20, t_sqlite)
    bench("Python", "fib(22) x20", 20, t_fib)
    bench("Python", "prime sieve 200K x20", 20, t_primes)
    bench("Python", "nbody 1000 steps x10", 10, t_nbody)
try:
    if not run_py: raise ImportError
    from PIL import Image, ImageFilter
    IMG = Image.new("RGB", (640, 480))
    IMG.putdata([((x * 7) % 256, (y * 3) % 256, (x ^ y) % 256) for y in range(480) for x in range(640)])
    def t_pil():
        IMG.resize((320, 240), Image.LANCZOS).filter(ImageFilter.GaussianBlur(2)).convert("L").tobytes()
    bench("Python", "Pillow resize+blur x20", 20, t_pil)
except ImportError:
    pass
try:
    if not run_py: raise ImportError
    import pandas as pd, numpy as np
    rng = np.random.default_rng(1)
    DF = pd.DataFrame({"k": rng.integers(0, 100, 50000), "v": rng.random(50000), "w": rng.random(50000)})
    def t_pandas():
        g = DF.groupby("k").agg({"v": ["sum", "mean"], "w": "max"})
        (DF["v"] * DF["w"]).rolling(50).mean().sum(); g.sort_values(("v", "sum")).head()
    bench("Python", "pandas groupby+rolling x20", 20, t_pandas)
except ImportError:
    pass

# separate processes: cold = first run, warm = best of the next two
def proc(group, name, cmd):
    ts = []
    for _ in range(3):
        t0 = time.perf_counter()
        subprocess.run(["sh", "-c", cmd], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        ts.append((time.perf_counter() - t0) * 1000)
    print(f"{group}|{name}|{min(ts[1:]):.1f}|{sum(ts):.1f}|{ts[0]:.1f}", flush=True)

if ONLY == "py":
    sys.exit(0)
# the same implementations on every rootfs: busybox applets, not coreutils / GNU bc
open("/tmp/warm_lines.txt", "w").write("\n".join(f"{i},user{i % 977},{'GET' if i % 3 else 'POST'},{i * 13 % 9973}" for i in range(200000)))
proc("Shell", "awk loop 3M", "busybox awk 'BEGIN{for(i=0;i<3000000;i++)s+=i%7; print s}'")
proc("Shell", "awk csv aggregate 200K", "busybox awk -F, '{s[$3]+=$4; c[$2]++} END{for(k in s) print k, s[k]}' /tmp/warm_lines.txt")
proc("Shell", "sed regex 200K lines", "busybox sed -e 's/user\\([0-9]*\\)/U\\1/g' -e '/POST/d' /tmp/warm_lines.txt | wc -l")
proc("Shell", "sort 200K lines", "busybox sort -t, -k4 -n /tmp/warm_lines.txt | tail -1")
proc("Shell", "bc pi 1500 digits", "echo 'scale=1500; 4*a(1)' | busybox bc -l")
cm = sys.argv[1] if len(sys.argv) > 1 else ""
if cm and os.path.exists(cm):
    out = subprocess.run([cm], capture_output=True, text=True).stdout
    m = re.search(r"Iterations/Sec\s*:\s*([\d.]+)", out)
    if m:
        print(f"C|CoreMark (iter/s)|{m.group(1)}|0|0", flush=True)

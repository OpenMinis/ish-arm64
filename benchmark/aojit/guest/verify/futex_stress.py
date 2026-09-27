# futex-heavy: locks, condition variables and semaphores across threads; deterministic result
import threading, queue
N, T = 20000, 8
lock = threading.Lock(); total = [0]
def add():
    for _ in range(N):
        with lock: total[0] += 1
ts = [threading.Thread(target=add) for _ in range(T)]; [t.start() for t in ts]; [t.join() for t in ts]
q = queue.Queue(maxsize=4); out = []
def prod():
    for i in range(3000): q.put(i)
    q.put(None)
def cons():
    s = 0
    while (x := q.get()) is not None: s += x
    out.append(s)
p, c = threading.Thread(target=prod), threading.Thread(target=cons); p.start(); c.start(); p.join(); c.join()
cv = threading.Condition(); turn = [0]; seq = []
def ping(me):
    for _ in range(500):
        with cv:
            cv.wait_for(lambda: turn[0] == me); seq.append(me); turn[0] = 1 - me; cv.notify_all()
a, b = threading.Thread(target=ping, args=(0,)), threading.Thread(target=ping, args=(1,)); a.start(); b.start(); a.join(); b.join()
sem = threading.Semaphore(2); ev = threading.Event(); hits = []
def w(i):
    ev.wait()
    with sem: hits.append(i)
ws = [threading.Thread(target=w, args=(i,)) for i in range(16)]; [t.start() for t in ws]; ev.set(); [t.join() for t in ws]
print(total[0], out[0], len(seq), seq[:4], sorted(hits) == list(range(16)))

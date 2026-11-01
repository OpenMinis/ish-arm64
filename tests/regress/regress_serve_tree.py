#!/usr/bin/env python3
"""FIX-261101-04: `signal to:tree` in ish.exe --serve (Windows only).

    python regress_serve_tree.py <ish.exe> <rootfs dir>

Stop and a timeout must reach everything a command started, setsid and double
forks included, while what a command leaves behind after a normal exit stays.
Every case drives a real ish.exe --serve over its JSON lines; a process is
told apart by a sleep length nothing else uses. The rootfs needs a shell,
sleep and setsid (Alpine's busybox), and is written to: give it a copy.
"""
import json
import queue
import subprocess
import sys
import threading
import time

failures = 0


def check(what, ok):
    global failures
    print(("  ok    " if ok else "  FAIL  ") + what, flush=True)
    if not ok:
        failures += 1


class Serve:
    def __init__(self, exe, rootfs):
        self.p = subprocess.Popen([exe, "-f", rootfs, "--serve"], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, encoding="utf-8")
        self.events = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()
        self.ready = self.wait(lambda e: e.get("ev") == "ready", 30)
        self.n = 0

    def _read(self):
        for line in self.p.stdout:
            try:
                self.events.put(json.loads(line))
            except ValueError:
                pass

    def send(self, **req):
        self.p.stdin.write(json.dumps(req) + "\n")
        self.p.stdin.flush()

    def wait(self, match, secs):
        end = time.time() + secs
        while time.time() < end:
            try:
                e = self.events.get(timeout=max(0.01, end - time.time()))
            except queue.Empty:
                break
            if match(e):
                return e
        return None

    def start(self, script):
        self.n += 1
        cid = "c%d" % self.n
        self.send(op="exec", id=cid, argv=["/bin/sh", "-c", script], env=["PATH=/usr/bin:/bin"],
                  cwd="/", stdin=None, stdout=None, stderr=None)
        started = self.wait(lambda e: e.get("id") == cid and e.get("ev") in ("started", "error"), 20)
        if started is None or started["ev"] != "started":
            raise SystemExit("exec failed: %r" % (started,))
        return cid

    def exit_of(self, cid, secs=20):
        return self.wait(lambda e: e.get("id") == cid and e.get("ev") == "exit", secs)

    def alive(self, seconds):
        """Is a `sleep <seconds>` running? Asked from a command of its own."""
        cid = self.start("for f in /proc/[0-9]*/cmdline; do "
                         "tr '\\0' ' ' < $f 2>/dev/null | grep -q '^sleep %d ' && exit 0; done; exit 1" % seconds)
        e = self.exit_of(cid)
        return e is not None and e.get("rc") == 0

    def gone_within(self, seconds, secs):
        end = time.time() + secs
        while time.time() < end:
            if not self.alive(seconds):
                return True
            time.sleep(0.2)
        return False

    def tracked(self):
        self.send(op="stats", id="s%d" % time.monotonic_ns())
        e = self.wait(lambda v: v.get("ev") == "stats", 10)
        return set((e or {}).get("cmds", {}))


def main():
    s = Serve(sys.argv[1], sys.argv[2])
    check("ready says tree_signal", s.ready is not None and s.ready.get("tree_signal") is True)

    print("normal exit, children left running")
    c = s.start("sleep 6001 & setsid sleep 6002 & (sh -c 'sleep 6003 &' &) ; sleep 1; exit 0")
    e = s.exit_of(c)
    check("the command exits 0", e is not None and e["rc"] == 0)
    time.sleep(1)
    check("a background child stays", s.alive(6001))
    check("a setsid child stays", s.alive(6002))
    check("a double-forked daemon stays", s.alive(6003))

    print("cancel with a setsid child")
    c = s.start("setsid sleep 6011 & sleep 6012")
    time.sleep(1)
    check("it runs", s.alive(6011))
    s.send(op="signal", id=c, sig=15, to="tree", kill_after_ms=1000)
    check("the command ends", s.exit_of(c) is not None)
    check("the setsid child goes", s.gone_within(6011, 4))

    print("cancel with a double-forked daemon")
    c = s.start("(sh -c 'setsid sleep 6021 &' &) ; sleep 6022")
    time.sleep(1)
    check("it runs", s.alive(6021))
    s.send(op="signal", id=c, sig=15, to="tree", kill_after_ms=1000)
    check("the command ends", s.exit_of(c) is not None)
    check("the daemon goes", s.gone_within(6021, 4))

    print("a setsid child that ignores SIGTERM, the command ending at once")
    c = s.start("setsid sh -c 'trap \"\" TERM; exec sleep 6031' & sleep 6032")
    time.sleep(1)
    check("it runs", s.alive(6031))
    s.send(op="signal", id=c, sig=15, to="tree", kill_after_ms=1000)
    check("the command ends on the SIGTERM", s.exit_of(c, 1) is not None)
    check("the child is still there right after", s.alive(6031))
    check("ish's own SIGKILL takes it after the grace", s.gone_within(6031, 5))
    time.sleep(0.5)
    check("the command's entry went with its tree", c not in s.tracked())

    print("a child in the session that ignores SIGTERM: gone still comes (the client stops tracking, idle-out can run)")
    c = s.start("sh -c 'trap \"\" TERM; exec sleep 6036' & sleep 6037")
    time.sleep(1)
    s.send(op="signal", id=c, sig=15, to="tree", kill_after_ms=1000)
    e = s.exit_of(c)
    check("exit counts it as left", e is not None and e.get("left", 0) >= 1)
    check("gone follows the SIGKILL", s.wait(lambda v: v.get("id") == c and v.get("ev") == "gone", 5) is not None)
    time.sleep(0.5)
    check("the entry went too", c not in s.tracked())

    print("a command that ignores SIGTERM, then SIGKILL (a client's escalation)")
    c = s.start("trap '' TERM; setsid sleep 6041 & sleep 6042")
    time.sleep(1)
    s.send(op="signal", id=c, sig=15, to="tree")
    time.sleep(0.5)
    s.send(op="signal", id=c, sig=9, to="tree")
    check("the command ends", s.exit_of(c) is not None)
    check("the child goes", s.gone_within(6041, 4))

    print("a tree that keeps forking")
    c = s.start("setsid sh -c 'while :; do sleep 6051 & sleep 0.05; done' & sleep 6052")
    time.sleep(1)
    s.send(op="signal", id=c, sig=9, to="tree")
    check("the command ends", s.exit_of(c) is not None)
    check("no fork outruns it", s.gone_within(6051, 4))

    print("other commands")
    check("the first command's children still run", s.alive(6001) and s.alive(6002) and s.alive(6003))
    c = s.start("exit 3")
    e = s.exit_of(c)
    s.send(op="signal", id=c, sig=15, to="tree")
    err = s.wait(lambda v: v.get("id") == c and v.get("ev") == "error", 5)
    check("a command that has gone: ESRCH", err is not None and err.get("errno") == 3)
    s.send(op="signal", id="nobody", sig=15, to="tree", kill_after_ms=-1)
    err = s.wait(lambda v: v.get("id") == "nobody" and v.get("ev") == "error", 5)
    check("kill_after_ms is checked", err is not None and err.get("errno") == 22)

    s.send(op="shutdown", grace_ms=500)
    try:
        s.p.wait(20)
    except subprocess.TimeoutExpired:
        s.p.kill()
        check("shutdown", False)
    print("serve tree: %d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

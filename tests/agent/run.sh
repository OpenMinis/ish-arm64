#!/bin/bash
# Agent-runtime checks against ish-agent-harness (meson -Dagent_harness=true).
#   tests/agent/run.sh <build dir> <fakefs rootfs>
# Works for either guest arch; prints one PASS/FAIL line per check.
set -u
BUILD=$1
ROOTFS=$2
H="$BUILD/ish-agent-harness"
ISH="$BUILD/ish"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
pass=0 fail=0

check() { # name, expected substring, actual output
    if [[ "$3" == *"$2"* ]]; then
        echo "PASS $1"; pass=$((pass + 1))
    else
        echo "FAIL $1"; echo "  expected: $2"; echo "  got: $(echo "$3" | tail -5)"; fail=$((fail + 1))
    fi
}
run() { timeout 120 "$@" 2>&1 | grep -v -e NETDIAG -e 'missing syscall'; }

mkdir -p "$WORK/rw" "$WORK/ro" "$WORK/ctx"
echo host-rw > "$WORK/rw/in.txt"
echo host-ro > "$WORK/ro/in.txt"
echo routed > "$WORK/ctx/f.txt"

out=$(ISHT_BIND="/mnt/rw=$WORK/rw" ISHT_CHANGES=1 run "$H" -f "$ROOTFS" /bin/sh -c \
    'cat /mnt/rw/in.txt; echo guest > /mnt/rw/out.txt; mkdir -p /mnt/rw/d; mv /mnt/rw/out.txt /mnt/rw/d/; ls /mnt/rw/d; sleep 1')
check bind-read host-rw "$out"
check bind-readdir out.txt "$out"
check bind-host-sees-write guest "$(cat "$WORK/rw/d/out.txt" 2>&1)"
check bind-change-feed "ISHT change op=2" "$out"

out=$(ISHT_BIND="/mnt/ro=$WORK/ro:ro" run "$H" -f "$ROOTFS" /bin/sh -c \
    'cat /mnt/ro/in.txt; touch /mnt/ro/x && echo WROTE || echo DENIED')
check bind-ro-read host-ro "$out"
check bind-ro-deny DENIED "$out"

script='i=0; while [ $i -lt 3000 ]; do i=$((i+1)); done; cat /ctx/f.txt 2>&1; echo new > /ctx/g.txt; readlink -f /ctx/g.txt'
out=$(ISHT_ROUTE="/ctx=$WORK/ctx" ISHT_CTX=1 run "$H" -f "$ROOTFS" /bin/sh -c "$script")
check route-ctx1-read routed "$out"
check route-ctx1-write new "$(cat "$WORK/ctx/g.txt" 2>&1)"
out=$(ISHT_ROUTE="/ctx=$WORK/ctx" run "$H" -f "$ROOTFS" /bin/sh -c 'cat /ctx/f.txt 2>&1')
check route-ctx0-isolated "No such file" "$out"

out=$(ISHT_FORK_LIMIT=2 run "$H" -f "$ROOTFS" /bin/sh -c \
    '/bin/true && echo f1; /bin/true && echo f2; /bin/true && echo f3 || echo DENIED3')
check fork-guard-allows f2 "$out"
check fork-guard-denies "can't fork" "$out"

# The shell still needs a file on PATH before execve can be intercepted.
out=$(ISHT_HANDLER=1 run "$H" -f "$ROOTFS" /bin/sh -c \
    'printf "#!/bin/sh\nexit 1\n" > /usr/local/bin/nativeecho; chmod +x /usr/local/bin/nativeecho; nativeecho a /tmp; echo rc=$?')
check offload-handler "nativeecho argc=3 [a]" "$out"
check offload-handler-rc rc=7 "$out"

out=$(run "$ISH" -n "uname=/usr/bin/uname" -f "$ROOTFS" /bin/sh -c 'uname -s')
check offload-host-binary Darwin "$out"

out=$(ISHT_TICKS=1 run "$H" -f "$ROOTFS" /bin/sh -c 'i=0; while [ $i -lt 2000 ]; do i=$((i+1)); done')
check timer-tick-hook "ISHT ticks=" "$out"

timeout 60 "$ISH" -f "$ROOTFS" /bin/sh -c 'exit 3' >/dev/null 2>&1
check exit-status rc=3 "rc=$?"
timeout 60 "$ISH" -f "$ROOTFS" /bin/sh -c 'kill -9 $$' >/dev/null 2>&1
check exit-signal rc=137 "rc=$?"

out=$(run "$ISH" -f "$ROOTFS" /bin/sh -c 'cat /proc/meminfo | head -1')
check proc-meminfo MemTotal "$out"

echo "== $pass passed, $fail failed"
[ $fail -eq 0 ]

# [T-ish-fork-rate] Fork-rate governor: run by run_fs_perf.sh with ISH_FORK_RATE=150,300 in the
# host environment and /tmp/now_ms injected. A 40-way fork+exec storm (60 execs each) must be held
# near the limit, while a light "tool call" shell started every second must stay fast (it bypasses
# the queue). Without the governor the storm finishes in <2s on a Mac and the probe takes >200ms.
mkdir -p /tmp/fr; rm -f /tmp/fr/*
( while :; do s=$(/tmp/now_ms); /bin/sh -c '/bin/ls /bin >/dev/null; /bin/cat /etc/passwd >/dev/null; /bin/true'; e=$(/tmp/now_ms); /bin/echo $((e-s)) >> /tmp/fr/probe; /bin/sleep 1; done ) & PR=$!
T0=$(/tmp/now_ms)
i=0; while [ $i -lt 40 ]; do
  ( n=0; while [ $n -lt 60 ]; do /bin/true; n=$((n+1)); done; /bin/echo done > /tmp/fr/w$i ) &
  i=$((i+1)); done
while [ $(ls /tmp/fr | grep -c '^w') -lt 40 ]; do /bin/sleep 1; done
T1=$(/tmp/now_ms); kill $PR
el=$((T1-T0)); rate=$((2400 * 1000 / el))
mx=$(sort -n /tmp/fr/probe | tail -1); n=$(wc -l < /tmp/fr/probe)
echo "fork_rate: 2400 execs in ${el}ms = ${rate}/s (limit 150, burst 300); probe max ${mx}ms over $n probes"
ok=1
[ $rate -le 260 ] || { echo "FAIL: storm ran at ${rate}/s, governor not holding it near 150/s"; ok=0; }
[ $rate -ge 100 ] || { echo "FAIL: storm only ${rate}/s, governor over-throttling"; ok=0; }
[ "$mx" -lt 300 ] || { echo "FAIL: light probe took ${mx}ms, bypass not working"; ok=0; }
[ $ok = 1 ] && echo FORK_RATE_OK
[ $ok = 1 ]

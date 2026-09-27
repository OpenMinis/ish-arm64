# grep recording workload (busybox grep over musl's regex engine): the options agents use (device log
# 2026-09-27) with patterns and trees unlike measure/grep.sh: basic and extended regexes, -i, -w, -v,
# -o, -c, -l, -L, -C, --include, -F, -x, -e lists, and a large-file scan.
W=/tmp/aojit/busybox/grepwork; rm -rf $W; mkdir -p $W; cd $W
P=/usr/lib/python3.12
awk 'BEGIN { srand(3); for (i = 0; i < 150000; i++) printf "%s [%s] id=%d user=u%03d %s\n", (i % 7 ? "INFO" : "WARN"), (rand() < 0.1 ? "ERROR" : "ok"), i, int(rand() * 900), (rand() < 0.5 ? "GET /api/v1/items" : "POST /api/v2/login") }' > log.txt
grep -rn 'raise ValueError' $P/json $P/email > /dev/null; grep -rn -i 'deprecat' $P/logging > /dev/null
grep -rc 'import' --include='*.py' $P/http $P/json > /dev/null; grep -rl 'class .*Error' $P/email > /dev/null
grep -rL 'import os' $P/json > /dev/null; grep -rn -w 'self' $P/json | wc -l > /dev/null
grep -roE '[a-z_]+\(self' $P/email | sort | uniq -c | sort -rn | head -3 > /dev/null
grep -rn -E '^(def|class) [A-Z]' $P/http > /dev/null; grep -n -C 2 -e 'lambda' -e 'yield' $P/json/decoder.py > /dev/null
grep -c 'ERROR' log.txt > /dev/null; grep -v 'INFO' log.txt | wc -l > /dev/null; grep -oE 'id=[0-9]+' log.txt | tail -1 > /dev/null
grep -iE 'post /api/v[0-9]/(login|items)' log.txt | wc -l > /dev/null; grep -F 'user=u042' log.txt | wc -l > /dev/null
grep -x 'nothing' log.txt > /dev/null; grep -E '(ok|ERROR)\] id=1[0-9]{4} ' log.txt | wc -l > /dev/null
grep -n 'user=u9[0-9][0-9]' log.txt | head -3 > /dev/null; grep -q 'WARN' log.txt
cd /; rm -rf $W

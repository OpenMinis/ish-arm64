# busybox workload for AOT recording: ash scripting plus the common applets.
W=/tmp/aojit/busybox/work; rm -rf $W; mkdir -p $W; cd $W
# --- ash: loops, arithmetic, strings, case, functions
fib() { a=0; b=1; i=0; while [ $i -lt $1 ]; do t=$((a + b)); a=$b; b=$t; i=$((i + 1)); done; echo $a; }
s=0; i=0
while [ $i -lt 20000 ]; do
    case $((i % 4)) in 0) s=$((s + i)) ;; 1) s=$((s - 1)) ;; *) s=$((s ^ i)) ;; esac
    v="item-$i"; v=${v#item-}; v=${v%0}; i=$((i + 1))
done
echo $s $(fib 80)
for w in alpha beta gamma delta; do printf '%s:%d\n' "$w" "${#w}"; done > words.txt
# --- data
seq 1 200000 > n.txt
awk 'BEGIN { srand(1); for (i = 0; i < 100000; i++) printf "%d,u%d,%s,%.2f,%s\n", i, int(rand() * 500), (rand() < 0.5 ? "GET" : "POST"), rand() * 900, (rand() < 0.1 ? "ERROR" : "ok") }' > log.csv
# --- text utilities
sort -rn n.txt | head -1; sort -t, -k4 -n log.csv | tail -1; sort -u -t, -k2,2 log.csv | wc -l
cut -d, -f2 log.csv | sort | uniq -c | sort -rn | head -3
tr 'a-z' 'A-Z' < log.csv | tr -d ',' | wc -c
sed -e 's/\([0-9]\)\([0-9]\)/\2\1/g' -e '/ERROR/d' log.csv | wc -l
sed -n '/POST/{s/,/;/g;p}' log.csv | head -2
awk -F, '{ s[$3] += $4; c[$5]++ } END { for (k in s) printf "%s %.1f\n", k, s[k]; for (k in c) print k, c[k] }' log.csv
awk '{ s += $1 } END { print s }' n.txt
grep -c ERROR log.csv; grep -E 'u(1|2)[0-9],POST' log.csv | wc -l; grep -v ok log.csv | head -1; grep -n 'u42,' log.csv | tail -1
head -c 100000 log.csv | wc -w; tail -n 3 log.csv; rev words.txt; paste -d: words.txt words.txt | head -2
xargs -n 1000 echo < n.txt | wc -l
# --- files and find
mkdir -p tree; for d in a b c d e; do mkdir -p tree/$d; for f in $(seq 1 60); do echo "file $d $f $((f * 7))" > tree/$d/f$f.txt; done; done
find tree -type f | wc -l; find tree -name 'f1*.txt' | sort | head -3; find tree -type f -exec grep -l ' 4' {} + | wc -l
grep -r 'file c' tree | wc -l; ls -la tree/a | head -3; du -s tree; cp -r tree tree2; mv tree2 tree3; rm -rf tree3
find /usr/lib/python3.12 -name '*.py' | head -2000 | xargs grep -l 'import os' | wc -l
grep -rc 'def ' /usr/lib/python3.12/json /usr/lib/python3.12/email | tail -2
# --- archives, checksums, misc
tar cf t.tar tree; gzip -c t.tar > t.tar.gz; gunzip -c t.tar.gz | tar tf - | wc -l; tar xzf t.tar.gz -C /tmp/aojit/busybox/work && ls /tmp/aojit/busybox/work/tree | wc -l
md5sum log.csv n.txt; sha256sum log.csv | cut -c1-16; cksum n.txt
date +%s > /dev/null; basename /a/b/c.txt .txt; dirname /a/b/c.txt; expr 7 \* 6; od -c words.txt | head -2
cd /; rm -rf $W

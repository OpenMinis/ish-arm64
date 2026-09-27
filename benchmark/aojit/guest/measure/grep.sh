# Candidate workload (profile.sh): grep as agents use it (the patterns of the 2026-09-27 device log:
# recursive -rn searches, -c counts, -oE extraction, -i -E alternations, -l file lists) over a code
# tree of a few tens of MB. grep_setup.sh makes the tree. On Alpine grep is a busybox applet.
T=/tmp/aojit/measure/tree
[ -d $T ] || { echo "measure/grep.sh: run measure/grep_setup.sh first" >&2; exit 1; }
cd $T
grep -rn 'jit_translate' . > /dev/null
grep -rn -i 'aot\|family' asbestos kernel > /dev/null
grep -rc 'static ' --include='*.c' . > /dev/null
grep -roE '[A-Za-z_]+_lock\(' kernel fs | sort | uniq -c | sort -rn | head -5 > /dev/null
grep -rl 'import os' py > /dev/null
grep -rn -E 'def (__init__|__repr__)' py | wc -l > /dev/null
grep -n -C 3 'pthread_mutex_lock' asbestos/guest-arm64/jit.c > /dev/null
grep -rn -w 'return' . | wc -l > /dev/null
grep -c 'e' big.txt > /dev/null; grep -n 'zzz_not_there' big.txt > /dev/null; grep -oE '[0-9]{4}-[0-9]{2}' big.txt | wc -l > /dev/null

# Candidate workload (profile.sh): find as agents use it (device log: find <dir> -name '*.ext', whole
# tree walks, -type/-newer/-size filters, -exec grep, find | xargs grep) over the tree of grep_setup.sh.
# On Alpine find is a busybox applet.
T=/tmp/aojit/measure/tree
[ -d $T ] || { echo "measure/find.sh: run measure/grep_setup.sh first" >&2; exit 1; }
for i in 1 2 3; do find $T -name '*.c' | wc -l > /dev/null; done
find $T -name 'jit.c' > /dev/null; find $T -iname '*readme*' > /dev/null
find $T -type f | wc -l > /dev/null; find $T -type d -name '__pycache__' > /dev/null
find $T -type f -name '*.py' -size +20k | wc -l > /dev/null
find $T -newer $T/big.txt -type f > /dev/null
find $T -name '*.h' -exec grep -l 'struct' {} + | wc -l > /dev/null
find $T -name '*.c' | xargs grep -l 'malloc' | wc -l > /dev/null
find / -name 'translate_path.c' 2> /dev/null > /dev/null

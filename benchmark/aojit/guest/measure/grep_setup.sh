# Setup for grep.sh / find.sh (not timed): a code tree of a few tens of MB (ish's own sources from
# /tmp/aojit/measure/ish-src.tar, which the host puts there, plus the python stdlib) and a large text file.
T=/tmp/aojit/measure/tree
rm -rf $T && mkdir -p $T/py && cd $T
[ -f /tmp/aojit/measure/ish-src.tar ] && tar xf /tmp/aojit/measure/ish-src.tar
cp -r /usr/lib/python3.12/. $T/py/ 2> /dev/null
awk 'BEGIN { srand(7); for (i = 0; i < 400000; i++) printf "2026-%02d-%02d %06d event=%s value=%d\n", 1 + i % 12, 1 + i % 28, i, (rand() < 0.5 ? "open" : "close"), int(rand() * 100000) }' > big.txt
du -sh $T | cut -f1; find $T -type f | wc -l

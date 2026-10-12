#!/bin/sh
# Inputs for the daily cases, generated in the guest (run from /tmp/daily by
# mkrootfs.sh). Deterministic: the cases print checksums of their output.
# With network, also fetches the wheels and npm tarballs the install cases use.
set -e
cd /tmp/daily
mkdir -p data wheels npm c/proj go/hello

# Text: 200k log lines and a word list, from a fixed LCG (no $RANDOM).
python3 - <<'EOF'
import json
x = 12345
def rnd(n):
    global x
    x = (x * 1103515245 + 12345) & 0x7fffffff
    return x % n
levels = ['INFO', 'WARN', 'ERROR', 'DEBUG']
words = ['alpha', 'beta', 'gamma', 'delta', 'epsilon', 'zeta', 'eta', 'theta', 'iota', 'kappa',
         'lambda', 'mu', 'nu', 'xi', 'omicron', 'pi', 'rho', 'sigma', 'tau', 'upsilon']
with open('data/log.txt', 'w') as f:
    for i in range(200000):
        f.write('2026-10-%02d %02d:%02d:%02d %s svc%d user=%d %s %s took=%dms\n' % (
            1 + rnd(28), rnd(24), rnd(60), rnd(60), levels[rnd(4)], rnd(12), rnd(5000),
            words[rnd(20)], words[rnd(20)], rnd(900)))
with open('data/words.txt', 'w') as f:
    for i in range(200000):
        f.write(words[rnd(20)] + str(rnd(50)) + '\n')
items = [{'id': i, 'name': 'item%d' % i, 'tags': [words[rnd(20)] for _ in range(3)],
          'price': rnd(100000) / 100, 'stock': rnd(500), 'meta': {'a': rnd(10), 'b': words[rnd(20)]}}
         for i in range(30000)]
with open('data/data.json', 'w') as f:
    json.dump({'items': items}, f)
with open('data/big.txt', 'w') as f:
    for i in range(400000):
        f.write('%d %s %s\n' % (i, words[rnd(20)], words[rnd(20)]))
import zipfile, os
with zipfile.ZipFile('data/pkg.zip', 'w', zipfile.ZIP_DEFLATED) as z:
    base = '/usr/lib/python3.12/email'
    for root, dirs, files in sorted(os.walk(base)):
        dirs.sort()
        for n in sorted(files):
            p = os.path.join(root, n)
            z.write(p, os.path.relpath(p, '/usr/lib/python3.12'))
EOF
tar czf data/py.tgz -C /usr/lib/python3.12 json email http urllib xml

# A small C project for make, and a git repository with history.
cat > c/hello.c <<'EOF'
#include <stdio.h>
int main(void) { printf("hello %d\n", 6 * 7); return 0; }
EOF
for n in 1 2 3 4 5 6; do
    cat > c/proj/m$n.c <<EOF
#include <string.h>
int f$n(const char *s) { int h = $n; while (*s) h = h * 31 + *s++; return h & 0xffff; }
EOF
done
cat > c/proj/main.c <<'EOF'
#include <stdio.h>
int f1(const char *), f2(const char *), f3(const char *), f4(const char *), f5(const char *), f6(const char *);
int main(void) { printf("%d\n", f1("a") + f2("bb") + f3("ccc") + f4("dddd") + f5("e") + f6("f")); return 0; }
EOF
cat > c/proj/Makefile <<'EOF'
OBJS = main.o m1.o m2.o m3.o m4.o m5.o m6.o
app: $(OBJS)
	$(CC) -o $@ $(OBJS)
%.o: %.c
	$(CC) -O2 -c $< -o $@
clean:
	rm -f app $(OBJS)
EOF
cat > go/hello/main.go <<'EOF'
package main

import "fmt"

func main() { fmt.Println("hello", 6*7) }
EOF
cat > go/hello/go.mod <<'EOF'
module hello

go 1.21
EOF

rm -rf repo repo.git
mkdir repo && cd repo
git init -q -b main && git config user.email b@d && git config user.name bench
cp -r /usr/lib/python3.12/json /usr/lib/python3.12/email .
git add -A && git commit -qm init
i=0
while [ $i -lt 30 ]; do
    echo "change $i" >> NOTES && git add NOTES && git commit -qm "c$i"
    i=$((i + 1))
done
echo dirty >> json/__init__.py
cd .. && git clone -q --bare repo repo.git

# Install inputs (network). Pure-Python wheels and npm tarballs.
if wget -q -T 10 -O /dev/null https://pypi.org/simple/ 2>/dev/null; then
    pip download -q --no-deps --only-binary=:all: -d wheels \
        idna==3.10 six==1.16.0 packaging==24.1 attrs==24.2.0 click==8.1.7 \
        requests==2.32.3 urllib3==2.2.3 certifi==2024.8.30 charset-normalizer==3.4.0
    (cd npm && npm pack -q left-pad@1.3.0 is-number@7.0.0 ms@2.1.3 lodash@4.17.21 >/dev/null)
fi
echo setup done

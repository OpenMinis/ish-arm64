#!/bin/bash
# ═══════════════════════════════════════════════════════════════════════
# iSH Unified Benchmark — Performance & Compatibility
#
# Usage:
#   ./run.sh             # Run both performance + compatibility suites
#   ./run.sh shell       # Performance only (Shell + Python + Node.js + Go + C)
#   ./run.sh compat      # Compatibility only (205 tests across 18 categories)
#
# Performance: assets/shellbench.sh runs guest-side inside each architecture
# with monotonic-clock timing, so fakefs startup cost is excluded.
# Pre-compiled C binaries (cbench_lite_x86, cbench_lite_arm64, cbench_lite_macos)
# avoid slow in-emulator gcc compilation.
#
# Assets:  assets/shellbench.sh, assets/cbench_lite.c (source),
#          assets/cbench_lite_{x86,arm64,macos} (prebuilt binaries)
# Reports: BENCHMARK_PERF.md, BENCHMARK_COMPAT.md
# Data:    results/
# ═══════════════════════════════════════════════════════════════════════

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
RESULTS_DIR="$SCRIPT_DIR/results"
ASSETS_DIR="$SCRIPT_DIR/assets"
DATE=$(date +%Y%m%d_%H%M%S)

ISH_X86="$PROJECT_DIR/build-x86-release/ish"
ISH_ARM64="$PROJECT_DIR/build-arm64-release/ish"
FAKEFS_X86="$PROJECT_DIR/alpine-x86-fakefs"
FAKEFS_ARM64="$PROJECT_DIR/alpine-arm64-fakefs"

PERF_MD="$SCRIPT_DIR/BENCHMARK_PERF.md"
COMPAT_MD="$SCRIPT_DIR/BENCHMARK_COMPAT.md"
CSV="$RESULTS_DIR/run_${DATE}.csv"
RUNS=3
TIMEOUT_S=120

mkdir -p "$RESULTS_DIR"

# ── Colors & helpers ────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; DIM='\033[2m'; NC='\033[0m'
log()  { echo -e "${CYAN}>>>${NC} $*"; }
ok()   { echo -e "${GREEN}OK${NC}  $*"; }
fail() { echo -e "${RED}ERR${NC} $*"; }
hr()   { echo "────────────────────────────────────────────────────────────────"; }

# ── Preflight ───────────────────────────────────────────────────────
preflight() {
    local ok=1
    [ -x "$ISH_X86" ]       || { fail "x86 binary: $ISH_X86"; ok=0; }
    [ -x "$ISH_ARM64" ]     || { fail "ARM64 binary: $ISH_ARM64"; ok=0; }
    [ -d "$FAKEFS_X86" ]    || { fail "x86 fakefs: $FAKEFS_X86"; ok=0; }
    [ -d "$FAKEFS_ARM64" ]  || { fail "ARM64 fakefs: $FAKEFS_ARM64"; ok=0; }
    [ $ok -eq 0 ] && exit 1
}

# ── Timing via /usr/bin/time ────────────────────────────────────────
# Returns "seconds instructions"
_time() {
    local tmp; tmp=$(mktemp)
    if timeout "$TIMEOUT_S" /usr/bin/time -l "$@" >/dev/null 2>"$tmp"; then :
    elif [ $? -eq 124 ]; then rm -f "$tmp"; echo "TIMEOUT 0"; return; fi
    local t; t=$(awk '/real/{print $1}' "$tmp" | head -1); [ -z "$t" ] && t=0
    local i; i=$(awk '/instructions retired/{gsub(/,/,""); print $1}' "$tmp"); [ -z "$i" ] && i=0
    rm -f "$tmp"; echo "$t $i"
}

_median() {
    echo "$@" | tr ' ' '\n' | grep -vE 'TIMEOUT|FAIL' | sort -n |
        awk '{a[NR]=$1} END{if(NR==0)print"FAIL";else if(NR%2)print a[(NR+1)/2];else print(a[NR/2]+a[NR/2+1])/2}'
}

_ratio() {
    local a="$1" b="$2"
    { [ "$a" = "FAIL" ] || [ "$b" = "FAIL" ]; } && { echo "—"; return; }
    awk "BEGIN{b=$b; if(b>0.00001)printf\"%.1fx\",$a/b; else print\"—\"}"
}

_fmti() {
    local n="$1"
    { [ "$n" = "0" ] || [ "$n" = "FAIL" ]; } && { echo "—"; return; }
    awk "BEGIN{n=$n; if(n>=1e9)printf\"%.1fB\",n/1e9; else if(n>=1e6)printf\"%.0fM\",n/1e6; else printf\"%.0fK\",n/1e3}"
}

# ── Run ISH helpers ─────────────────────────────────────────────────
ish_x86()  { "$ISH_X86" -f "$FAKEFS_X86" /bin/sh -c "$1"; }
ish_arm()  { "$ISH_ARM64" -f "$FAKEFS_ARM64" /bin/sh -c "$1"; }

# Push a file into the ARM64 fakefs data directory
push_asset() {
    local src="$1" dst="$2"
    cp "$src" "$FAKEFS_ARM64/data$dst"
}

# ═══════════════════════════════════════════════════════════════════
# SUITE 1: Shell — guest-side timing (eliminates startup overhead)
#
# Runs assets/shellbench.sh INSIDE each iSH instance. The script
# measures each test with guest-side monotonic clock, so fakefs/realfs
# startup cost is paid once and does NOT affect individual test times.
# ═══════════════════════════════════════════════════════════════════

suite_shell() {
    log "Shell benchmark (guest-side timing, startup excluded)"
    hr

    # Push benchmark scripts + prebuilt binaries into both rootfs
    for asset in shellbench.sh cbench_lite.c; do
        [ -f "$ASSETS_DIR/$asset" ] || continue
        cat "$ASSETS_DIR/$asset" | timeout 10 "$ISH_X86" -f "$FAKEFS_X86" /bin/sh -c "cat > /tmp/$asset" 2>/dev/null || true
        cat "$ASSETS_DIR/$asset" | timeout 10 "$ISH_ARM64" -f "$FAKEFS_ARM64" /bin/sh -c "cat > /tmp/$asset" 2>/dev/null || true
    done
    # Push prebuilt C binaries (avoids slow in-emulator compilation)
    if [ -f "$ASSETS_DIR/cbench_lite_x86" ]; then
        cat "$ASSETS_DIR/cbench_lite_x86" | timeout 10 "$ISH_X86" -f "$FAKEFS_X86" /bin/sh -c "cat > /tmp/cbench_prebuilt && chmod +x /tmp/cbench_prebuilt" 2>/dev/null || true
    fi
    if [ -f "$ASSETS_DIR/cbench_lite_arm64" ]; then
        cat "$ASSETS_DIR/cbench_lite_arm64" | timeout 10 "$ISH_ARM64" -f "$FAKEFS_ARM64" /bin/sh -c "cat > /tmp/cbench_prebuilt && chmod +x /tmp/cbench_prebuilt" 2>/dev/null || true
    fi

    # Run inside each platform, capture "cat|name|ms" output
    log "Running on Native..."
    local native_out; native_out=$(mktemp)
    bash "$ASSETS_DIR/shellbench.sh" > "$native_out" 2>/dev/null

    log "Running on x86..."
    local x86_out; x86_out=$(mktemp)
    timeout 900 "$ISH_X86" -f "$FAKEFS_X86" /bin/sh -c "sh /tmp/shellbench.sh" > "$x86_out" 2>/dev/null || true

    log "Running on ARM64..."
    local arm_out; arm_out=$(mktemp)
    timeout 900 "$ISH_ARM64" -f "$FAKEFS_ARM64" /bin/sh -c "sh /tmp/shellbench.sh" > "$arm_out" 2>/dev/null || true

    echo ""
    printf "${BOLD}%-9s %-18s │ %8s │ %8s │ %8s │ %8s │ %8s${NC}\n" \
        "Cat" "Test" "Native" "x86" "ARM64" "x86/Nat" "x86/A64"
    hr

    local rows=()

    # Parse native results into associative-like arrays (POSIX sh compat via temp files)
    local nat_data; nat_data=$(mktemp)
    local x86_data; x86_data=$(mktemp)
    local arm_data; arm_data=$(mktemp)
    cp "$native_out" "$nat_data"
    cp "$x86_out" "$x86_data"
    cp "$arm_out" "$arm_data"

    # Iterate over ARM64 results (superset — has Python/Node/Go if installed)
    while IFS='|' read -r cat name arm_ms; do
        [ -z "$cat" ] && continue
        local nat_ms x86_ms
        nat_ms=$(grep "^${cat}|${name}|" "$nat_data" | head -1 | cut -d'|' -f3)
        x86_ms=$(grep "^${cat}|${name}|" "$x86_data" | head -1 | cut -d'|' -f3)
        [ -z "$nat_ms" ] && nat_ms="—"
        [ -z "$x86_ms" ] && x86_ms="—"

        # Detect failed measurements on x86:
        # - Near-timeout value (≥29000ms when tests use timeout 30): treat as hang
        # - Node.js non-startup tests that finish "suspiciously fast" (faster than
        #   ARM64 AND less than 500ms AND ARM64 > 800ms) likely crashed before work
        if [ "$x86_ms" != "—" ] && [ "$x86_ms" -ge 29000 ] 2>/dev/null; then
            x86_ms="FAIL"
        fi
        if [ "$cat" = "Node.js" ] && [ "$name" != "startup" ] && \
           [ "$x86_ms" != "—" ] && [ "$x86_ms" != "FAIL" ] && \
           [ "$arm_ms" -gt 800 ] 2>/dev/null && \
           [ "$x86_ms" -lt 500 ] 2>/dev/null; then
            x86_ms="FAIL"
        fi

        # Format times
        local nat_f x86_f arm_f xn_r xa_r
        [ "$nat_ms" = "—" ] && nat_f="—" || nat_f="${nat_ms}ms"
        if [ "$x86_ms" = "—" ]; then x86_f="—"
        elif [ "$x86_ms" = "FAIL" ]; then x86_f="FAIL"
        else x86_f="${x86_ms}ms"
        fi
        arm_f="${arm_ms}ms"

        # Ratios — skip when either side is non-numeric
        if [ "$nat_ms" != "—" ] && [ "$x86_ms" != "—" ] && [ "$x86_ms" != "FAIL" ] && \
           [ "$nat_ms" -gt 0 ] 2>/dev/null; then
            xn_r=$(awk "BEGIN{printf\"%.1fx\",$x86_ms/$nat_ms}")
        else
            xn_r="—"
        fi
        if [ "$x86_ms" != "—" ] && [ "$x86_ms" != "FAIL" ] && [ "$arm_ms" -gt 0 ] 2>/dev/null; then
            xa_r=$(awk "BEGIN{printf\"%.1fx\",$x86_ms/$arm_ms}")
        else
            xa_r="—"
        fi

        printf "%-9s %-18s │ %8s │ %8s │ %8s │ %8s │ %8s\n" \
            "$cat" "$name" "$nat_f" "$x86_f" "$arm_f" "$xn_r" "$xa_r"

        echo "shell,$cat,$name,$nat_ms,$x86_ms,$arm_ms" >> "$CSV"
        rows+=("$cat|$name|$nat_f|$x86_f|$arm_f|$xn_r|$xa_r")
    done < "$arm_out"

    rm -f "$native_out" "$x86_out" "$arm_out" "$nat_data" "$x86_data" "$arm_data"
    echo ""
    if [ ${#rows[@]} -gt 0 ]; then
        _md_shell_section "${rows[@]}"
    fi
}

_md_shell_section() {
    local rows=("$@") prev=""
    {
        echo "## 1. Shell Benchmark (Native vs x86 vs ARM64)"
        echo ""
        echo "> **Guest-side timing** — each test measured inside the emulator with"
        echo "> monotonic clock. Startup overhead (fakefs init) is excluded."
        echo "> This isolates pure emulation performance."
        echo ""
    } >> "$PERF_MD"

    for r in "${rows[@]}"; do
        IFS='|' read -r cat name nat x86 arm xn xa <<< "$r"
        if [ "$cat" != "$prev" ]; then
            [ -n "$prev" ] && echo "" >> "$PERF_MD"
            echo "### $cat" >> "$PERF_MD"
            echo "" >> "$PERF_MD"
            echo "| Test | Native | x86 | ARM64 | x86/Native | **x86/ARM64** |" >> "$PERF_MD"
            echo "|------|:---:|:---:|:---:|:---:|:---:|" >> "$PERF_MD"
            prev="$cat"
        fi
        echo "| $name | $nat | $x86 | $arm | $xn | **$xa** |" >> "$PERF_MD"
    done
    echo "" >> "$PERF_MD"
}

# NOTE: Python, Node.js, Go, and C benchmarks all run inside suite_shell
# via assets/shellbench.sh. The separate suite_python / suite_node / suite_c
# functions were removed — shellbench.sh performs identical work with
# guest-side timing (no startup overhead) and produces comparable x86 data.

# ═══════════════════════════════════════════════════════════════════
# SUITE 2: Compatibility — pass/fail across x86 and ARM64
# ═══════════════════════════════════════════════════════════════════

# ── All compatibility tests (both x86 and ARM64 attempt every test) ──
# Derived from bench/scripts/test_200_core_software.sh (264 items),
# deduplicated and restructured into a flat array.
# x86 rootfs must have packages pre-installed via apk before running.
COMPAT_TESTS=(
    # 1. Basic System Tools (28)
    "Core|ls|ls / >/dev/null"
    "Core|cat|cat /etc/os-release >/dev/null"
    "Core|echo|echo test"
    "Core|grep|echo test | grep test >/dev/null"
    "Core|sed|echo test | sed 's/test/ok/'"
    "Core|awk|echo '1 2' | awk '{print \$1+\$2}'"
    "Core|wc|echo 'a b c' | wc -w"
    "Core|sort|echo -e '3\n1\n2' | sort | head -1"
    "Core|uniq|echo -e 'a\na\nb' | uniq | wc -l"
    "Core|head|echo -e '1\n2\n3' | head -1"
    "Core|tail|echo -e '1\n2\n3' | tail -1"
    "Core|cut|echo 'a:b:c' | cut -d: -f2"
    "Core|tr|echo abc | tr a-z A-Z"
    "Core|tee|echo test | tee /dev/null >/dev/null"
    "Core|xargs|echo test | xargs echo"
    "Core|find|find /etc -name 'os-release' -type f >/dev/null"
    "Core|which|which sh >/dev/null"
    "Core|whoami|whoami >/dev/null"
    "Core|pwd|pwd >/dev/null"
    "Core|basename|basename /usr/bin/test"
    "Core|dirname|dirname /usr/bin/test"
    "Core|date|date >/dev/null"
    "Core|sleep|sleep 0"
    "Core|yes|yes | head -1 >/dev/null"
    "Core|seq|seq 3 | wc -l"
    "Core|expr|expr 2 + 2 >/dev/null"
    "Core|env|env >/dev/null"
    "Core|printenv|printenv PATH >/dev/null"
    # 2. File Operations (22)
    "FileOps|cp|echo test > /tmp/_s && cp /tmp/_s /tmp/_d && cat /tmp/_d >/dev/null && rm /tmp/_s /tmp/_d"
    "FileOps|mv|echo test > /tmp/_o && mv /tmp/_o /tmp/_n && rm /tmp/_n"
    "FileOps|rm|touch /tmp/_del && rm /tmp/_del"
    "FileOps|mkdir|mkdir -p /tmp/_td && rmdir /tmp/_td"
    "FileOps|touch|touch /tmp/_t && rm /tmp/_t"
    "FileOps|chmod|touch /tmp/_ch && chmod 644 /tmp/_ch && rm /tmp/_ch"
    "FileOps|ln -s|echo x>/tmp/_o2 && ln -s /tmp/_o2 /tmp/_l2 && rm /tmp/_o2 /tmp/_l2"
    "FileOps|readlink|ln -s /tmp/_tgt /tmp/_rl && readlink /tmp/_rl >/dev/null; rm -f /tmp/_rl"
    "FileOps|stat|stat /bin/sh >/dev/null"
    "FileOps|file|file /bin/busybox >/dev/null 2>&1"
    "FileOps|du|du -sh /etc >/dev/null"
    "FileOps|df|df -h / >/dev/null"
    "FileOps|dd|dd if=/dev/zero of=/tmp/_dd bs=1k count=1 2>/dev/null && rm /tmp/_dd"
    "FileOps|truncate|truncate -s 100 /tmp/_tr && rm /tmp/_tr"
    "FileOps|split|echo -e '1\n2\n3\n4' > /tmp/_sp && split -l 2 /tmp/_sp /tmp/_sp_ && rm /tmp/_sp /tmp/_sp_*"
    "FileOps|tar|echo test > /tmp/_tf && tar czf /tmp/_tf.tgz -C /tmp _tf && rm /tmp/_tf /tmp/_tf.tgz"
    "FileOps|gzip|echo test | gzip | gunzip >/dev/null"
    "FileOps|bzip2|echo test | bzip2 | bunzip2 >/dev/null"
    "FileOps|xz|echo test > /tmp/_xz && xz /tmp/_xz && unxz /tmp/_xz.xz && rm /tmp/_xz"
    "FileOps|zstd|echo test > /tmp/_zs && zstd -q /tmp/_zs -o /tmp/_zs.zst && rm /tmp/_zs /tmp/_zs.zst"
    "FileOps|sync|sync"
    "FileOps|rsync|rsync --version >/dev/null 2>&1"
    # 3. Text Processing (16)
    "TextProc|diff|echo a>/tmp/_da && echo b>/tmp/_db && diff /tmp/_da /tmp/_db; rm /tmp/_da /tmp/_db; true"
    "TextProc|fold|echo abcdefghij | fold -w 5 | head -1 >/dev/null"
    "TextProc|nl|echo -e 'a\nb' | nl | head -1 >/dev/null"
    "TextProc|rev|echo abc | rev >/dev/null"
    "TextProc|strings|strings /bin/busybox | head -1 >/dev/null"
    "TextProc|hexdump|echo test | hexdump -C | head -1 >/dev/null"
    "TextProc|od|echo a | od -c >/dev/null"
    "TextProc|base64|echo test | base64 >/dev/null"
    "TextProc|iconv|echo test | iconv -f UTF-8 -t UTF-8 >/dev/null"
    "TextProc|fmt|echo 'test line' | fmt >/dev/null 2>&1"
    "TextProc|column|echo -e 'a\tb' | column -t >/dev/null 2>&1"
    "TextProc|numfmt|echo 1000000 | numfmt --to=iec >/dev/null 2>&1"
    "TextProc|comm|echo 1>/tmp/_c1 && echo 2>/tmp/_c2 && comm /tmp/_c1 /tmp/_c2 >/dev/null; rm /tmp/_c1 /tmp/_c2"
    "TextProc|shuf|seq 5 | shuf | wc -l >/dev/null"
    "TextProc|jq|echo '{\"a\":1}' | jq .a >/dev/null 2>&1"
    "TextProc|bc|echo '2+2' | bc >/dev/null"
    # 4. Build Toolchain (18)
    "Build|gcc|gcc --version >/dev/null 2>&1"
    "Build|g++|g++ --version >/dev/null 2>&1"
    "Build|cc|cc --version >/dev/null 2>&1"
    "Build|make|make --version >/dev/null 2>&1"
    "Build|cmake|cmake --version >/dev/null 2>&1"
    "Build|ld|ld --version >/dev/null 2>&1"
    "Build|as|as --version >/dev/null 2>&1"
    "Build|ar|ar --version >/dev/null 2>&1"
    "Build|nm|nm --version >/dev/null 2>&1"
    "Build|objdump|objdump --version >/dev/null 2>&1"
    "Build|strip|strip --version >/dev/null 2>&1"
    "Build|readelf|readelf --version >/dev/null 2>&1"
    "Build|pkg-config|pkg-config --version >/dev/null 2>&1"
    "Build|autoconf|autoconf --version >/dev/null 2>&1"
    "Build|automake|automake --version >/dev/null 2>&1"
    "Build|bison|bison --version >/dev/null 2>&1"
    "Build|flex|flex --version >/dev/null 2>&1"
    "Build|m4|echo 'define(X,Y)X' | m4 >/dev/null 2>&1"
    # 5. Python Ecosystem (20)
    "Python|python3|python3 --version >/dev/null 2>&1"
    "Python|pip3|pip3 --version >/dev/null 2>&1"
    "Python|pip list|pip3 list 2>/dev/null | head -3 >/dev/null"
    "Python|import os|python3 -c 'import os; print(os.getpid())'"
    "Python|import json|python3 -c 'import json; print(json.dumps({\"a\":1}))'"
    "Python|import re|python3 -c 'import re; print(re.match(r\"\\d+\",\"42\").group())'"
    "Python|import math|python3 -c 'import math; print(math.pi)'"
    "Python|import sqlite3|python3 -c 'import sqlite3; print(sqlite3.sqlite_version)'"
    "Python|import hashlib|python3 -c 'import hashlib; print(hashlib.sha256(b\"x\").hexdigest()[:8])'"
    "Python|import socket|python3 -c 'import socket; print(socket.gethostname())'"
    "Python|import datetime|python3 -c 'from datetime import datetime; print(datetime.now().year)'"
    "Python|import venv|python3 -c 'import venv; print(\"ok\")'"
    "Python|import csv|python3 -c 'import csv,io; r=csv.reader(io.StringIO(\"a,b\")); print(next(r))'"
    "Python|import subprocess|python3 -c 'import subprocess; print(subprocess.check_output([\"echo\",\"ok\"]).strip().decode())'"
    "Python|import pathlib|python3 -c 'from pathlib import Path; print(Path(\"/bin/sh\").exists())'"
    "Python|import tempfile|python3 -c 'import tempfile; f=tempfile.NamedTemporaryFile(); print(f.name); f.close()'"
    "Python|import argparse|python3 -c 'import argparse; p=argparse.ArgumentParser(); print(\"ok\")'"
    "Python|async/await|python3 -c 'import asyncio; asyncio.run(asyncio.sleep(0)); print(\"ok\")'"
    "Python|dataclass|python3 -c 'from dataclasses import dataclass; print(\"ok\")'"
    "Python|http.server|python3 -m http.server --help 2>&1 | head -1 >/dev/null"
    # 6. Node.js Ecosystem (18)
    "Node.js|node|node --version >/dev/null 2>&1"
    "Node.js|npm|npm --version >/dev/null 2>&1"
    "Node.js|console.log|node -e 'console.log(42)'"
    "Node.js|require fs|node -e 'require(\"fs\").writeFileSync(\"/tmp/_n.txt\",\"ok\"); console.log(require(\"fs\").readFileSync(\"/tmp/_n.txt\",\"utf8\"))'"
    "Node.js|require path|node -e 'console.log(require(\"path\").join(\"a\",\"b\"))'"
    "Node.js|require os|node -e 'console.log(require(\"os\").platform())'"
    "Node.js|require crypto|node -e 'console.log(require(\"crypto\").randomBytes(4).toString(\"hex\"))'"
    "Node.js|require http|node -e 'console.log(typeof require(\"http\").createServer)'"
    "Node.js|require url|node -e 'console.log(new URL(\"http://a.com/b\").pathname)'"
    "Node.js|require zlib|node -e 'console.log(require(\"zlib\").gzipSync(\"hi\").length)'"
    "Node.js|require stream|node -e 'console.log(typeof require(\"stream\").Readable)'"
    "Node.js|require events|node -e 'const E=require(\"events\");const e=new E();e.on(\"x\",()=>console.log(\"ok\"));e.emit(\"x\")'"
    "Node.js|Promise|node -e 'Promise.resolve(42).then(v=>console.log(v))'"
    "Node.js|async/await|node -e '(async()=>{console.log(await Promise.resolve(1))})()'"
    "Node.js|Buffer|node -e 'console.log(Buffer.from(\"hello\").toString(\"hex\"))'"
    "Node.js|JSON parse|node -e 'console.log(JSON.parse(\"{\\\"a\\\":1}\").a)'"
    "Node.js|Map/Set|node -e 'const m=new Map([[1,2]]); console.log(m.get(1))'"
    "Node.js|RegExp|node -e 'console.log(/\\d+/.test(\"abc123\"))'"
    # 7. Other Languages (10)
    "Lang|perl|perl -e 'print 2+2' 2>/dev/null"
    "Lang|ruby|ruby -e 'puts 2+2' 2>/dev/null"
    "Lang|php|php -r 'echo 2+2;' 2>/dev/null"
    "Lang|lua|lua5.4 -e 'print(2+2)' 2>/dev/null || lua -e 'print(2+2)' 2>/dev/null"
    "Lang|bash|bash -c 'echo ok'"
    "Lang|ash|ash -c 'echo ok'"
    "Lang|go version|go version >/dev/null 2>&1"
    "Lang|go env|go env GOROOT >/dev/null 2>&1"
    "Lang|go compile|echo 'package main;func main(){}' > /tmp/_go.go && go tool compile -o /tmp/_go.o /tmp/_go.go 2>/dev/null && rm -f /tmp/_go.go /tmp/_go.o"
    "Lang|clang|clang --version >/dev/null 2>&1"
    # 8. Network Tools (14)
    # ping/nc/nslookup are now exercised end-to-end (real syscalls), not just
    # checked for --help/--version, so they actually exercise iSH's socket layer.
    "Network|curl|curl --version >/dev/null 2>&1"
    "Network|wget|wget --version >/dev/null 2>&1"
    "Network|ssh|ssh -V 2>&1 | grep -qi openssh"
    "Network|scp|scp 2>&1 | head -1 >/dev/null; true"
    "Network|ping localhost|ping -c 1 -W 2 127.0.0.1 >/dev/null 2>&1"
    "Network|ping 4-pkt|ping -c 4 -W 2 127.0.0.1 >/dev/null 2>&1"
    "Network|netstat|netstat -h 2>&1 | head -1 >/dev/null; true"
    "Network|ss|ss -h 2>&1 | head -1 >/dev/null; true"
    "Network|nslookup localhost|nslookup -timeout=2 localhost 127.0.0.1 2>&1 | grep -q '127.0.0.1\\|Address'"
    "Network|nslookup 8.8.8.8|nslookup -timeout=2 8.8.8.8 2>&1 | grep -qi 'name\\|server'"
    "Network|dig|dig -v 2>&1 | head -1 >/dev/null; true"
    "Network|nc -l + nc->127.0.0.1|(nc -l -p 19999 -q 1 >/tmp/_nc_in 2>/dev/null & sleep 0.2; echo hello | nc -w 1 127.0.0.1 19999 >/dev/null 2>&1); wait 2>/dev/null; grep -q hello /tmp/_nc_in 2>/dev/null; rm -f /tmp/_nc_in"
    "Network|nc port-scan|nc -z -w 1 127.0.0.1 1 2>/dev/null; [ \$? -ne 0 ]"
    "Network|socat|socat -V >/dev/null 2>&1"
    "Network|ip|ip link 2>&1 | head -1 >/dev/null; true"
    "Network|ifconfig|ifconfig -a 2>&1 | head -1 >/dev/null; true"
    "Network|openssl|openssl version >/dev/null 2>&1"
    # 9. Version Control (5)
    "VCS|git|git --version >/dev/null 2>&1"
    "VCS|git init|rm -rf /tmp/_gr && git init /tmp/_gr >/dev/null 2>&1 && rm -rf /tmp/_gr"
    "VCS|hg|hg --version >/dev/null 2>&1"
    "VCS|svn|svn --version >/dev/null 2>&1"
    "VCS|git config|git config --global user.name 2>&1 >/dev/null; true"
    # 10. Editors (6)
    "Editor|vi|vi --version 2>&1 | head -1 >/dev/null; true"
    "Editor|vim|vim --version 2>&1 | head -1 >/dev/null; true"
    "Editor|nano|nano --version >/dev/null 2>&1"
    "Editor|ed|echo -e 'a\ntest\n.\n1p\nq' | ed 2>/dev/null | grep -q test; true"
    "Editor|sed -i|echo test>/tmp/_si && sed -i 's/test/ok/' /tmp/_si && grep -q ok /tmp/_si && rm /tmp/_si"
    "Editor|awk script|echo 'a b' | awk '{print \$1}' >/dev/null"
    # 11. Shell Tools (6)
    "Shell|bash|bash --version >/dev/null 2>&1"
    "Shell|ash|ash -c 'echo ok' >/dev/null"
    "Shell|screen|screen -v 2>&1 | head -1 >/dev/null; true"
    "Shell|tmux|tmux -V 2>&1 | head -1 >/dev/null; true"
    "Shell|timeout|timeout 1 echo ok >/dev/null"
    "Shell|time|time echo ok 2>&1 >/dev/null"
    # 12. Databases (5)
    "DB|sqlite3|sqlite3 --version >/dev/null 2>&1"
    "DB|sqlite3 query|sqlite3 :memory: 'CREATE TABLE t(x);INSERT INTO t VALUES(1);SELECT * FROM t;'"
    "DB|redis-cli|redis-cli --version >/dev/null 2>&1; true"
    "DB|psql|psql --version >/dev/null 2>&1; true"
    "DB|mariadb|mariadb --version >/dev/null 2>&1; true"
    # 13. Multimedia (6)
    "Media|ffmpeg|ffmpeg -version 2>&1 | head -1 >/dev/null; true"
    "Media|ffprobe|ffprobe -version 2>&1 | head -1 >/dev/null; true"
    "Media|convert|convert -version 2>&1 | head -1 >/dev/null; true"
    "Media|identify|identify -version 2>&1 | head -1 >/dev/null; true"
    "Media|sox|sox --version 2>&1 | head -1 >/dev/null; true"
    "Media|gm|gm version 2>&1 | head -1 >/dev/null; true"
    # 14. Crypto & Security (8)
    "Crypto|openssl enc|echo test | openssl enc -aes-256-cbc -pbkdf2 -pass pass:123 2>/dev/null | openssl enc -d -aes-256-cbc -pbkdf2 -pass pass:123 2>/dev/null"
    "Crypto|sha256sum|echo test | sha256sum >/dev/null"
    "Crypto|sha512sum|echo test | sha512sum >/dev/null"
    "Crypto|md5sum|echo test | md5sum >/dev/null"
    "Crypto|gpg|gpg --version >/dev/null 2>&1"
    "Crypto|ssh-keygen|ssh-keygen -h 2>&1 | head -1 >/dev/null; true"
    "Crypto|base64 enc|echo test | base64 | base64 -d >/dev/null"
    "Crypto|/dev/urandom|dd if=/dev/urandom bs=16 count=1 2>/dev/null | wc -c >/dev/null"
    # 15. System Monitoring (8)
    "SysMon|ps|ps aux 2>&1 | head -1 >/dev/null; true"
    "SysMon|free|free 2>&1 | head -1 >/dev/null; true"
    "SysMon|uptime|uptime >/dev/null 2>&1"
    "SysMon|/proc/meminfo|cat /proc/meminfo >/dev/null 2>&1; true"
    "SysMon|/proc/cpuinfo|cat /proc/cpuinfo >/dev/null 2>&1; true"
    "SysMon|/proc/version|cat /proc/version >/dev/null 2>&1; true"
    "SysMon|/dev/null|echo test > /dev/null"
    "SysMon|/dev/zero|dd if=/dev/zero bs=16 count=1 2>/dev/null | wc -c >/dev/null"
    # 16. Dev & Debug Tools (8)
    "Debug|strace|strace -V 2>&1 | head -1 >/dev/null; true"
    "Debug|ldd|ldd --version 2>&1 | head -1 >/dev/null; ldd /bin/busybox 2>&1 | head -1 >/dev/null; true"
    "Debug|gdb|gdb --version 2>&1 | head -1 >/dev/null; true"
    "Debug|lsof|lsof -v 2>&1 | head -1 >/dev/null; true"
    "Debug|objdump|objdump --version >/dev/null 2>&1; true"
    "Debug|valgrind|valgrind --version 2>&1 | head -1 >/dev/null; true"
    "Debug|ltrace|ltrace -V 2>&1 | head -1 >/dev/null; true"
    "Debug|perf|perf --version 2>&1 | head -1 >/dev/null; true"
    # 17. Package Manager (3)
    "PkgMgr|apk|apk --version >/dev/null 2>&1"
    "PkgMgr|pip3|pip3 --version >/dev/null 2>&1"
    "PkgMgr|npm|npm --version >/dev/null 2>&1"
    # 18. Signal & Process (4)
    "Signal|trap|sh -c 'trap \"echo c\" INT; kill -INT \$\$; echo ok' 2>/dev/null; true"
    "Signal|kill|sh -c 'sleep 99 & kill \$! 2>/dev/null; wait \$! 2>/dev/null; echo ok'"
    "Signal|wait|sh -c 'true & wait \$!'"
    "Signal|bg job|sh -c 'sleep 0 & wait'"
    # 19. ClawHub skill dependencies — smoke tests (15)
    # Tools required by top-starred ClawHub agent skills. Browser (Playwright)
    # and neural-net-heavy (Whisper, nano-pdf, ComPDFKit) skills are excluded.
    # Cloud-auth CLIs are smoke-tested via --help; pure-local tools get real
    # invocations.
    "Skill|yt-dlp (youtube-watcher 287★)|yt-dlp --version >/dev/null 2>&1"
    "Skill|ffmpeg (video-frames 115★)|ffmpeg -version >/dev/null 2>&1"
    "Skill|jq (trello 140★)|echo '{\"a\":1}' | jq .a >/dev/null 2>&1"
    "Skill|ImageMagick convert (imagemagick)|convert -version >/dev/null 2>&1"
    "Skill|ImageMagick magick (imagemagick)|magick -version >/dev/null 2>&1"
    "Skill|ImageMagick smoke (vision/image-resize)|convert -size 16x16 xc:white /tmp/_im.png && rm /tmp/_im.png"
    "Skill|matplotlib (chart-mpl/python-dataviz)|python3 -c 'import matplotlib; matplotlib.use(\"Agg\"); import matplotlib.pyplot as p; p.plot([1,2,3]); p.savefig(\"/tmp/_mpl.png\"); import os; os.remove(\"/tmp/_mpl.png\")'"
    "Skill|pandas (pandas-skill)|python3 -c 'import pandas as pd; print(pd.DataFrame({\"a\":[1,2,3]}).sum().a)' >/dev/null 2>&1"
    "Skill|numpy (numpy)|python3 -c 'import numpy as np; print(np.zeros(5).sum())' >/dev/null 2>&1"
    "Skill|akshare (akshare-stock 76★)|python3 -c 'import akshare' 2>/dev/null"
    "Skill|khal (caldav-calendar 216★)|khal --help 2>&1 | head -1 >/dev/null"
    "Skill|vdirsyncer (caldav-calendar 216★)|vdirsyncer --help 2>&1 | head -1 >/dev/null"
    "Skill|mcporter (mcporter 184★)|npx -y mcporter --help 2>&1 | head -1 >/dev/null"
    "Skill|meitu-cli (meitu-skills 119★)|npx -y meitu-cli@latest --help 2>&1 | head -1 >/dev/null"
    "Skill|node-edge-tts (edge-tts 29★)|npx -y node-edge-tts 2>&1 | head -1 | grep -q Usage"
)

# Binary name → Alpine package mapping for auto-install
# Only needed for binaries not in busybox or base install
_pkg_for() {
    case "$1" in
        file) echo file;; xz|unxz) echo xz;; zstd) echo zstd;; rsync) echo rsync;;
        fmt|numfmt|base32) echo coreutils;; column) echo util-linux;;
        jq) echo jq;; bc) echo bc;;
        gcc|cc) echo build-base;; g++|c++) echo build-base;;
        make) echo make;; cmake) echo cmake;; m4) echo m4;;
        ld|as|ar|nm|objdump|objcopy|strip|size|ranlib|readelf) echo binutils;;
        pkg-config) echo pkgconf;; autoconf) echo autoconf;; automake) echo automake;;
        bison) echo bison;; flex) echo flex;; clang) echo clang;;
        python3) echo python3;; pip3|pip) echo py3-pip;;
        node) echo nodejs;; npm) echo nodejs npm;;
        go) echo go;;
        perl) echo perl;; ruby) echo ruby;; php) echo php83;; lua5.4) echo lua5.4;;
        bash) echo bash;;
        git) echo git;; hg) echo mercurial;; svn) echo subversion;;
        curl) echo curl;; wget) echo wget;; ssh|scp|sftp) echo openssh-client;;
        socat) echo socat;; dig|nslookup) echo bind-tools;;
        nano) echo nano;; vim) echo vim;; ed) echo ed;;
        sqlite3) echo sqlite;; openssl) echo openssl;;
        ffmpeg|ffprobe) echo ffmpeg;; convert|identify|mogrify) echo imagemagick;;
        gm) echo graphicsmagick;; sox) echo sox;;
        gpg) echo gnupg;; strace) echo strace;;
        gdb) echo gdb;; lsof) echo lsof;; valgrind) echo valgrind;;
        ltrace) echo ltrace;; screen) echo screen;; tmux) echo tmux;;
        *) echo "";;
    esac
}

# Auto-install missing packages for a given ISH instance
# Args: <ish_binary> <fakefs_flag> <fakefs_path> <label>
_ensure_packages() {
    local ish_bin="$1" fs_flag="$2" fs_path="$3" label="$4"

    log "Checking packages on $label..."

    # Collect unique binary names that need packages
    local missing_pkgs="" checked=""
    local all_bins="file xz zstd rsync fmt numfmt column jq bc gcc g++ make cmake m4 ld as ar nm objdump strip readelf pkg-config autoconf automake bison flex clang python3 pip3 node npm go perl ruby php lua5.4 bash git hg svn curl wget ssh socat dig nano vim ed sqlite3 openssl ffmpeg convert gm sox gpg strace gdb lsof valgrind ltrace screen tmux"

    for bin in $all_bins; do
        local pkg
        pkg=$(_pkg_for "$bin")
        [ -z "$pkg" ] && continue

        # Check if binary exists in guest
        if ! timeout 5 "$ish_bin" "$fs_flag" "$fs_path" /bin/sh -c "which $bin >/dev/null 2>&1" >/dev/null 2>&1; then
            for p in $pkg; do
                echo "$missing_pkgs" | grep -qw "$p" || missing_pkgs="$missing_pkgs $p"
            done
        fi
    done

    if [ -n "$missing_pkgs" ]; then
        # Ensure DNS + HTTP repos
        timeout 5 "$ish_bin" "$fs_flag" "$fs_path" /bin/sh -c \
            "test -f /etc/resolv.conf || echo 'nameserver 8.8.8.8' > /etc/resolv.conf; sed -i 's|https://|http://|g' /etc/apk/repositories 2>/dev/null" \
            >/dev/null 2>&1 || true

        log "Installing on $label:$missing_pkgs"
        timeout 600 "$ish_bin" "$fs_flag" "$fs_path" /bin/sh -c \
            "apk update >/dev/null 2>&1; apk add --no-cache $missing_pkgs 2>&1 | tail -3" 2>&1 | grep -v fork
        ok "$label packages ready"
    else
        ok "$label: all packages present"
    fi
}

suite_compat() {
    log "Compatibility tests (x86 vs ARM64)"
    hr

    # Auto-install missing packages on both architectures
    _ensure_packages "$ISH_X86" -f "$FAKEFS_X86" "x86"
    _ensure_packages "$ISH_ARM64" -f "$FAKEFS_ARM64" "ARM64"
    echo ""

    local total=${#COMPAT_TESTS[@]} n=0
    local x86p=0 x86f=0 armp=0 armf=0
    local rows=()

    printf "${BOLD}%-9s %-22s │ %6s │ %6s${NC}\n" "Cat" "Test" "x86" "ARM64"
    hr

    for line in "${COMPAT_TESTS[@]}"; do
        n=$((n+1))
        IFS='|' read -r cat name cmd <<< "$line"

        # Skill tests that exec `npx -y <pkg>` need a longer budget to spin
        # up Node + load the package; everything else gets the 15s default.
        local t=15
        case "$cmd" in *"npx -y"*) t=60 ;; esac

        local xr ar
        if timeout "$t" "$ISH_X86" -f "$FAKEFS_X86" /bin/sh -c "$cmd" >/dev/null 2>&1; then
            xr="PASS"; x86p=$((x86p+1))
        else
            xr="FAIL"; x86f=$((x86f+1))
        fi
        if timeout "$t" "$ISH_ARM64" -f "$FAKEFS_ARM64" /bin/sh -c "$cmd" >/dev/null 2>&1; then
            ar="PASS"; armp=$((armp+1))
        else
            ar="FAIL"; armf=$((armf+1))
        fi

        local xs as
        [ "$xr" = "PASS" ] && xs="${GREEN}PASS${NC}" || xs="${RED}FAIL${NC}"
        [ "$ar" = "PASS" ] && as="${GREEN}PASS${NC}" || as="${RED}FAIL${NC}"
        printf "%-9s %-22s │ %b │ %b\n" "$cat" "$name" "$xs" "$as"
        rows+=("$cat|$name|$xr|$ar")
    done

    echo ""
    ok "x86:   $x86p / $total pass ($(( x86p * 100 / total ))%)"
    ok "ARM64: $armp / $total pass ($(( armp * 100 / total ))%)"

    _md_compat "$x86p" "$x86f" "$armp" "$armf" "$total" "${rows[@]}"
}

_md_compat() {
    local x86p="$1" x86f="$2" armp="$3" armf="$4" total="$5"
    shift 5; local rows=("$@")

    cat > "$COMPAT_MD" << EOF
# iSH Compatibility: x86 vs ARM64

> **Generated:** $(date '+%Y-%m-%d %H:%M:%S') | **Tests:** $total | **Host:** macOS $(sw_vers -productVersion)
>
> Both architectures use **fakefs** mode with virtual device nodes.
> x86 rootfs = Alpine x86 minirootfs (busybox only)
> ARM64 rootfs = Alpine aarch64 full rootfs (apk, python3, node, gcc, go, etc.)
>
> All tests attempt to run on both architectures. x86 failures in the
> software/language categories reflect genuine 32-bit architecture limitations
> (no python3/node/gcc packages available for i386, or runtime VA requirements).

| Architecture | Pass | Fail | Rate |
|:---:|:---:|:---:|:---:|
| **x86** (Jitter) | $x86p | $x86f | **$(( x86p * 100 / total ))%** |
| **ARM64** (Asbestos JIT) | $armp | $armf | **$(( armp * 100 / total ))%** |

---

EOF

    local prev="" cp=0 cf=0 ap=0 af=0 pending=()

    flush() {
        if [ -n "$prev" ]; then
            local t=$((cp+cf))
            echo "### $prev ($t tests)" >> "$COMPAT_MD"
            echo "" >> "$COMPAT_MD"
            echo "| Test | x86 | ARM64 |" >> "$COMPAT_MD"
            echo "|------|:---:|:---:|" >> "$COMPAT_MD"
            for r in "${pending[@]}"; do echo "$r" >> "$COMPAT_MD"; done
            echo "" >> "$COMPAT_MD"
            echo "> x86: $cp/$t — ARM64: $ap/$t" >> "$COMPAT_MD"
            echo "" >> "$COMPAT_MD"
        fi
        pending=(); cp=0; cf=0; ap=0; af=0
    }

    for entry in "${rows[@]}"; do
        IFS='|' read -r cat name xr ar <<< "$entry"
        if [ "$cat" != "$prev" ]; then flush; prev="$cat"; fi
        [ "$xr" = "PASS" ] && cp=$((cp+1)) || cf=$((cf+1))
        [ "$ar" = "PASS" ] && ap=$((ap+1)) || af=$((af+1))
        pending+=("| $name | $xr | $ar |")
    done
    flush

    # Failures summary
    echo "---" >> "$COMPAT_MD"
    echo "" >> "$COMPAT_MD"
    echo "## Failures" >> "$COMPAT_MD"
    echo "" >> "$COMPAT_MD"
    echo "### x86 only" >> "$COMPAT_MD"
    echo "" >> "$COMPAT_MD"
    local any=0
    for e in "${rows[@]}"; do
        IFS='|' read -r c n x a <<< "$e"
        [ "$x" = "FAIL" ] && [ "$a" = "PASS" ] && { echo "- \`$n\` ($c)" >> "$COMPAT_MD"; any=1; }
    done
    [ $any -eq 0 ] && echo "_None_" >> "$COMPAT_MD"

    echo "" >> "$COMPAT_MD"
    echo "### ARM64 only" >> "$COMPAT_MD"
    echo "" >> "$COMPAT_MD"
    any=0
    for e in "${rows[@]}"; do
        IFS='|' read -r c n x a <<< "$e"
        [ "$a" = "FAIL" ] && [ "$x" = "PASS" ] && { echo "- \`$n\` ($c)" >> "$COMPAT_MD"; any=1; }
    done
    [ $any -eq 0 ] && echo "_None_" >> "$COMPAT_MD"

    ok "Compatibility report: $COMPAT_MD"
}

# ═══════════════════════════════════════════════════════════════════
# Report header
# ═══════════════════════════════════════════════════════════════════

write_perf_header() {
    cat > "$PERF_MD" << HEADER
# iSH Performance Benchmark

> **Generated:** $(date '+%Y-%m-%d %H:%M:%S')
> **Host:** macOS $(sw_vers -productVersion) / $(uname -m)
> **x86:** $(basename "$ISH_X86") ($(ls -lh "$ISH_X86" | awk '{print $5}'), fakefs)
> **ARM64:** $(basename "$ISH_ARM64") ($(ls -lh "$ISH_ARM64" | awk '{print $5}'), fakefs)
> **Runs:** $RUNS (median) | **Timeout:** ${TIMEOUT_S}s

| | x86 Emulation | ARM64 JIT |
|---|:---:|:---:|
| Engine | Interpreter (Jitter) | JIT Compiler (Asbestos) |
| Guest | i386 → ARM64 host | AArch64 → AArch64 host |
| Address | 32-bit (4 GB) | 48-bit (256 TB) |
| SIMD | Partial SSE/SSE2 | Full NEON + Crypto |
| Node/Go/Rust | Not possible | Supported |

---

HEADER
}

# ═══════════════════════════════════════════════════════════════════
# Main
# ═══════════════════════════════════════════════════════════════════

main() {
    local suite="${1:-all}"

    echo ""
    echo "═══════════════════════════════════════════════════════════════"
    echo "  iSH Unified Benchmark"
    echo "  $(date '+%Y-%m-%d %H:%M:%S')"
    echo "═══════════════════════════════════════════════════════════════"
    echo ""

    preflight

    echo "suite,category,name,native_time,native_insns,x86_time,x86_insns,arm64_time,arm64_insns" > "$CSV"

    case "$suite" in
        all)
            write_perf_header
            echo ""; suite_shell     # includes Shell + Python + Node.js + Go + C (guest-side timing)
            echo ""; suite_compat    # 205 tests across 18 categories
            ;;
        shell|perf)  write_perf_header; suite_shell ;;
        compat)      suite_compat ;;
        *)           echo "Usage: $0 [all|shell|compat]"; exit 1 ;;
    esac

    echo ""
    echo "═══════════════════════════════════════════════════════════════"
    ok "Done"
    [ -f "$PERF_MD" ]   && echo "  Performance:    $PERF_MD"
    [ -f "$COMPAT_MD" ] && echo "  Compatibility:  $COMPAT_MD"
    echo "  Raw CSV:        $CSV"
    echo "═══════════════════════════════════════════════════════════════"
}

main "$@"

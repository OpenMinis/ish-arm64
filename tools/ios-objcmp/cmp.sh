#!/bin/bash
# cmp.sh <buildA> <buildB>: compare code, data, symbols and relocations of every object
a=$1; b=$2; same=0; diff=0; only=""
sig() { { nm -m "$1" | sort; otool -t "$1" | tail -n +2; otool -r "$1"; for s in "__TEXT __const" "__TEXT __cstring" "__DATA __data" "__DATA __const" "__DATA __bss" "__DATA __common"; do otool -s $s "$1" 2>/dev/null | tail -n +2; done; } | shasum | cut -c1-12; }
cd "$a"; for o in $(find . -name "*.o" | sort); do
  if [ ! -f "$b/$o" ]; then only="$only A:$o"; continue; fi
  if [ "$(sig $o)" = "$(cd $b && sig $o)" ]; then same=$((same+1)); else diff=$((diff+1)); echo "DIFF $o"; fi
done
cd "$b"; for o in $(find . -name "*.o" | sort); do [ -f "$a/$o" ] || only="$only B:$o"; done
echo "same $same, differ $diff, only in one:$only"

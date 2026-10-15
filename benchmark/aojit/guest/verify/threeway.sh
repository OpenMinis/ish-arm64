# Runs check.sh with AOT on and then off in this ish (echo on|off > /proc/ish/jit applies to the
# processes started afterwards) and keeps /proc/ish/jit after each; verify.sh adds the ISH_JIT=0
# run and compares. Results: /tmp/aojit/verify/res/{out_on,out_off,jit_on,jit_end}.txt
V=/tmp/aojit/verify R=$V/res
rm -rf $R; mkdir -p $R
[ -s $V/r10m ] || head -c 10485760 /dev/urandom > $V/r10m
echo on > /proc/ish/jit; sh $V/check.sh > $R/out_on.txt 2>&1; cat /proc/ish/jit > $R/jit_on.txt
echo off > /proc/ish/jit; sh $V/check.sh > $R/out_off.txt 2>&1
echo on > /proc/ish/jit; cat /proc/ish/jit > $R/jit_end.txt

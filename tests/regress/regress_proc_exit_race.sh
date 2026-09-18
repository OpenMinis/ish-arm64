#!/bin/sh
# [T-ish-exit-mm-general-lock] Processes exiting at a high rate while other
# processes read their /proc/<pid>/{cmdline,stat,statm,maps} and run `ps`.
# Before the fix this killed the whole iSH instance within a second: a procfs
# reader took mem->lock on an mm that do_exit was destroying without holding
# general_lock (mem_destroy `brk #1` / SIGSEGV, device 2026-09-19 and native).
# Run inside the guest: ish -f <rootfs> /bin/sh < this_file ; pass = PROC_RACE_OK printed.
set -u
j=0; while [ $j -lt 16 ]; do ( i=0; while [ $i -lt 1000000 ]; do /bin/true; i=$((i+1)); done ) & j=$((j+1)); done
k=0; while [ $k -lt 6 ]; do ( while :; do for p in /proc/[0-9]*; do cat $p/cmdline $p/stat $p/statm $p/maps >/dev/null 2>&1; done; ps -o pid,args >/dev/null 2>&1; done ) & k=$((k+1)); done
sleep 25
kill $(jobs -p) 2>/dev/null; wait 2>/dev/null
echo PROC_RACE_OK

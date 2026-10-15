# libpython recording workload: every task once, in one ish run.
for t in /tmp/aojit/py/tasks/*.sh; do sh $t > /dev/null; done

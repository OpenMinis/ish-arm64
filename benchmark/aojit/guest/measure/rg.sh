# Candidate workload (profile.sh): the searches agents send to rg (always an explicit path: without one rg searches stdin when it is not a terminal)
cd /usr/lib/python3.12
for i in 1 2 3; do rg -n 'def __init__' . > /dev/null; done
rg -l 'import os' . > /dev/null; rg --files . | wc -l > /dev/null; rg -t py -c 'class \w+\(' . > /dev/null
rg -i -n 'todo|fixme' . > /dev/null; rg -P -n '(?<=self\.)_\w+ = ' . > /dev/null; rg -w -n 'yield' json email > /dev/null

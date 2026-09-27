# find recording workload (busybox find): name and type walks, size/newer/depth filters, -exec, xargs,
# over trees unlike measure/find.sh.
P=/usr/lib/python3.12
for i in 1 2; do find $P -name '*.py' | wc -l > /dev/null; done
find $P -iname '*TEST*' > /dev/null; find $P -type d -name '__pycache__' | wc -l > /dev/null
find $P -maxdepth 2 -type f -name '*.py' -size +40k > /dev/null; find $P -path '*email*' -name '*.py' > /dev/null
find $P -type f -newer $P/os.py > /dev/null; find /etc -type f -mtime -10000 > /dev/null
find $P/json $P/http -name '*.py' -exec grep -l 'def ' {} + > /dev/null; find $P/email -name '*.py' | xargs grep -c 'return' > /dev/null
find $P -type l > /dev/null; find /usr/lib -maxdepth 1 -name 'lib*.so*' > /dev/null; find /bin /sbin -type f -perm -u+x | wc -l > /dev/null
find / -name 'nonexistent_file_x' 2> /dev/null > /dev/null

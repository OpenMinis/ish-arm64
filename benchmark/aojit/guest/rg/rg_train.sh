# ripgrep recording workload (patterns differ from the measured ones): literal, regex, case, type
# filters, globs, counts, context, files, json output, PCRE2 and multiline
cd /usr/lib/python3.12
rg -n 'return None' > /dev/null; rg -l 'import re$' > /dev/null; rg -c 'except \w+Error' > /dev/null
rg -i 'deprecat' -g '*.py' > /dev/null; rg -t py -n 'async def' > /dev/null; rg -F -n '.join(' > /dev/null
rg -C 2 'raise ValueError' json > /dev/null; rg --files -g '*.txt' > /dev/null; rg --json 'lambda' email > /dev/null
rg -P -n '(?i)\bself\.(\w+)\s*=\s*\1\b' > /dev/null; rg -U -n 'def \w+\(self\):\n\s+"""' > /dev/null
rg -o -N '[A-Z][A-Z_]{5,}' > /dev/null; rg -v -c '^\s*#' json > /dev/null; rg --stats -q 'yield from' > /dev/null
echo rg-train-ok

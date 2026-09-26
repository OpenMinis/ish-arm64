seq 1 200000 > /tmp/aojit/shell/nums.txt
for i in 1 2 3; do
  sort -rn /tmp/aojit/shell/nums.txt | head -3
  grep -c '7$' /tmp/aojit/shell/nums.txt
  sed 's/\([0-9]\)\([0-9]\)/\2\1/g' /tmp/aojit/shell/nums.txt | awk '{s+=$1} END {print s}'
  cut -c1-3 /tmp/aojit/shell/nums.txt | uniq -c | wc -l
done
for i in $(seq 1 300); do echo $i | tr 0-9 a-j; done | md5sum

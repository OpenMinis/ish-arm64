// Everyday Node.js work: JSON, regex log parsing, CPU kernels, directory walk.
'use strict';
const fs = require('fs');
const path = require('path');
const DATA = '/opt/macjit/data';
const which = process.argv[2];

if (which === 'json') {
  const raw = fs.readFileSync(`${DATA}/data.json`, 'utf8');
  let obj, out;
  for (let i = 0; i < 5; i++) { obj = JSON.parse(raw); out = JSON.stringify(obj); }
  console.log(obj.length, out.length, obj.filter(r => r.active).length);
} else if (which === 'regex') {
  const pat = /^(\S+) \S+ \S+ \[([^\]]+)\] "(\w+) ([^ ?]+)[^"]*" (\d{3}) (\d+)/;
  const status = new Map(); let total = 0;
  for (const line of fs.readFileSync(`${DATA}/access.log`, 'utf8').split('\n')) {
    const m = pat.exec(line);
    if (!m) continue;
    status.set(m[5], (status.get(m[5]) || 0) + 1);
    total += Number(m[6]);
  }
  console.log(total, [...status.entries()].sort().join(' '));
} else if (which === 'compute') {
  const fib = n => (n < 2 ? n : fib(n - 1) + fib(n - 2));
  const sieve = n => { const a = new Uint8Array(n + 1); let c = 0;
    for (let i = 2; i <= n; i++) { if (!a[i]) { c++; for (let j = i * i; j <= n; j += i) a[j] = 1; } } return c; };
  let seed = 1; const rnd = () => (seed = (seed * 1103515245 + 12345) % 2147483648);
  const arr = Array.from({ length: 1000000 }, rnd).sort((a, b) => a - b);
  const words = Array.from({ length: 200000 }, () => rnd().toString(36));
  const m = new Map(); for (const w of words) m.set(w, (m.get(w) || 0) + 1);
  console.log(fib(30), sieve(5000000), arr[500000], m.size);
} else if (which === 'fswalk') {
  let files = 0, bytes = 0;
  const walk = d => { for (const e of fs.readdirSync(d, { withFileTypes: true })) {
    const p = path.join(d, e.name);
    if (e.isDirectory()) walk(p); else if (e.isFile()) { files++; bytes += fs.statSync(p).size; } } };
  walk('/usr/lib'); walk('/usr/share');
  console.log(files, bytes);
}

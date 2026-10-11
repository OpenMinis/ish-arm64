// Hot-code benchmark for node: warm up once, then N timed runs in-process.
const crypto = require('crypto');
function bench(name, n, fn) {
  let t0 = process.hrtime.bigint(); fn(); const cold = Number(process.hrtime.bigint() - t0) / 1e6;
  t0 = process.hrtime.bigint();
  for (let i = 0; i < n; i++) fn();
  const total = Number(process.hrtime.bigint() - t0) / 1e6;
  console.log(`Node.js|${name}|${(total / n).toFixed(3)}|${total.toFixed(1)}|${cold.toFixed(1)}`);
}
const doc = Array.from({length: 20}, (_, i) => ({id: i, name: 'user' + i, tags: ['a', 'b', String(i % 7)], score: i * 1.5, nested: {ok: i % 2 === 0, v: [i, i + 1]}}));
bench('json stringify+parse x10000', 10000, () => JSON.parse(JSON.stringify(doc)));
const buf = Buffer.alloc(65536, 7);
bench('sha256 64KB x2000', 2000, () => crypto.createHash('sha256').update(buf).digest('hex'));
const log = Array.from({length: 12000}, (_, i) => `2026-10-11 ${i % 17 ? 'INFO' : 'ERROR'} req=${i} ip=10.0.${i % 255}.${i * 7 % 255} took=${i % 997}ms`).join('\n');
const pats = [/ERROR .*?ip=(\d+\.\d+\.\d+\.\d+)/g, /took=(\d{3})ms/g];
bench('regex scan x50', 50, () => { for (const p of pats) (log.match(p) || []).length; });
bench('array sort 100K x10', 10, () => Array.from({length: 100000}, (_, i) => (i * 2654435761) % 1000003).sort((a, b) => a - b));
let s; bench('int loop 10M x5', 5, () => { s = 0; for (let i = 0; i < 1e7; i++) s += i % 7; });

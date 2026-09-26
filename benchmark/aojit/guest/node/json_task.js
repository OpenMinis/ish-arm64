// API response processing: filter, group, sort, re-serialize.
const fs = require('fs');
const d = JSON.parse(fs.readFileSync('/tmp/aojit/py/api.json', 'utf8'));
const by = new Map();
for (const it of d.items) {
  if (it.score < 0.1) continue;
  const k = it.owner.login;
  if (!by.has(k)) by.set(k, []);
  by.get(k).push({ id: it.id, score: +it.score.toFixed(3), tags: [...it.tags].sort(), day: it.created.slice(0, 10) });
}
const out = {};
for (const [k, v] of [...by.entries()].sort()) out[k] = v.sort((a, b) => b.score - a.score).slice(0, 50);
const s = JSON.stringify(out, null, 2);
fs.writeFileSync('/tmp/aojit/node/out.json', s);
console.log(s.length, Object.keys(JSON.parse(s)).length);

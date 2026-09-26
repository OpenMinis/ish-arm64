// JSON, regex, string and object churn
let rows = [];
for (let i = 0; i < 200000; i++) rows.push({id: i, name: 'item' + i, v: Math.sin(i) * 1000, tags: ['a' + (i % 7), 'b' + (i % 13)]});
const s = JSON.stringify(rows);
const back = JSON.parse(s);
let m = 0; for (const r of back) if (/item\d*7$/.test(r.name)) m += r.v;
const by = {}; for (const r of back) { const k = r.tags[0]; by[k] = (by[k] || 0) + r.v; }
console.log(s.length, Math.round(m), Object.keys(by).length);

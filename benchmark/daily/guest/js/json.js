// Parse a JSON file, transform, serialise.
const fs = require('fs');
const crypto = require('crypto');
const items = JSON.parse(fs.readFileSync(process.argv[2], 'utf8')).items;
const byTag = {};
for (const it of items) for (const t of it.tags) byTag[t] = (byTag[t] || 0) + 1;
const cheap = items.filter(it => it.price < 50).sort((a, b) => a.price - b.price || a.id - b.id).map(it => it.id);
const out = JSON.stringify({ tags: Object.keys(byTag).sort().map(k => [k, byTag[k]]), cheap: cheap.slice(0, 20), n: items.length });
console.log(crypto.createHash('sha256').update(out).digest('hex').slice(0, 16), out.length);

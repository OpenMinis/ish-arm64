// Agent file work: walk a tree, grep contents, write a report.
const fs = require('fs'), path = require('path');
const hits = [], sizes = {};
function walk(dir) {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) { walk(p); continue; }
    if (!e.name.endsWith('.py')) continue;
    const st = fs.statSync(p);
    sizes[path.extname(p)] = (sizes[path.extname(p)] || 0) + st.size;
    const text = fs.readFileSync(p, 'utf8');
    const m = text.match(/^def \w+\(/gm);
    if (m && m.length > 20) hits.push({ file: path.relative('/usr/lib', p), defs: m.length });
  }
}
walk('/usr/lib/python3.12/email'); walk('/usr/lib/python3.12/json'); walk('/usr/lib/python3.12/asyncio'); walk('/usr/lib/python3.12/unittest');
hits.sort((a, b) => b.defs - a.defs);
fs.writeFileSync('/tmp/aojit/node/report.md', '| file | defs |\n|---|---|\n' + hits.map(h => `| ${h.file} | ${h.defs} |`).join('\n'));
console.log(hits.length, sizes);

// Write, read back and delete many small files; walk a directory tree.
const fs = require('fs');
const path = require('path');
const dir = '/tmp/nodefs';
fs.rmSync(dir, { recursive: true, force: true });
fs.mkdirSync(dir);
for (let i = 0; i < 500; i++) fs.writeFileSync(path.join(dir, 'f' + i + '.txt'), 'line ' + i + '\n'.repeat(i % 7));
let bytes = 0;
for (const n of fs.readdirSync(dir)) bytes += fs.readFileSync(path.join(dir, n)).length;
let files = 0;
(function walk(d) { for (const e of fs.readdirSync(d, { withFileTypes: true })) e.isDirectory() ? walk(path.join(d, e.name)) : files++; })('/usr/lib/python3.12/email');
fs.rmSync(dir, { recursive: true, force: true });
console.log(bytes, files);

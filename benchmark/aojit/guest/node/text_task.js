// Log analysis, crypto, zlib, base64, URLs, markdown rendering.
const fs = require('fs'), crypto = require('crypto'), zlib = require('zlib');
const buf = fs.readFileSync('/tmp/aojit/py/app.log');
const text = buf.subarray(0, 6 << 20).toString('utf8');
const re = /^(\S+) \[(\w+)\] (\w+): (.*) footprint=(\d+)MB$/gm;
const cnt = new Map(); let peak = 0, m;
while ((m = re.exec(text)) !== null) {
  const k = m[2] + ':' + m[3];
  cnt.set(k, (cnt.get(k) || 0) + 1);
  peak = Math.max(peak, +m[5]);
}
const hash = crypto.createHash('sha256').update(buf).digest('hex');
const gz = zlib.gzipSync(buf.subarray(0, 4 << 20));
const back = zlib.gunzipSync(gz);
const b64 = Buffer.from(back.subarray(0, 1 << 20)).toString('base64');
const urls = [];
for (let i = 0; i < 20000; i++) {
  const u = new URL(`https://api.example.com/v1/sessions/${i}/screen?limit=${i % 80}&strip=true`);
  urls.push(u.searchParams.get('limit') + u.pathname.split('/')[3]);
}
const rows = [...cnt.entries()].sort((a, b) => b[1] - a[1]).map(([k, v]) => `| ${k} | ${v} |`).join('\n');
console.log(cnt.size, peak, hash.slice(0, 12), gz.length, b64.length, urls.length, rows.length, crypto.randomUUID().length);

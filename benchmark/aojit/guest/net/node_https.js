// Holdout for libssl/libcrypto recorded from Python: node's https + crypto (system OpenSSL on Alpine).
const https = require('https'), crypto = require('crypto');
const port = +process.argv[2], n = +process.argv[3];
let done = 0, bytes = 0;
function one(i) {
  https.get({ host: '127.0.0.1', port, path: i % 5 ? `/api/${i}` : '/blob/1', rejectUnauthorized: false, agent: false }, res => {
    res.on('data', d => bytes += d.length);
    res.on('end', () => { if (++done < n) one(done); else finish(); });
  }).on('error', e => { console.error(e); process.exit(1); });
}
function finish() {
  const buf = crypto.randomBytes(1 << 20); let h = 0;
  for (let i = 0; i < 16; i++) h ^= crypto.createHash('sha256').update(buf).digest()[0];
  console.log('node https ok', done, bytes > 0, h >= 0);
}
one(0);

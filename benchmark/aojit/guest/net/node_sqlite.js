// Holdout for libsqlite3 recorded from Python: node:sqlite on the system library.
const { DatabaseSync } = require('node:sqlite'); const fs = require('fs');
const path = '/tmp/aojit/net/node_tg.db'; for (const s of ['', '-wal', '-shm']) try { fs.unlinkSync(path + s); } catch {}
const db = new DatabaseSync(path);
db.exec(`PRAGMA journal_mode=WAL; CREATE TABLE messages(id INTEGER PRIMARY KEY, chat_id INT, msg_id INT, sender TEXT, date INT, text TEXT, meta TEXT, UNIQUE(chat_id,msg_id));
CREATE INDEX i1 ON messages(chat_id, date DESC);`);
const words = 'deploy build error agent python node server token proxy image release bug fix test merge cache'.split(' ');
let seed = 42; const rnd = n => (seed = (seed * 1103515245 + 12345) % 2147483648) % n;
const ins = db.prepare('INSERT INTO messages(chat_id,msg_id,sender,date,text,meta) VALUES (?,?,?,?,?,?)');
db.exec('BEGIN'); for (let i = 0; i < 20000; i++) {
  ins.run(rnd(30), i, 'u' + rnd(200), 1700000000 + i * 31, Array.from({ length: 3 + rnd(15) }, () => words[rnd(words.length)]).join(' '), JSON.stringify({ v: rnd(1000) }));
  if (i % 1000 === 999) { db.exec('COMMIT'); db.exec('BEGIN'); } } db.exec('COMMIT');
let acc = 0; const q1 = db.prepare('SELECT id, text FROM messages WHERE chat_id=? ORDER BY date DESC LIMIT 50');
const q2 = db.prepare("SELECT count(*) c FROM messages WHERE text LIKE ?"), q3 = db.prepare("SELECT sender, count(*) c FROM messages WHERE chat_id=? GROUP BY sender ORDER BY c DESC LIMIT 5");
const q4 = db.prepare("SELECT sum(json_extract(meta,'$.v')) s FROM messages WHERE chat_id=?");
for (let r = 0; r < 200; r++) { const c = rnd(30); acc += q1.all(c).length + q2.get(`%${words[rnd(words.length)]} ${words[rnd(words.length)]}%`).c + q3.all(c).length + (q4.get(c).s % 7); }
console.log('node sqlite acc', acc);

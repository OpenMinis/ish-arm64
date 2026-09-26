"""tg-hub-like message store: bulk ingest, upserts, indexed/range/FTS/JSON queries.
usage: sqlite_work.py <seed> <messages> <query rounds>"""
import json, os, random, sqlite3, sys, time
seed, n, rounds = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
random.seed(seed)
path = f'/tmp/aojit/net/tg_{seed}.db'
for s in ('', '-wal', '-shm', '-journal'):
    try: os.remove(path + s)
    except FileNotFoundError: pass
db = sqlite3.connect(path)
db.executescript('''
PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;
CREATE TABLE chats(id INTEGER PRIMARY KEY, title TEXT, kind TEXT, unread INTEGER DEFAULT 0);
CREATE TABLE messages(
  id INTEGER PRIMARY KEY, chat_id INTEGER NOT NULL, msg_id INTEGER NOT NULL,
  sender_id INTEGER, sender_name TEXT, date INTEGER NOT NULL, text TEXT,
  reply_to INTEGER, media_type TEXT, is_read INTEGER DEFAULT 0, raw_json TEXT,
  UNIQUE(chat_id, msg_id));
CREATE INDEX idx_msg_chat_date ON messages(chat_id, date DESC);
CREATE INDEX idx_msg_sender ON messages(sender_id);
CREATE VIRTUAL TABLE messages_fts USING fts5(text, content='messages', content_rowid='id');
CREATE TRIGGER messages_ai AFTER INSERT ON messages BEGIN
  INSERT INTO messages_fts(rowid, text) VALUES (new.id, new.text); END;
''')
words = ('deploy build error agent python node server token ssh proxy image video release '
         'meeting invoice report bug fix test merge branch update cache memory disk network').split()
chats = [(i, f'chat {i}', random.choice(['group', 'private', 'channel'])) for i in range(1, 41)]
db.executemany('INSERT INTO chats(id, title, kind) VALUES (?,?,?)', chats)
t0 = time.time(); base = 1_700_000_000
batch = []
for i in range(n):
    chat = random.randint(1, 40)
    text = ' '.join(random.choice(words) for _ in range(random.randint(3, 25)))
    raw = json.dumps({'views': random.randint(0, 5000), 'fwd': random.random() < 0.1,
                      'entities': [{'type': 'url', 'len': 20}] if random.random() < 0.2 else []})
    batch.append((chat, i, random.randint(1, 300), f'user{random.randint(1, 300)}', base + i * 37,
                  text, i - 1 if random.random() < 0.15 else None,
                  random.choice([None, None, None, 'photo', 'doc']), raw))
    if len(batch) == 1000:
        with db: db.executemany('INSERT INTO messages(chat_id,msg_id,sender_id,sender_name,date,text,reply_to,media_type,raw_json) VALUES (?,?,?,?,?,?,?,?,?)', batch)
        batch = []
if batch:
    with db: db.executemany('INSERT INTO messages(chat_id,msg_id,sender_id,sender_name,date,text,reply_to,media_type,raw_json) VALUES (?,?,?,?,?,?,?,?,?)', batch)
t1 = time.time(); acc = 0
for r in range(rounds):
    chat = random.randint(1, 40); w = random.choice(words)
    acc += len(db.execute('SELECT id, sender_name, text FROM messages WHERE chat_id=? ORDER BY date DESC LIMIT 50', (chat,)).fetchall())
    acc += db.execute('SELECT count(*) FROM messages WHERE chat_id=? AND date BETWEEN ? AND ?', (chat, base + r * 1000, base + r * 1000 + 400000)).fetchone()[0]
    acc += len(db.execute('SELECT m.id FROM messages_fts f JOIN messages m ON m.id=f.rowid WHERE messages_fts MATCH ? ORDER BY m.date DESC LIMIT 20', (f'{w} AND {random.choice(words)}',)).fetchall())
    acc += db.execute("SELECT count(*) FROM messages WHERE text LIKE ?", (f'%{w} {random.choice(words)}%',)).fetchone()[0]
    acc += len(db.execute("SELECT sender_id, count(*) c FROM messages WHERE chat_id=? GROUP BY sender_id ORDER BY c DESC LIMIT 10", (chat,)).fetchall())
    acc += db.execute("SELECT coalesce(sum(json_extract(raw_json, '$.views')),0) FROM messages WHERE chat_id=? AND json_extract(raw_json,'$.fwd')", (chat,)).fetchone()[0] % 7
    with db:
        db.execute('UPDATE messages SET is_read=1 WHERE chat_id=? AND date < ?', (chat, base + r * 20000))
        db.execute('INSERT OR REPLACE INTO messages(chat_id,msg_id,sender_id,sender_name,date,text,raw_json) VALUES (?,?,?,?,?,?,?)',
                   (chat, n + r, 1, 'me', base + n * 37 + r, f'reply {w} done', '{}'))
        db.execute('UPDATE chats SET unread=(SELECT count(*) FROM messages WHERE chat_id=? AND is_read=0) WHERE id=?', (chat, chat))
acc += len(db.execute('SELECT c.title, count(m.id), max(m.date) FROM chats c LEFT JOIN messages m ON m.chat_id=c.id GROUP BY c.id').fetchall())
db.close()
print('rows', n, 'acc', acc, f'ingest {t1 - t0:.2f}s queries {time.time() - t1:.2f}s', file=sys.stderr)
print('acc', acc)

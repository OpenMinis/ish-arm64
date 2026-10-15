python3 -c "
import sqlite3, json
c = sqlite3.connect(':memory:')
c.execute('create table t(id integer primary key, owner text, score real, tags text)')
d = json.load(open('/tmp/aojit/py/api.json'))['items']
c.executemany('insert into t values(?,?,?,?)', [(i['id'], i['owner']['login'], i['score'], ','.join(i['tags'])) for i in d])
print(c.execute('select owner, count(*), round(avg(score),4) from t group by owner order by 2 desc limit 3').fetchall())
print(c.execute(\"select count(*) from t where tags like '%error%'\").fetchone())
"

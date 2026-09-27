# Candidate workload (profile.sh): python3 -c one-liners over json/hashlib/base64/struct/datetime/math/binascii and one
# large JSON round trip; local only (no network), so the CPU numbers are stable.
for i in 1 2 3 4 5 6 7 8 9 10; do
  echo '{"items":[{"id":1,"v":"x"},{"id":2,"v":"y"}],"ok":true}' | python3 -c 'import json,sys,hashlib,base64,struct,datetime,math,binascii; d=json.load(sys.stdin); s=json.dumps(d,sort_keys=True); print(hashlib.sha256(s.encode()).hexdigest()[:8], base64.b64encode(s.encode())[:8], struct.pack("<I",len(s)).hex(), datetime.datetime.now().isoformat()[:4], math.sqrt(len(s)))' > /dev/null
done
python3 -c 'import json,hashlib,zlib; d=json.dumps([{"i":i,"s":str(i)*5} for i in range(60000)]); h=hashlib.sha256(d.encode()).hexdigest(); print(len(json.loads(d)), h[:8], len(zlib.compress(d.encode())))' > /dev/null

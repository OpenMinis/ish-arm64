# Candidate workload (profile.sh), needs network for the urllib part: python3 -c one-liners (json/hashlib/base64/struct/datetime/socket), plus urllib https
for i in 1 2 3 4 5 6 7 8 9 10; do
  echo '{"items":[{"id":1,"v":"x"},{"id":2,"v":"y"}],"ok":true}' | python3 -c 'import json,sys,hashlib,base64,struct,datetime,math,binascii; d=json.load(sys.stdin); s=json.dumps(d,sort_keys=True); print(hashlib.sha256(s.encode()).hexdigest()[:8], base64.b64encode(s.encode())[:8], struct.pack("<I",len(s)).hex(), datetime.datetime.now().isoformat()[:4], math.sqrt(len(s)))' > /dev/null
done
for i in 1 2 3; do python3 -c 'import json,urllib.request,socket,select,ssl; r=urllib.request.urlopen("https://www.cloudflare.com/cdn-cgi/trace", timeout=20); print(r.status, len(r.read()))' > /dev/null || echo PYFAIL; done
python3 -c 'import json,hashlib,zlib; d=json.dumps([{"i":i,"s":str(i)*5} for i in range(60000)]); h=hashlib.sha256(d.encode()).hexdigest(); print(len(json.loads(d)), h[:8], len(zlib.compress(d.encode())))' > /dev/null

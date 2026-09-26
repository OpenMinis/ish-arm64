// Agent tool calls: run shell commands and parse their output.
const { execSync, spawnSync } = require('child_process');
let n = 0;
for (let i = 0; i < 15; i++) {
  n += execSync('ls -la /usr/bin | wc -l').toString().trim() | 0;
  const r = spawnSync('grep', ['-c', 'import', '/usr/lib/python3.12/os.py']);
  n += parseInt(r.stdout.toString(), 10);
}
const ps = execSync('ps -o pid,comm').toString().split('\n').filter(Boolean).length;
console.log(n, ps > 0);

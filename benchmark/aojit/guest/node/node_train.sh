# Node.js agent workload (recording): every task once, startup a few times, then what agents
# run through node: npm, fetch, cf and MCP servers over stdio (start, handshake, tools/list, a
# few calls; also through npx, the way Minis starts them). Node runs in the mode kernel/exec.c
# picks (hybrid by default). The npm and MCP parts need the npm packages of images.json; without
# them they are skipped with a warning.
cd /tmp/aojit/node
for i in 1 2 3 4 5; do node -e 'console.log(JSON.stringify({ok: true, n: process.argv.length}))'; done
for t in json_task fs_task exec_task text_task agent_task; do node $t.js; done
# npm's own CLI (some rootfs wrap /usr/bin/npm in a shell script)
echo "require('/usr/lib/node_modules/npm/lib/cli.js')(process)" > /tmp/npmcli.js
node /tmp/npmcli.js --version; node /tmp/npmcli.js config get registry; node /tmp/npmcli.js ls -g --depth=0
rm -rf /tmp/trainnp; node /tmp/npmcli.js i --prefix /tmp/trainnp ms kleur --no-audit --no-fund
node -e "fetch('https://api.cloudflare.com/client/v4/ips?networks=jdcloud').then(r=>r.text()).then(t=>console.log(t.length))"
node -e "fetch('https://registry.npmjs.org/ms',{headers:{accept:'application/json'}}).then(r=>r.json()).then(j=>console.log(Object.keys(j.versions).length))"
if command -v cf > /dev/null; then
    cf --version; cf auth --help > /dev/null; CLOUDFLARE_API_TOKEN=bogus cf accounts list > /dev/null 2>&1
else echo "⚠️  cf not installed, skipped" >&2; fi
if command -v mcp-server-filesystem > /dev/null; then
    python3 mcp_probe.py filesystem memory everything context7 mcp-remote firecrawl github npx-filesystem
else echo "⚠️  MCP servers not installed, skipped" >&2; fi

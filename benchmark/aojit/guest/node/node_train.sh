# Node.js agent workload (recording): every task once, startup a few times.
cd /tmp/aojit/node
for i in 1 2 3 4 5; do node -e 'console.log(JSON.stringify({ok: true, n: process.argv.length}))'; done
for t in json_task fs_task exec_task text_task agent_task; do node $t.js; done

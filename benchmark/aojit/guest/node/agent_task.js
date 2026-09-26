// Chat-agent bookkeeping: build prompts from a long history, estimate tokens,
// truncate, and read tool outputs concurrently.
const fs = require('fs');
const history = [];
for (let i = 0; i < 400; i++) {
  history.push({ role: i % 2 ? 'assistant' : 'user', content: `turn ${i}: ` + 'lorem ipsum dolor sit amet '.repeat(20 + (i % 30)),
                 tool: i % 5 === 0 ? { name: 'shell_execute', input: JSON.stringify({ command: `ls -la /tmp/${i}` }) } : null });
}
const estimate = s => Math.ceil(s.split(/\s+/).length * 1.3);
let prompts = 0, tokens = 0;
for (let turn = 50; turn <= history.length; turn += 10) {
  let budget = 8000, parts = [];
  for (let i = turn - 1; i >= 0 && budget > 0; i--) {
    const h = history[i], text = `${h.role}: ${h.content}` + (h.tool ? `\n[tool ${h.tool.name}] ${h.tool.input}` : '');
    const t = estimate(text);
    if (t > budget) break;
    budget -= t; parts.unshift(text);
  }
  const prompt = JSON.stringify({ model: 'x', messages: parts.map(p => ({ role: 'user', content: p })) });
  prompts++; tokens += estimate(prompt);
}
(async () => {
  const files = fs.readdirSync('/usr/lib/python3.12').filter(f => f.endsWith('.py')).slice(0, 150);
  const texts = await Promise.all(files.map(f => fs.promises.readFile('/usr/lib/python3.12/' + f, 'utf8')));
  console.log(prompts, tokens, texts.reduce((a, t) => a + t.length, 0));
})();

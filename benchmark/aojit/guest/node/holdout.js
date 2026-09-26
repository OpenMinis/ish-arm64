// Not recorded: markdown rendering, CSV aggregation, a line diff, object sorting.
const lines = [];
for (let i = 0; i < 4000; i++) lines.push(i % 7 === 0 ? `## Section ${i}` : i % 3 === 0 ? `- item **${i}** with \`code\` and [link](http://x/${i})` : `Plain text ${i} _emph_ more words here`);
const md = lines.join('\n');
const html = md.split('\n').map(l => l.startsWith('## ') ? `<h2>${l.slice(3)}</h2>` : l.startsWith('- ') ?
  `<li>${l.slice(2).replace(/\*\*(.+?)\*\*/g, '<b>$1</b>').replace(/`(.+?)`/g, '<code>$1</code>').replace(/\[(.+?)\]\((.+?)\)/g, '<a href="$2">$1</a>')}</li>` :
  `<p>${l.replace(/_(.+?)_/g, '<i>$1</i>')}</p>`).join('\n');
const csv = []; for (let i = 0; i < 60000; i++) csv.push(`${i},${['a', 'b', 'c', 'd'][i % 4]},${(i * 7919) % 1000},${i % 13}`);
const agg = {}; for (const row of csv) { const [id, k, v, w] = row.split(','); (agg[k] ||= { n: 0, s: 0 }); agg[k].n++; agg[k].s += +v * +w; }
function lcs(a, b) { const dp = Array.from({ length: a.length + 1 }, () => new Int32Array(b.length + 1));
  for (let i = 1; i <= a.length; i++) for (let j = 1; j <= b.length; j++) dp[i][j] = a[i - 1] === b[j - 1] ? dp[i - 1][j - 1] + 1 : Math.max(dp[i - 1][j], dp[i][j - 1]);
  return dp[a.length][b.length]; }
const A = lines.slice(0, 900), B = lines.slice(50, 950).map((l, i) => i % 10 ? l : l + '!');
const objs = []; for (let i = 0; i < 80000; i++) objs.push({ id: i, name: 'n' + ((i * 2654435761) % 100000), v: Math.sin(i) });
objs.sort((x, y) => x.name < y.name ? -1 : x.name > y.name ? 1 : x.v - y.v);
console.log(html.length, JSON.stringify(agg).length, lcs(A, B), objs[0].name);

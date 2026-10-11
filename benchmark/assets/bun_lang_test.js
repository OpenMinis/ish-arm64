// Comprehensive JS language + stdlib correctness check for bun on iSH.
// Any assertion failure throws -> non-zero exit. Success prints a marker
// and exits 0. Designed to be a single self-contained smoke test that
// exercises much more than a trivial "1+1" or array literal.

function assert(cond, msg) {
  if (!cond) throw new Error("FAIL: " + msg);
}

// ---- Language features ----

// Classes, private fields, getters
class Counter {
  #n = 0;
  inc() { this.#n++; return this; }
  get value() { return this.#n; }
}
assert(new Counter().inc().inc().inc().value === 3, "class private fields");

// Destructuring + rest/spread
const { a, ...rest } = { a: 1, b: 2, c: 3 };
assert(a === 1 && rest.b === 2 && rest.c === 3, "object destructure + rest");
const [x, , z, ...tail] = [1, 2, 3, 4, 5];
assert(x === 1 && z === 3 && tail.join(",") === "4,5", "array destructure + rest");
assert([...[1, 2], ...[3, 4]].join(",") === "1,2,3,4", "spread in array literal");

// Template literals + tagged templates
const n = 21;
assert(`sum=${n * 2}` === "sum=42", "template literal");

// Default params + arrow functions + closures
const makeAdder = (base = 10) => (x) => base + x;
assert(makeAdder()(5) === 15, "closures + default params");

// Generators + iterator protocol
function* fib() {
  let [a, b] = [0, 1];
  while (true) { yield a; [a, b] = [b, a + b]; }
}
const first6 = [];
for (const v of fib()) { first6.push(v); if (first6.length === 6) break; }
assert(first6.join(",") === "0,1,1,2,3,5", "generator + for-of");

// Custom iterator protocol
const range = {
  [Symbol.iterator]() {
    let i = 0;
    return { next() { return i < 3 ? { value: i++, done: false } : { done: true }; } };
  },
};
assert([...range].join(",") === "0,1,2", "Symbol.iterator protocol");

// Proxy + Reflect
const proxied = new Proxy({}, { get(_t, k) { return `got:${String(k)}`; } });
assert(proxied.anything === "got:anything", "Proxy get trap");
assert(Reflect.has({ foo: 1 }, "foo") === true, "Reflect.has");

// try/catch/finally + custom errors
class MyError extends Error {}
let finallyRan = false;
try {
  try { throw new MyError("boom"); } finally { finallyRan = true; }
} catch (e) {
  assert(e instanceof MyError && e.message === "boom", "custom Error subclass");
}
assert(finallyRan, "finally block runs on throw");

// Optional chaining + nullish coalescing
const obj = { deep: { val: null } };
assert(obj?.deep?.val ?? "default" === "default", "optional chaining + nullish coalescing");
assert(obj?.missing?.val === undefined, "optional chaining short-circuit");

// ---- Standard library ----

// JSON
assert(JSON.stringify({ a: [1, 2, 3], b: "x" }) === '{"a":[1,2,3],"b":"x"}', "JSON.stringify");
assert(JSON.parse('{"k":[1,2]}').k[1] === 2, "JSON.parse");

// Math
assert(Math.max(1, 5, 3) === 5 && Math.min(1, 5, 3) === 1, "Math.max/min");
assert(Math.round(2.5) === 3 && Math.floor(2.9) === 2, "Math.round/floor");

// Array methods
assert([1, [2, [3, 4]]].flat(2).join(",") === "1,2,3,4", "Array.flat");
assert([1, 2, 3].map((v) => v * v).reduce((s, v) => s + v, 0) === 14, "Array.map/reduce");
assert(Array.from({ length: 3 }, (_, i) => i * 2).join(",") === "0,2,4", "Array.from");
assert([1, 2, 3].includes(2) === true, "Array.includes");

// String methods
assert("abc".padStart(5, "0") === "00abc", "String.padStart");
assert("  hi  ".trim() === "hi", "String.trim");
assert("a-b-c".split("-").join("+") === "a+b+c", "String.split/join");
assert("Hello".at(-1) === "o", "String.at negative index");

// Map / Set / WeakMap
const m = new Map([["a", 1], ["b", 2]]);
assert(m.get("a") === 1 && m.size === 2, "Map basic");
const s = new Set([1, 1, 2, 3, 3]);
assert(s.size === 3, "Set dedups");

// RegExp
const match = /(\d+)-(\d+)/.exec("order 12-34 done");
assert(match && match[1] === "12" && match[2] === "34", "RegExp.exec groups");
assert("a1b2c3".replace(/\d/g, "#") === "a#b#c#", "String.replace global regex");

// Date
const d = new Date(2024, 0, 15);
assert(d.getFullYear() === 2024 && d.getMonth() === 0 && d.getDate() === 15, "Date getters");

// TypedArray / ArrayBuffer
const u8 = new Uint8Array([1, 2, 3, 4]);
assert(u8.reduce((s, v) => s + v, 0) === 10, "TypedArray.reduce");
const dv = new DataView(new ArrayBuffer(4));
dv.setInt32(0, 305419896);
assert(dv.getInt32(0) === 305419896, "DataView get/set");

// Async / Promise (top-level await)
async function delayedDouble(v) {
  await Promise.resolve();
  return v * 2;
}
const results = await Promise.all([delayedDouble(1), delayedDouble(2), delayedDouble(3)]);
assert(results.join(",") === "2,4,6", "async/await + Promise.all");

const [settled] = await Promise.allSettled([Promise.reject(new Error("x"))]);
assert(settled.status === "rejected", "Promise.allSettled");

console.log("BUN_JS_TEST_PASS");

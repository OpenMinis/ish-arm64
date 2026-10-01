"use strict";
// /lib/fetch-polyfill.js: an undici global dispatcher on top of node:http / node:https, for the
// Node.js that iSH runs (kernel/exec.c injects --require for this file, first).
//
// Under --jitless (the default mode) V8 has no WebAssembly, so undici (Node's own fetch and
// every copy a package bundles) cannot build its llhttp parser, and the JS llhttp in
// wasm-polyfill.js is not a working replacement. With ISH_NODE_MODE=hybrid WebAssembly is real,
// but its generated code runs through the gadgets, so this transport is still ~0.6s faster on
// the first request. All undici copies share the global dispatcher through
// Symbol.for('undici.globalDispatcher.1'/'.2') and only create their own Agent when none is set,
// so installing this one first makes every undici fetch send through node:http, whose parser is
// Node's native llhttp. fetch keeps its own semantics (Headers, Request, Response, streams,
// FormData, AbortSignal, redirects, decompression); only the transport changes.
// Not covered: callers that build their own Agent / Client / ProxyAgent, and WebSocket upgrades.
// wasm-polyfill.js must stay: Node's internal undici calls WebAssembly.compile() when it loads.
//
// http/https are required on the first request, so scripts that never fetch pay nothing.
{
  const K1 = Symbol.for("undici.globalDispatcher.1");
  const K2 = Symbol.for("undici.globalDispatcher.2");
  let mods = null;
  const lazy = () => mods || (mods = {
    http: require("http"),
    https: require("https"),
    agents: {
      "http:": new (require("http").Agent)({ keepAlive: true }),
      "https:": new (require("https").Agent)({ keepAlive: true }),
    },
  });

  // undici passes headers as a flat [k, v, ...] array, an array of pairs, an iterable or an object.
  function toHeaderObject(h) {
    const out = {};
    const add = (k, v) => {
      if (v == null) return;
      k = String(k);
      const key = k.toLowerCase();
      if (key in out) out[key] = [].concat(out[key], String(v));
      else out[key] = Array.isArray(v) ? v.map(String) : String(v);
    };
    if (!h) return out;
    if (Array.isArray(h)) {
      if (h.length && Array.isArray(h[0])) for (const [k, v] of h) add(k, v);
      else for (let i = 0; i < h.length; i += 2) add(h[i], h[i + 1]);
    } else if (typeof h[Symbol.iterator] === "function") {
      for (const [k, v] of h) add(k, v);
    } else {
      for (const k of Object.keys(h)) add(k, h[k]);
    }
    return out;
  }

  const toBuffers = (raw) => raw.map((s) => Buffer.from(s, "latin1"));

  async function writeBody(req, body) {
    if (body == null) return req.end();
    if (typeof body === "string" || body instanceof Uint8Array) return req.end(body);
    if (body instanceof ArrayBuffer) return req.end(Buffer.from(body));
    if (typeof body.pipe === "function" && typeof body.on === "function") {
      body.on("error", (e) => req.destroy(e));
      return body.pipe(req);
    }
    if (typeof body[Symbol.asyncIterator] === "function" || typeof body[Symbol.iterator] === "function") {
      for await (const chunk of body) {
        if (req.destroyed) return;
        if (!req.write(typeof chunk === "string" ? chunk : Buffer.from(chunk.buffer || chunk, chunk.byteOffset || 0, chunk.byteLength))) {
          await new Promise((r) => req.once("drain", r));
        }
      }
      return req.end();
    }
    return req.end(String(body));
  }

  class HttpDispatcher {
    constructor() { this.closed = false; }

    dispatch(opts, handler) {
      const { http, https, agents } = lazy();
      let done = false;
      const fail = (err) => {
        if (done) return;
        done = true;
        try { handler.onError(err); } catch {}
      };
      try {
        if (opts.upgrade || opts.method === "CONNECT") {
          throw Object.assign(new Error("undici-http-dispatcher: upgrade/CONNECT not supported (no WebAssembly under --jitless)"), { code: "UND_ERR_NOT_SUPPORTED" });
        }
        const origin = new URL(typeof opts.origin === "string" ? opts.origin : String(opts.origin));
        const mod = origin.protocol === "https:" ? https : http;
        const req = mod.request({
          protocol: origin.protocol,
          hostname: origin.hostname.replace(/^\[|\]$/g, ""),
          port: origin.port || undefined,
          path: opts.path || "/",
          method: opts.method || "GET",
          headers: toHeaderObject(opts.headers),
          agent: agents[origin.protocol],
          servername: opts.servername || undefined,
        });
        const abort = (err) => { fail(err || new Error("aborted")); req.destroy(err); };
        if (handler.onConnect) handler.onConnect(abort);
        if (done) { req.destroy(); return true; }

        req.on("error", fail);
        req.on("response", (res) => {
          if (done) return res.destroy();
          if (handler.onResponseStarted) handler.onResponseStarted();
          const resume = () => res.resume();
          const cont = handler.onHeaders(res.statusCode, toBuffers(res.rawHeaders), resume, res.statusMessage || "");
          if (cont === false) res.pause();
          res.on("data", (chunk) => {
            if (done) return;
            if (handler.onData(chunk) === false) res.pause();
          });
          res.on("end", () => {
            if (done) return;
            done = true;
            handler.onComplete(toBuffers(res.rawTrailers || []));
          });
          res.on("error", fail);
          res.on("aborted", () => fail(new Error("response aborted")));
        });
        writeBody(req, opts.body).catch((e) => { fail(e); req.destroy(e); });
      } catch (err) {
        queueMicrotask(() => fail(err));
      }
      return true;
    }

    close() { this.closed = true; return Promise.resolve(); }
    destroy() { this.closed = true; return Promise.resolve(); }
    // Dispatcher is an EventEmitter in undici; nothing here emits.
    on() { return this; }
    once() { return this; }
    off() { return this; }
    removeListener() { return this; }
    emit() { return false; }
  }

  if (globalThis[K1] === undefined) {
    const d = new HttpDispatcher();
    // Same attributes as undici's setGlobalDispatcher, so a later setGlobalDispatcher still works.
    for (const k of [K1, K2]) {
      Object.defineProperty(globalThis, k, { value: d, writable: true, enumerable: false, configurable: false });
    }
  }
}

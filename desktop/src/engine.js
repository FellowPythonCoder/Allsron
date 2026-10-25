// Sreon Desktop — engine.js
// Talks to the SPRFST engine: one JSON request per line in, one JSON answer per line out.
'use strict';
const { spawn } = require('child_process');

class Engine {
  constructor(role) {
    this.role = role;
    this.proc = null;
    this.buffer = '';
    this.nextID = 1;
    this.waiting = new Map();
    this.alive = false;
    this.onExit = null;
  }

  /** Starts the engine process. Returns true when the process was spawned. */
  start({ interpreter, entry, resources, env = {} }) {
    this.buffer = '';
    const childEnv = Object.assign({}, process.env, env, {
      SREON_RES: resources,
      SPRFST_HOME: resources,
      SREON_SPRFST: interpreter,
      NO_COLOR: '1',
    });
    let p;
    try {
      p = spawn(interpreter, ['run', entry, '--', this.role === 'worker' ? '--worker' : '--serve'], {
        cwd: resources,
        env: childEnv,
        stdio: ['pipe', 'pipe', 'ignore'],
        windowsHide: true,
      });
    } catch (e) { return false; }
    this.proc = p;
    this.alive = true;
    p.stdout.setEncoding('utf8');
    p.stdout.on('data', (chunk) => this._consume(chunk));
    p.stdin.on('error', () => {});               // a closed pipe must never crash the app
    p.on('error', () => this._finished(p));
    p.on('exit', () => this._finished(p));
    return true;
  }

  // Only the current process may declare the engine finished.
  _finished(from) {
    if (this.proc !== from || !this.alive) return;
    this.alive = false;
    const pending = this.waiting;
    this.waiting = new Map();
    for (const done of pending.values()) done({ ok: false, error: 'engine stopped' });
    if (this.onExit) this.onExit();
  }

  _consume(chunk) {
    this.buffer += chunk;
    let nl;
    while ((nl = this.buffer.indexOf('\n')) >= 0) {
      const line = this.buffer.slice(0, nl);
      this.buffer = this.buffer.slice(nl + 1);
      if (!line) continue;
      let obj;
      try { obj = JSON.parse(line); } catch (e) { continue; }
      if (!obj || typeof obj.id !== 'number') continue;   // the greeting line has no id
      const done = this.waiting.get(obj.id);
      if (done) { this.waiting.delete(obj.id); done(obj); }
    }
  }

  /** Sends a request and resolves with the answer. Never rejects. */
  send(request) {
    return new Promise((resolve) => {
      if (!this.alive) { resolve({ ok: false, error: 'engine not running' }); return; }
      const id = this.nextID++;
      let line;
      try { line = JSON.stringify(Object.assign({}, request, { id })) + '\n'; }
      catch (e) { resolve({ ok: false, error: 'bad request' }); return; }
      this.waiting.set(id, resolve);
      try { this.proc.stdin.write(line); }
      catch (e) { this.waiting.delete(id); resolve({ ok: false, error: 'engine not running' }); }
    });
  }

  stop() {
    this.alive = false;
    try { if (this.proc) this.proc.kill(); } catch (e) { /* already gone */ }
  }
}

module.exports = { Engine };

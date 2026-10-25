// Sreon Desktop — shell.js
// The app-wide state: the engine, the settings it reports, theme, scripts and Shield rules.
'use strict';
const path = require('path');
const { Engine } = require('./engine');
const { RuleSet } = require('./rules');

/** Reads `--name:#rrggbb` out of the engine's theme CSS. */
function cssVar(css, name, fallback) {
  const m = new RegExp('--' + name + '\\s*:\\s*(#[0-9a-fA-F]{6})').exec(css || '');
  return m ? m[1] : fallback;
}

class Shell {
  constructor({ interpreter, resources, platform = process.platform, systemDark = true }) {
    this.interpreter = interpreter;
    this.resources = resources;
    this.entry = path.join(resources, 'engine', 'main.spf');
    this.platform = platform;
    this.systemDark = systemDark;
    this.core = new Engine('core');
    this.worker = new Engine('worker');
    this.settings = {};
    this.actions = [];
    this.css = '';
    this.scripts = {};
    this.rules = new RuleSet();
    this.windows = [];
    this.onSettings = null;
  }

  startEngine() {
    return this.core.start({ interpreter: this.interpreter, entry: this.entry, resources: this.resources });
  }

  startWorkerIfNeeded() {
    if (this.worker.alive) return;
    this.worker.start({ interpreter: this.interpreter, entry: this.entry, resources: this.resources });
  }

  // ------------------------------------------------------------ settings
  str(key, fallback) {
    const v = this.settings[key];
    return v === undefined || v === null ? fallback : String(v);
  }
  flag(key, fallback) {
    const v = this.settings[key];
    if (typeof v === 'boolean') return v;
    if (v === 'true') return true;
    if (v === 'false') return false;
    return fallback;
  }
  int(key, fallback) {
    const n = Number(this.settings[key]);
    return Number.isFinite(n) && this.settings[key] !== undefined && this.settings[key] !== null ? Math.trunc(n) : fallback;
  }
  get(key, fallback) { const v = this.settings[key]; return v === undefined || v === null ? fallback : v; }

  async boot() {
    const r = await this.core.send({ op: 'boot', dark: this.systemDark });
    if (r.settings) this.settings = r.settings;
    this.actions = (r.actions || []).map((a) => a.map(String));
    await this.refreshTheme();
    await this.loadScripts();
    await this.installRules();
    return r;
  }

  async fetchSettings() {
    const r = await this.core.send({ op: 'settings' });
    if (r.settings) this.settings = r.settings;
    return r;
  }

  async refreshTheme() {
    const r = await this.core.send({ op: 'theme', dark: this.systemDark });
    if (typeof r.css === 'string') this.css = r.css;
    return r;
  }

  async loadScripts() {
    const r = await this.core.send({ op: 'scripts' });
    this.scripts = r || {};
    return r;
  }

  async installRules() {
    const r = await this.core.send({ op: 'rules' });
    this.rules.load(r.lists || [], r.on);
    return this.rules.count;
  }

  /** JavaScript run at the start of every web page (never on sreon:// pages). */
  webSource() {
    const s = this.scripts || {};
    let out = '';
    if (s.shield_on) {
      out += "(function(){if(location.protocol==='sreon:')return;" + (s.config || '') + '})();' + (s.shield_js || '');
    }
    if (s.dark_pages) {
      out += "(function(){if(location.protocol==='sreon:'||!document.documentElement)return;" +
        "var s=document.createElement('style');s.id='sreon-dark';s.textContent=" + JSON.stringify(s.dark_css || '') +
        ';document.documentElement.appendChild(s);})();';
    }
    return out;
  }

  themeColors() {
    return { bg: cssVar(this.css, 'bg', '#0c0c10'), bg2: cssVar(this.css, 'bg2', '#111116') };
  }

  remove(w) { this.windows = this.windows.filter((x) => x !== w); }

  stop() { this.core.stop(); this.worker.stop(); }
}

module.exports = { Shell, cssVar };

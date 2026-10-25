// Sreon Desktop — controller.js
// One browser window: its tabs, the pieces of chrome around them, and every action the
// engine can ask for. All the Electron-specific work happens through `host`, a small adapter
// (see main.js), so this logic can be tested without a screen.
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const { computeLayout } = require('./layout');

const CHROME_VIEWS = ['sidebar', 'toolbar', 'panel', 'palette'];

/** Electron makes `sreon://newtab` into `sreon://newtab/`. The engine expects the short form. */
function normUrl(u) {
  return String(u || '').replace(/^(sreon:\/\/[^\/?#]+)\/(?=[?#]|$)/, '$1');
}

function validUrl(u) {
  try { new URL(u); return true; } catch (e) { return false; }
}

class WindowController {
  constructor(shell, host, { isPrivate = false } = {}) {
    this.shell = shell;
    this.host = host;
    this.isPrivate = isPrivate;
    this.tabs = [];
    this.nextTabID = 1;
    this.activeID = 0;
    this.splitID = null;
    this.wsName = 'Personal';
    this.panelShown = false;
    this.paletteShown = false;
    this.syncTimer = null;
    this.saveTimer = null;
    this.closing = false;
    this.size = { w: 1200, h: 800 };
    this.chromeReady = false;
  }

  // ----------------------------------------------------------- helpers
  get s() { return this.shell; }
  tab(id) { return this.tabs.find((t) => t.id === id) || null; }
  get current() { return this.tab(this.activeID); }
  tabDict(t) {
    return { id: t.id, title: t.title, url: t.url, loading: t.loading, pinned: t.pinned, sleeping: t.sleeping,
      ws: t.ws, back: t.canBack, forward: t.canForward, secure: t.secure, progress: t.progress, private: t.isPrivate };
  }
  needsToolbarPad() {
    // Only the Mac has window buttons floating over the toolbar.
    return this.s.platform === 'darwin' && (this.s.str('side_mode', 'expanded') === 'hidden' || this.s.str('side', 'left') === 'right');
  }

  // ------------------------------------------------------------- start
  start(startURL, restore) {
    this.loadChrome();
    const home = this.s.str('home', 'sreon://newtab');
    if (startURL) this.newTab(startURL, true);
    else if (restore && !this.isPrivate && this.restoreSession()) { /* restored */ }
    else if (this.s.str('startup', 'newtab') === 'home') this.newTab(home || 'sreon://newtab', true);
    else this.newTab('sreon://newtab', true);
    this.scheduleSync();
  }

  loadChrome() {
    this.host.chromeLoad('sidebar', 'sreon://chrome/sidebar');
    this.toolbarPad = this.needsToolbarPad();
    this.host.chromeLoad('toolbar', this.toolbarPad ? 'sreon://chrome/toolbar?pad=1' : 'sreon://chrome/toolbar');
    this.applyThemeColors();
    this.layoutAll();
  }

  // ------------------------------------------------------------ layout
  resized(w, h) { this.size = { w, h }; this.layoutAll(); }

  layoutAll() {
    const L = computeLayout(this.size.w, this.size.h, (k, d) => this.s.get(k, d), this.panelShown, !!this.splitID);
    this.layout = L;
    this.host.setChromeBounds('sidebar', L.sidebar);
    this.host.setChromeBounds('panel', this.panelShown ? L.panel : null);
    this.host.setChromeBounds('toolbar', L.toolbar);
    this.host.setChromeBounds('palette', this.paletteShown ? L.palette : null);
    this.layoutTabs();
  }

  layoutTabs() {
    const L = this.layout || computeLayout(this.size.w, this.size.h, (k, d) => this.s.get(k, d), this.panelShown, !!this.splitID);
    const second = this.splitID ? this.tab(this.splitID) : null;
    for (const t of this.tabs) {
      if (!t.hasView) continue;
      if (t.id === this.activeID) this.host.showTab(t, L.active);
      else if (second && t.id === second.id) this.host.showTab(t, L.second);
      else this.host.showTab(t, null);
    }
  }

  applyThemeColors() { this.host.applyTheme(this.s.themeColors(), this.s.flag('glass', true)); }

  pushTheme() {
    const js = "var t=document.getElementById('theme');if(t){t.textContent=" + JSON.stringify(this.s.css) + ';}';
    for (const v of CHROME_VIEWS) this.host.chromeEval(v, js);
    for (const t of this.tabs) if (t.hasView && t.url.startsWith('sreon://')) this.host.tabEval(t, js);
  }

  relayoutFromSettings() {
    this.applyThemeColors();
    this.layoutAll();
    const pad = this.needsToolbarPad();
    if (pad !== this.toolbarPad) this.toolbarPad = pad;
    this.refreshChrome(null);
  }

  // -------------------------------------------------------------- tabs
  newTab(url, select, background = false) {
    const t = { id: this.nextTabID++, url: normUrl(url), title: '', loading: false, progress: 0, pinned: false, sleeping: false,
      ws: this.wsName, canBack: false, canForward: false, secure: false, isPrivate: this.isPrivate,
      lastActive: Date.now(), hasView: false };
    let index = this.tabs.length;
    if (this.s.flag('new_tab_next', true)) {
      const i = this.tabs.findIndex((x) => x.id === this.activeID);
      if (i >= 0) index = i + 1;
    }
    this.tabs.splice(index, 0, t);
    this.makeView(t);
    if (select && !background) this.selectTab(t.id);
    this.scheduleSync();
    return t;
  }

  makeView(t) {
    t.hasView = true;
    t.sleeping = false;
    this.host.createTabView(t, { private: this.isPrivate });
    this.host.showTab(t, null);
    if (t.url) this.host.loadURL(t, t.url);
  }

  selectTab(id) {
    const t = this.tab(id);
    if (!t) return;
    if (t.ws !== this.wsName) this.wsName = t.ws;
    if (!t.hasView) this.makeView(t);
    this.activeID = id;
    t.lastActive = Date.now();
    this.layoutTabs();
    this.host.focusTab(t);
    this.scheduleSync();
    if (this.panelShown) this.refreshChrome(['panel']);
    this.updateWindowTitle();
    this.scheduleSessionSave();
  }

  updateWindowTitle() {
    const t = this.current;
    this.host.setTitle(this.s.flag('rtl', true) && t && t.title ? t.title : 'Sreon');
  }

  closeTab(id) {
    const idx = this.tabs.findIndex((t) => t.id === id);
    if (idx < 0) return;
    const t = this.tabs[idx];
    if (!t.isPrivate && t.url && !t.url.startsWith('sreon://newtab')) this.s.core.send({ op: 'closed', url: t.url });
    if (t.hasView) this.host.destroyTabView(t);
    t.hasView = false;
    this.tabs.splice(idx, 1);
    if (this.splitID === id) this.splitID = null;
    const inWS = this.tabs.filter((x) => x.ws === this.wsName);
    if (this.activeID === id) {
      if (inWS.length) this.selectTab(inWS[Math.min(idx, inWS.length - 1)].id);
      else if (this.tabs.length === 0 && this.s.windows.length > 1) { this.host.closeWindow(); return; }
      else this.newTab('sreon://newtab', true);
    }
    this.scheduleSync();
    this.scheduleSessionSave();
  }

  sleepTab(id) {
    const t = this.tab(id);
    if (!t || !t.hasView || id === this.activeID || id === this.splitID || t.sleeping) return;
    t.url = normUrl(this.host.tabURL(t) || t.url);
    this.host.destroyTabView(t);
    t.hasView = false;
    t.sleeping = true;
    t.loading = false;
    this.s.core.send({ op: 'count', what: 'sleeps' });
    this.scheduleSync();
  }

  sleepIdleTabs() {
    if (!this.s.flag('sleep', true)) return;
    const limit = Math.max(2, this.s.int('sleep_min', 15)) * 60 * 1000;
    for (const t of this.tabs) {
      if (t.hasView && t.id !== this.activeID && t.id !== this.splitID && !t.loading && Date.now() - t.lastActive > limit) this.sleepTab(t.id);
    }
  }

  open(url, where) {
    url = normUrl(url);
    if (!validUrl(url)) return;
    const t = this.current;
    if (where === 'here' && t) {
      if (!t.hasView) this.makeView(t);
      t.url = url;
      t.title = '';
      this.host.loadURL(t, url);
      this.layoutTabs();
      this.host.focusTab(t);
      this.scheduleSync();
    } else if (where === 'bg') this.newTab(url, false, true);
    else this.newTab(url, true);
  }

  // Page events reported by the host ------------------------------------
  tabUpdate(id, patch) {
    const t = this.tab(id);
    if (!t) return;
    let urlChanged = false;
    for (const k of ['title', 'loading', 'progress', 'canBack', 'canForward', 'secure']) {
      if (patch[k] !== undefined) t[k] = patch[k];
    }
    if (patch.url !== undefined) {
      const u = normUrl(patch.url);
      if (u && u !== t.url) { t.url = u; urlChanged = true; }
    }
    this.scheduleSync();
    if (urlChanged) this.scheduleSessionSave();
    if (id === this.activeID) this.updateWindowTitle();
  }

  async tabLoaded(id) {
    const t = this.tab(id);
    if (!t) return;
    if (/^https?:/i.test(t.url)) {
      if (!this.isPrivate) this.s.core.send({ op: 'visit', url: t.url, title: t.title });
      const r = await this.s.core.send({ op: 'zoomfor', url: t.url });
      if (typeof r.level === 'number' && Math.abs(r.level - 1) > 0.001) this.host.setZoom(t, r.level);
    }
    this.scheduleSync();
    if (id === this.activeID && this.panelShown) this.refreshChrome(['panel']);
  }

  /** Asks the engine whether a page may load. Resolves { allow, load }. */
  async checkNav(url) {
    const r = await this.s.core.send({ op: 'nav', url });
    return { allow: r.allow !== false, load: typeof r.load === 'string' ? r.load : '' };
  }

  // ---------------------------------------------------- engine syncing
  scheduleSync() {
    if (this.syncTimer || this.closing) return;
    this.syncTimer = setTimeout(() => { this.syncTimer = null; this.syncNow(); }, 60);
  }

  syncNow() {
    if (this.closing) return Promise.resolve();
    this.updateWindowTitle();
    return this.s.core.send({ op: 'tabs', tabs: this.tabs.map((t) => this.tabDict(t)), active: this.activeID, ws: this.wsName })
      .then(() => this.refreshChrome(null));
  }

  refreshChrome(only) {
    const names = only || (this.panelShown ? ['sidebar', 'toolbar', 'panel'] : ['sidebar', 'toolbar']);
    for (const n of names) {
      const arg = n === 'toolbar' && this.needsToolbarPad() ? '{pad:true}' : '{}';
      this.host.chromeEval(n, "window.sreon&&sreon.act('refresh'," + arg + ')');
    }
  }

  refreshEverything() {
    this.loadChrome();
    for (const t of this.tabs) if (t.hasView && t.url.startsWith('sreon://')) this.host.reload(t);
    this.scheduleSync();
  }

  // ------------------------------------------------------------ bridge
  /** A message from a Sreon page. `view` is the page's own name; resolves the reply for the page. */
  async handleBridge(op, view, data) {
    const s = this.s;
    if (op === 'act') {
      const name = (data && data.name) || '';
      let target = s.core;
      if (name.startsWith('st.exec.')) {
        s.startWorkerIfNeeded();
        if (s.worker.alive) target = s.worker;
      }
      const r = await target.send({ op: 'act', view, data });
      return this.finish(r, view);
    }
    if (op === 'ctx') {
      const r = await s.core.send({ op: 'ctx', view, data });
      const items = r.menu || [];
      if (items.length) this.host.popMenu(items, view, Number(data && data.x) || 0, Number(data && data.y) || 0);
      return {};
    }
    if (op === 'ready') {
      const r = await s.core.send({ op: 'ready' });
      const out = Object.assign({}, r); delete out.id;
      return out;
    }
    return {};
  }

  finish(r, view) {
    const out = Object.assign({}, r);
    delete out.id;
    const hostActions = Array.isArray(out.host) ? out.host : [];
    delete out.host;
    if (typeof out.toast === 'string' && ['sidebar', 'toolbar', 'panel', 'palette'].includes(view)) this.host.toast(out.toast);
    // The page gets its answer first; the window then does what the engine asked.
    setTimeout(() => { for (const a of hostActions) this.perform(a); }, 0);
    return out;
  }

  /** Runs an engine act on behalf of the window (menus, shortcuts, context menus). */
  async runAct(name, args, view = 'window') {
    const r = await this.s.core.send({ op: 'act', view, data: { name, args } });
    if (typeof r.toast === 'string') this.host.toast(r.toast);
    for (const a of Array.isArray(r.host) ? r.host : []) this.perform(a);
  }
  runCommand(id) { return this.runAct('cmd', { id }); }

  // ------------------------------------------------------ host actions
  perform(a) {
    const what = (a && a.do) || '';
    const s = this.s;
    const id = typeof a.id === 'number' ? a.id : this.activeID;
    switch (what) {
      case 'open': this.open(a.url || '', a.where || 'tab'); break;
      case 'select': if (typeof a.id === 'number') this.selectTab(a.id); break;
      case 'close': this.closeTab(id); break;
      case 'pin': { const t = this.tab(id); if (t) { t.pinned = !t.pinned; this.scheduleSync(); this.scheduleSessionSave(); } break; }
      case 'sleep': this.sleepTab(id); break;
      case 'sleep_others': for (const t of this.tabs.slice()) if (t.id !== this.activeID) this.sleepTab(t.id); break;
      case 'close_others': {
        const keep = typeof a.id === 'number' ? a.id : this.activeID;
        for (const t of this.tabs.slice()) if (t.id !== keep && !t.pinned && t.ws === this.wsName) this.closeTab(t.id);
        this.selectTab(keep);
        break;
      }
      case 'move_ws': {
        const t = this.tab(typeof a.id === 'number' ? a.id : 0);
        if (t && typeof a.ws === 'string') {
          t.ws = a.ws;
          if (t.id === this.activeID && a.ws !== this.wsName) this.switchWorkspace(this.wsName);
          this.scheduleSync();
        }
        break;
      }
      case 'workspace': this.switchWorkspace(a.name || this.wsName); break;
      case 'go': this.doGo(a.what || ''); break;
      case 'sidebar': case 'layout': s.fetchSettings().then(() => this.relayoutFromSettings()); break;
      case 'panel':
        this.panelShown = typeof a.show === 'boolean' ? a.show : !this.panelShown;
        this.layoutAll();
        this.applyThemeColors();
        if (this.panelShown) this.host.chromeLoad('panel', 'sreon://chrome/panel');
        break;
      case 'palette': this.showPalette(!!a.show, a.fill || ''); break;
      case 'rules':
        s.installRules().then(() => { const t = this.current; if (t && /^https?:/i.test(t.url)) this.host.reload(t); });
        break;
      case 'webconfig':
        s.loadScripts().then(() => { for (const t of this.tabs) if (t.hasView) this.host.setUserAgent(t, s.str('ua', '')); });
        break;
      case 'theme': case 'appearance':
        s.fetchSettings().then(() => s.refreshTheme()).then(() => {
          for (const w of s.windows) w.pushTheme();
          this.relayoutFromSettings();
        });
        break;
      case 'menu': this.host.rebuildMenu(); break;
      case 'refresh': this.refreshChrome(null); break;
      case 'extract': this.extractPage(a.then || 'brief'); break;
      case 'copy': this.host.clipboardWrite(a.text || ''); break;
      case 'find': this.host.showFind(); break;
      case 'zoom': if (typeof a.level === 'number' && this.current) this.host.setZoom(this.current, a.level); break;
      case 'split': this.toggleSplit(); break;
      case 'screenshot': this.host.screenshot(this.current); break;
      case 'pdf': this.host.savePDF(this.current); break;
      case 'print': this.host.printPage(this.current); break;
      case 'window': this.host.newWindow(!!a.private); break;
      case 'clear_data': this.host.clearData().then(() => this.host.toast('Browsing data cleared')); break;
      case 'fullscreen': this.host.toggleFullScreen(); break;
      case 'notify': this.host.notify(a.title || 'Sreon', a.body || ''); break;
      case 'reveal': if (a.path) this.host.reveal(a.path); break;
      case 'paste_settings':
        s.core.send({ op: 'paste_settings', text: this.host.clipboardRead() }).then(async (r) => {
          if (r.ok === true) {
            await s.fetchSettings(); await s.refreshTheme(); await s.loadScripts(); await s.installRules();
            for (const w of s.windows) { w.pushTheme(); w.relayoutFromSettings(); }
            this.host.toast('Settings applied');
          } else this.host.toast("The clipboard doesn't hold Sreon settings");
        });
        break;
      default: break;
    }
  }

  switchWorkspace(name) {
    this.wsName = name;
    const cur = this.current;
    if (cur && cur.ws === name) { this.scheduleSync(); return; }
    const first = this.tabs.find((t) => t.ws === name);
    if (first) this.selectTab(first.id);
    else this.newTab('sreon://newtab', true);
  }

  doGo(what) {
    const t = this.current;
    if (!t || !t.hasView) return;
    if (['back', 'forward', 'reload', 'stop', 'reload_hard'].includes(what)) this.host.navigate(t, what);
  }

  showPalette(show, fill) {
    this.paletteShown = show;
    if (show) {
      let url = 'sreon://chrome/palette';
      if (fill) url += '?fill=' + encodeURIComponent(fill);
      this.host.chromeLoad('palette', url);
    }
    this.layoutAll();
    this.host.focusPalette(show);
    if (!show && this.current) this.host.focusTab(this.current);
  }

  toggleSplit() {
    if (this.splitID) { this.splitID = null; this.layoutAll(); return; }
    const others = this.tabs.filter((t) => t.id !== this.activeID && t.ws === this.wsName).sort((a, b) => b.lastActive - a.lastActive);
    if (others.length) {
      const o = others[0];
      if (!o.hasView) this.makeView(o);
      this.splitID = o.id;
    } else {
      const t = this.newTab('sreon://newtab', false, true);
      this.splitID = t.id;
    }
    this.layoutAll();
  }

  async extractPage(kind) {
    const js = this.s.scripts.extract_js;
    const t = this.current;
    if (!t || !t.hasView || !js) return;
    let text = '';
    try { text = String((await this.host.tabEvalResult(t, js)) || ''); } catch (e) { return; }
    const r = await this.s.core.send({ op: 'digest', kind, data: text });
    if (typeof r.open === 'string') this.open(r.open, 'tab');
    if (typeof r.toast === 'string') this.host.toast(r.toast);
    if (r.apply && r.apply.view === 'panel') this.host.chromeEval('panel', 'window.sreon&&sreon.apply(' + JSON.stringify(r.apply) + ')');
  }

  // ----------------------------------------------------------- session
  sessionFile() {
    const base = process.env.SREON_HOME || path.join(os.homedir(), '.sreon');
    return path.join(base, 'session.json');
  }

  scheduleSessionSave() {
    if (this.isPrivate || this.closing) return;
    clearTimeout(this.saveTimer);
    this.saveTimer = setTimeout(() => this.saveSessionNow(), 1500);
  }

  saveSessionNow() {
    if (this.isPrivate || this.s.windows[0] !== this) return;
    const list = this.tabs.filter((t) => t.url && !t.url.startsWith('sreon://error'))
      .map((t) => ({ url: t.url, title: t.title, pinned: t.pinned, ws: t.ws }));
    const active = Math.max(0, this.tabs.findIndex((t) => t.id === this.activeID));
    try {
      const f = this.sessionFile();
      fs.mkdirSync(path.dirname(f), { recursive: true });
      fs.writeFileSync(f, JSON.stringify({ tabs: list, active, ws: this.wsName }));
    } catch (e) { /* a full disk must never stop browsing */ }
  }

  restoreSession() {
    let obj;
    try { obj = JSON.parse(fs.readFileSync(this.sessionFile(), 'utf8')); } catch (e) { return false; }
    if (!obj || !Array.isArray(obj.tabs) || !obj.tabs.length) return false;
    this.wsName = typeof obj.ws === 'string' ? obj.ws : 'Personal';
    const made = [];
    for (const item of obj.tabs) {
      if (!item || typeof item.url !== 'string') continue;
      made.push({ id: this.nextTabID++, url: normUrl(item.url), title: item.title || '', loading: false, progress: 0,
        pinned: !!item.pinned, sleeping: true, ws: item.ws || 'Personal', canBack: false, canForward: false, secure: false,
        isPrivate: false, lastActive: Date.now(), hasView: false });
    }
    if (!made.length) return false;
    this.tabs.push(...made);
    const idx = Math.max(0, Math.min(made.length - 1, Number.isInteger(obj.active) ? obj.active : 0));
    this.selectTab(made[idx].id);
    return true;
  }

  // ------------------------------------------------------------- close
  windowClosing() {
    this.saveSessionNow();
    this.closing = true;
    clearTimeout(this.syncTimer);
    clearTimeout(this.saveTimer);
    for (const t of this.tabs) if (t.hasView) { this.host.destroyTabView(t); t.hasView = false; }
    this.s.remove(this);
  }
}

module.exports = { WindowController, normUrl, validUrl };

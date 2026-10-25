'use strict';
// A stand-in for Electron: records what the controller asks for.
class MockHost {
  constructor() {
    this.calls = [];
    this.views = new Map();      // tab id -> { url, rect }
    this.chrome = {};            // name -> { url, rect, evals: [] }
    this.toasts = [];
    this.title = '';
    this.clip = '';
    this.closed = false;
  }
  rec(name, ...args) { this.calls.push([name, ...args]); }
  count(name) { return this.calls.filter((c) => c[0] === name).length; }
  last(name) { const l = this.calls.filter((c) => c[0] === name); return l[l.length - 1]; }

  createTabView(t) { this.rec('createTabView', t.id); this.views.set(t.id, { url: '', rect: null }); }
  destroyTabView(t) { this.rec('destroyTabView', t.id); this.views.delete(t.id); }
  loadURL(t, url) { this.rec('loadURL', t.id, url); const v = this.views.get(t.id); if (v) v.url = url; }
  tabURL(t) { const v = this.views.get(t.id); return v ? v.url : ''; }
  showTab(t, rect) { const v = this.views.get(t.id); if (v) v.rect = rect; }
  navigate(t, what) { this.rec('navigate', t.id, what); }
  reload(t) { this.rec('reload', t.id); }
  focusTab(t) { this.rec('focusTab', t.id); }
  setZoom(t, level) { this.rec('setZoom', t.id, level); }
  setUserAgent(t, ua) { this.rec('setUserAgent', t.id, ua); }
  tabEval(t, js) { this.rec('tabEval', t.id, js); }
  tabEvalResult(t, js) { this.rec('tabEvalResult', t.id); return Promise.resolve('page text'); }

  chromeLoad(name, url) { this.chrome[name] = Object.assign(this.chrome[name] || { evals: [] }, { url }); this.rec('chromeLoad', name, url); }
  setChromeBounds(name, rect) { this.chrome[name] = Object.assign(this.chrome[name] || { evals: [] }, { rect }); }
  chromeEval(name, js) { (this.chrome[name] = this.chrome[name] || { evals: [] }).evals.push(js); }
  focusPalette(show) { this.rec('focusPalette', show); }
  applyTheme(colors, glass) { this.rec('applyTheme', colors, glass); }

  setTitle(t) { this.title = t; }
  toast(text) { this.toasts.push(text); }
  popMenu(items, view) { this.rec('popMenu', items.length, view); }
  rebuildMenu() { this.rec('rebuildMenu'); }
  closeWindow() { this.closed = true; this.rec('closeWindow'); }
  newWindow(priv) { this.rec('newWindow', priv); }
  toggleFullScreen() { this.rec('fullscreen'); }
  showFind() { this.rec('showFind'); }
  screenshot() { this.rec('screenshot'); }
  savePDF() { this.rec('pdf'); }
  printPage() { this.rec('print'); }
  clearData() { this.rec('clearData'); return Promise.resolve(); }
  notify(t, b) { this.rec('notify', t, b); }
  reveal(p) { this.rec('reveal', p); }
  clipboardWrite(t) { this.clip = t; this.rec('clipboardWrite', t); }
  clipboardRead() { return this.clip; }
}
module.exports = { MockHost };

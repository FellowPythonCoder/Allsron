// Sreon Desktop — main.js
// The Linux and Windows shell. Everything the browser decides is in the SPRFST engine;
// this file only gives it windows, tabs, a way to load pages, and the operating system's services.
'use strict';
const path = require('path');
const fs = require('fs');
const {
  app, BaseWindow, BrowserWindow, WebContentsView, protocol, session, ipcMain, Menu, clipboard,
  shell: osShell, dialog, nativeTheme, Notification,
} = require('electron');
const { Shell } = require('./src/shell');
const { WindowController, normUrl } = require('./src/controller');

// -------------------------------------------------------------- where things are
const packaged = app.isPackaged;
const RES = packaged ? path.join(process.resourcesPath, 'sreon') : path.resolve(__dirname, '..');
const EXE = process.platform === 'win32' ? 'sprfst.exe' : 'sprfst';
const BIN = packaged ? path.join(process.resourcesPath, 'bin', EXE) : (process.env.SPRFST || path.join(RES, 'build', 'bin', EXE));
const PRELOAD = path.join(__dirname, 'preload.js');
const ICON = path.join(RES, 'assets', 'logo', 'Sreon-1024.png');

app.setName('Sreon');
protocol.registerSchemesAsPrivileged([
  { scheme: 'sreon', privileges: { standard: true, secure: true, supportFetchAPI: true, corsEnabled: true } },
]);

let shell = null;
const owners = new Map();          // webContents id -> { host, role }
const hosts = new Set();
const SPLASH_MIN = 2800;
let splash = null, splashShownAt = 0, splashGone = true, splashWatchdog = null;
const held = [];                   // windows kept back while the splash plays
let pendingURLs = [];
let launched = false;
let lastBoot = true;

// ------------------------------------------------------------------- sessions
const sessions = new Map();
function profileFor(isPrivate, id) {
  const key = isPrivate ? 'sreon-private-' + id : 'persist:sreon';
  if (sessions.has(key)) return sessions.get(key);
  const ses = session.fromPartition(key);
  ses.protocol.handle('sreon', handleSreon);
  ses.webRequest.onBeforeRequest({ urls: ['http://*/*', 'https://*/*', 'ws://*/*', 'wss://*/*'] }, (details, cb) => {
    // Shield. A page you asked for by name is never blocked, only what it pulls in.
    cb({ cancel: details.resourceType !== 'mainFrame' && shell.rules.shouldBlock(details.url) });
  });
  ses.on('will-download', (_e, item) => onDownload(item));
  sessions.set(key, ses);
  return ses;
}

const pageCache = new Map();
async function handleSreon(request) {
  const url = normUrl(request.url);
  const cacheable = url.includes('/assets/');
  if (cacheable && pageCache.has(url)) {
    const hit = pageCache.get(url);
    return new Response(hit.body, { status: 200, headers: { 'content-type': hit.mime } });
  }
  const r = await shell.core.send({ op: 'page', url });
  const body = typeof r.body === 'string' ? r.body : '';
  const mime = typeof r.mime === 'string' ? r.mime : 'text/html; charset=utf-8';
  const status = Number(r.status) || 200;
  if (cacheable && status === 200) pageCache.set(url, { body, mime });
  return new Response(body, { status, headers: { 'content-type': mime, 'cache-control': 'no-store' } });
}

// ------------------------------------------------------------------ downloads
function uniquePath(dir, name) {
  const ext = path.extname(name), base = path.basename(name, ext);
  let p = path.join(dir, name), n = 1;
  while (fs.existsSync(p)) p = path.join(dir, base + ' (' + n++ + ')' + ext);
  return p;
}
function onDownload(item) {
  const dest = uniquePath(app.getPath('downloads'), item.getFilename() || 'download');
  item.setSavePath(dest);
  const from = item.getURL();
  item.once('done', (_e, state) => {
    const h = focusedHost();
    if (state === 'completed') {
      shell.core.send({ op: 'download', name: path.basename(dest), path: dest, from, state: 'done' });
      if (h) h.toast('Downloaded ' + path.basename(dest));
    } else if (h) h.toast('A download failed');
  });
}

// --------------------------------------------------------------------- shortcuts
function parseCombo(spec) {
  const parts = spec.toLowerCase().split('+');
  const key = parts.pop();
  return { cmd: parts.includes('cmd'), ctrl: parts.includes('ctrl'), shift: parts.includes('shift'), alt: parts.includes('alt') || parts.includes('opt'), key };
}
// The key as printed on the cap, whatever Shift or the keyboard layout turned it into.
function keyOf(input) {
  const code = String(input.code || '');
  if (/^Key[A-Z]$/.test(code)) return code.slice(3).toLowerCase();
  if (/^Digit[0-9]$/.test(code)) return code.slice(5);
  const named = { BracketLeft: '[', BracketRight: ']', Comma: ',', Period: '.', Equal: '=', Minus: '-', Tab: 'tab' };
  if (named[code]) return named[code];
  const k = String(input.key || '').toLowerCase();
  return k === '+' ? '=' : k;
}
function comboMatches(c, input) {
  const wantCtrl = c.ctrl || (c.cmd && process.platform !== 'darwin');
  const wantMeta = c.cmd && process.platform === 'darwin';
  if (!!input.control !== !!wantCtrl) return false;
  if (!!input.meta !== !!wantMeta) return false;
  if (!!input.alt !== !!c.alt) return false;
  if (keyOf(input) !== c.key) return false;
  // Shift must match for letters and digits; for punctuation (zoom, brackets) either way is fine.
  return /[^a-z0-9]/.test(c.key) || c.key === 'tab' ? (c.key === 'tab' ? !!input.shift === !!c.shift : true) : !!input.shift === !!c.shift;
}
let combos = [];
function rebuildCombos() {
  combos = shell.actions.filter((a) => a[2]).map((a) => ({ id: a[0], c: parseCombo(a[2]) }));
}

// ---------------------------------------------------------------------- hosts
function focusedHost() {
  for (const h of hosts) if (h.win && h.win.isFocused()) return h;
  return [...hosts][0] || null;
}

class ElectronHost {
  constructor(isPrivate) {
    this.isPrivate = isPrivate;
    this.id = Math.random().toString(36).slice(2);
    this.tabViews = new Map();
    this.chrome = {};
    this.findView = null;
    this.findShown = false;
    const colors = shell.themeColors();
    this.win = new BaseWindow({
      width: 1280, height: 820, minWidth: 640, minHeight: 420, show: false,
      title: 'Sreon', icon: ICON, backgroundColor: colors.bg, autoHideMenuBar: true,
    });
    this.ses = profileFor(isPrivate, this.id);
    for (const name of ['sidebar', 'toolbar', 'panel', 'palette']) this.chrome[name] = this.makeChromeView(name);
    this.controller = new WindowController(shell, this, { isPrivate });
    shell.windows.push(this.controller);
    hosts.add(this);
    this.win.on('resize', () => this.fit());
    this.win.on('close', () => {
      this.controller.windowClosing();
      for (const v of Object.values(this.chrome)) this.dropView(v);
      this.dropView(this.findView);
      this.dropView(this.toastView);
      clearTimeout(this.toastTimer);
      hosts.delete(this);
      if (hosts.size === 0 && splashGone) setTimeout(() => app.quit(), 0);
      if (isPrivate) { this.ses.clearStorageData().catch(() => {}); this.ses.clearCache().catch(() => {}); }
    });
    this.fit();
  }

  fit() {
    const [w, h] = this.win.getContentSize();
    this.controller.resized(w, h);
  }

  dropView(view) {
    if (!view) return;
    owners.delete(view.webContents.id);
    try { this.win.contentView.removeChildView(view); } catch (e) { /* already removed */ }
    try { view.webContents.close(); } catch (e) { /* already closed */ }
  }

  watchKeys(wc) {
    wc.on('before-input-event', (event, input) => {
      if (input.type !== 'keyDown') return;
      this.lastInput = Date.now();
      for (const k of combos) {
        if (comboMatches(k.c, input)) { event.preventDefault(); this.controller.runCommand(k.id); return; }
      }
    });
  }

  makeChromeView(name) {
    const view = new WebContentsView({ webPreferences: { session: this.ses, preload: PRELOAD, sandbox: true, contextIsolation: true } });
    view.setBackgroundColor(name === 'palette' ? '#00000000' : shell.themeColors().bg2);
    view.setVisible(false);
    this.win.contentView.addChildView(view);
    owners.set(view.webContents.id, { host: this, role: name });
    this.watchKeys(view.webContents);
    return view;
  }

  raise() {
    // Things that float above the page go back on top whenever a page is added.
    for (const v of [this.chrome.sidebar, this.chrome.toolbar, this.chrome.panel, this.chrome.palette, this.findView]) {
      if (v) { try { this.win.contentView.addChildView(v); } catch (e) { /* not attached */ } }
    }
  }

  // ---- adapter used by WindowController -----------------------------------
  chromeLoad(name, url) { this.chrome[name].webContents.loadURL(url).catch(() => {}); }
  setChromeBounds(name, rect) {
    const v = this.chrome[name];
    if (!rect) { v.setVisible(false); return; }
    v.setBounds({ x: Math.round(rect.x), y: Math.round(rect.y), width: Math.max(1, Math.round(rect.w)), height: Math.max(1, Math.round(rect.h)) });
    v.setVisible(true);
  }
  chromeEval(name, js) {
    const wc = this.chrome[name] && this.chrome[name].webContents;
    if (wc && !wc.isDestroyed()) wc.executeJavaScript(js).catch(() => {});
  }
  focusPalette(show) { if (show) this.chrome.palette.webContents.focus(); }
  applyTheme(colors) {
    this.win.setBackgroundColor(colors.bg);
    for (const n of ['sidebar', 'toolbar', 'panel']) this.chrome[n].setBackgroundColor(colors.bg2);
  }

  createTabView(t, { private: priv }) {
    const view = new WebContentsView({
      webPreferences: { session: this.ses, preload: PRELOAD, sandbox: true, contextIsolation: true, nodeIntegration: false, spellcheck: true },
    });
    view.setVisible(false);
    const wc = view.webContents;
    this.tabViews.set(t.id, view);
    owners.set(wc.id, { host: this, role: 'tab', tabId: t.id });
    this.win.contentView.addChildView(view);
    this.raise();
    this.watchKeys(wc);
    wc.defaultUA = wc.getUserAgent();
    if (this.controller.s.str('ua', '')) wc.setUserAgent(this.controller.s.str('ua', ''));
    const c = this.controller;
    const nav = () => ({ canBack: wc.navigationHistory.canGoBack(), canForward: wc.navigationHistory.canGoForward() });
    wc.on('page-title-updated', (_e, title) => c.tabUpdate(t.id, { title }));
    wc.on('did-start-loading', () => c.tabUpdate(t.id, { loading: true, progress: 0.3 }));
    wc.on('did-stop-loading', () => { c.tabUpdate(t.id, Object.assign({ loading: false, progress: 1, url: wc.getURL() }, nav())); c.tabLoaded(t.id); });
    wc.on('did-navigate', (_e, url) => c.tabUpdate(t.id, Object.assign({ url, secure: url.startsWith('https:') }, nav())));
    wc.on('did-navigate-in-page', (_e, url, isMain) => { if (isMain) c.tabUpdate(t.id, Object.assign({ url }, nav())); });
    wc.on('did-start-navigation', (details) => {
      const url = details.url;
      if (!details.isMainFrame || details.isSameDocument || !/^https?:/i.test(url)) return;
      c.checkNav(url).then((r) => {
        if (r.allow || wc.isDestroyed()) return;
        wc.stop();
        if (r.load) wc.loadURL(r.load).catch(() => {});
      });
    });
    wc.on('dom-ready', () => {
      const css = shell.rules.cssFor(wc.getURL());
      if (css) wc.insertCSS(css).catch(() => {});
    });
    wc.on('did-fail-load', (_e, code, desc, url, isMain) => {
      if (!isMain || code === -3 || String(url).startsWith('sreon://')) return;
      wc.loadURL('sreon://error?u=' + encodeURIComponent(url) + '&r=' + encodeURIComponent(desc)).catch(() => {});
    });
    wc.on('render-process-gone', () => { if (!wc.isDestroyed()) wc.reload(); });
    wc.setWindowOpenHandler(({ url, disposition }) => {
      if (disposition === 'foreground-tab' || disposition === 'background-tab') {
        c.open(url, disposition === 'background-tab' ? 'bg' : 'tab');
      } else if (!shell.flag('popups', true) || Date.now() - (this.lastInput || 0) < 1600) {
        c.open(url, 'tab');
      } else {
        shell.core.send({ op: 'count', what: 'popups' });
        this.toast('Blocked a pop-up window');
      }
      return { action: 'deny' };
    });
    wc.on('context-menu', (_e, p) => {
      const items = [];
      if (p.linkURL) {
        items.push({ label: 'Open link in new tab', click: () => c.open(p.linkURL, 'tab') });
        items.push({ label: 'Copy link address', click: () => clipboard.writeText(p.linkURL) });
        items.push({ type: 'separator' });
      }
      if (p.selectionText) items.push({ role: 'copy' });
      if (p.isEditable) items.push({ role: 'paste' });
      items.push({ label: 'Back', enabled: wc.navigationHistory.canGoBack(), click: () => wc.navigationHistory.goBack() });
      items.push({ label: 'Forward', enabled: wc.navigationHistory.canGoForward(), click: () => wc.navigationHistory.goForward() });
      items.push({ label: 'Reload', click: () => wc.reload() });
      if (shell.flag('inspect', true)) items.push({ type: 'separator' }, { label: 'Inspect', click: () => wc.inspectElement(p.x, p.y) });
      const menu = Menu.buildFromTemplate(items);
      try { menu.popup({ window: this.win }); } catch (e) { menu.popup(); }
    });
  }

  destroyTabView(t) {
    const v = this.tabViews.get(t.id);
    this.tabViews.delete(t.id);
    this.dropView(v);
  }
  tabWC(t) { const v = this.tabViews.get(t.id); return v && !v.webContents.isDestroyed() ? v.webContents : null; }
  loadURL(t, url) { const wc = this.tabWC(t); if (wc) wc.loadURL(url).catch(() => {}); }
  tabURL(t) { const wc = this.tabWC(t); return wc ? wc.getURL() : ''; }
  showTab(t, rect) {
    const v = this.tabViews.get(t.id);
    if (!v) return;
    if (!rect) { v.setVisible(false); return; }
    v.setBounds({ x: Math.round(rect.x), y: Math.round(rect.y), width: Math.max(1, Math.round(rect.w)), height: Math.max(1, Math.round(rect.h)) });
    v.setVisible(true);
  }
  navigate(t, what) {
    const wc = this.tabWC(t);
    if (!wc) return;
    if (what === 'back') wc.navigationHistory.goBack();
    else if (what === 'forward') wc.navigationHistory.goForward();
    else if (what === 'stop') wc.stop();
    else if (what === 'reload_hard') wc.reloadIgnoringCache();
    else if (what === 'reload') { if (!wc.getURL()) wc.loadURL(t.url).catch(() => {}); else wc.reload(); }
  }
  reload(t) { const wc = this.tabWC(t); if (wc) wc.reload(); }
  focusTab(t) { const wc = this.tabWC(t); if (wc) wc.focus(); }
  setZoom(t, level) { const wc = this.tabWC(t); if (wc) wc.setZoomFactor(level); }
  setUserAgent(t, ua) { const wc = this.tabWC(t); if (wc) wc.setUserAgent(ua || wc.defaultUA); }
  tabEval(t, js) { const wc = this.tabWC(t); if (wc) wc.executeJavaScript(js).catch(() => {}); }
  tabEvalResult(t, js) { const wc = this.tabWC(t); return wc ? wc.executeJavaScript(js) : Promise.resolve(''); }

  setTitle(title) { this.win.setTitle(title); }
  toast(text) {
    // A short message floats above the page, then goes away.
    if (!this.toastView) {
      this.toastView = new WebContentsView({ webPreferences: { sandbox: true, contextIsolation: true } });
      this.toastView.setBackgroundColor('#00000000');
      this.win.contentView.addChildView(this.toastView);
    }
    const [w, h] = this.win.getContentSize();
    const html = '<body style="margin:0;background:transparent;font:13px system-ui,sans-serif;display:flex;justify-content:center;align-items:flex-end;height:100vh">' +
      '<div style="margin-bottom:10px;padding:9px 16px;border-radius:12px;background:rgba(14,17,28,.94);color:#eef1fb;border:1px solid rgba(255,255,255,.14)">' +
      String(text).replace(/&/g, '&amp;').replace(/</g, '&lt;') + '</div>';
    this.toastView.webContents.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(html)).catch(() => {});
    this.toastView.setBounds({ x: 0, y: Math.max(0, h - 70), width: w, height: 60 });
    this.toastView.setVisible(true);
    this.win.contentView.addChildView(this.toastView);
    clearTimeout(this.toastTimer);
    this.toastTimer = setTimeout(() => { try { this.toastView.setVisible(false); } catch (e) { /* closed */ } }, 2200);
  }
  popMenu(items, view, x, y) {
    const template = items.map((it) => it.sep ? { type: 'separator' }
      : { label: String(it.title || ''), click: () => this.controller.runAct(String(it.act || ''), it.args || {}) });
    const v = this.chrome[view];
    const b = v ? v.getBounds() : { x: 0, y: 0 };
    try { Menu.buildFromTemplate(template).popup({ window: this.win, x: Math.round(b.x + x), y: Math.round(b.y + y) }); }
    catch (e) { Menu.buildFromTemplate(template).popup(); }
  }
  rebuildMenu() { shell.core.send({ op: 'boot', dark: shell.systemDark }).then((r) => { shell.actions = (r.actions || []).map((a) => a.map(String)); rebuildCombos(); buildMenu(); }); }
  closeWindow() { this.win.close(); }
  newWindow(priv) { createWindow({ private: priv }); }
  toggleFullScreen() { this.win.setFullScreen(!this.win.isFullScreen()); }
  clearData() { return this.ses.clearStorageData().then(() => this.ses.clearCache()); }
  notify(title, body) { if (Notification.isSupported()) new Notification({ title, body }).show(); }
  reveal(p) { osShell.showItemInFolder(p); }
  clipboardWrite(text) { clipboard.writeText(text); }
  clipboardRead() { return clipboard.readText(); }

  stamp() {
    const d = new Date(), p = (n) => String(n).padStart(2, '0');
    return d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate()) + ' at ' + p(d.getHours()) + '.' + p(d.getMinutes()) + '.' + p(d.getSeconds());
  }
  screenshot(t) {
    const wc = t && this.tabWC(t);
    if (!wc) return;
    wc.capturePage().then((img) => {
      const f = path.join(app.getPath('downloads'), 'Sreon ' + this.stamp() + '.png');
      fs.writeFileSync(f, img.toPNG());
      this.toast('Screenshot saved to Downloads');
    }).catch(() => this.toast('Could not take a screenshot'));
  }
  savePDF(t) {
    const wc = t && this.tabWC(t);
    if (!wc) return;
    wc.printToPDF({}).then((data) => {
      const f = path.join(app.getPath('downloads'), 'Sreon ' + this.stamp() + '.pdf');
      fs.writeFileSync(f, data);
      this.toast('PDF saved to Downloads');
    }).catch(() => this.toast('Could not save a PDF'));
  }
  printPage(t) { const wc = t && this.tabWC(t); if (wc) wc.print(); }

  showFind() {
    if (!this.findView) {
      this.findView = new WebContentsView({ webPreferences: { preload: path.join(__dirname, 'preload-find.js'), sandbox: true, contextIsolation: true } });
      this.findView.setBackgroundColor('#00000000');
      this.findView.webContents.loadFile(path.join(__dirname, 'find.html')).catch(() => {});
      this.win.contentView.addChildView(this.findView);
      owners.set(this.findView.webContents.id, { host: this, role: 'find' });
    }
    this.findShown = true;
    this.placeFind();
    this.findView.setVisible(true);
    this.win.contentView.addChildView(this.findView);
    this.findView.webContents.focus();
    this.findView.webContents.send('find:show');
  }
  placeFind() {
    const L = this.controller.layout;
    if (!this.findView || !L) return;
    const w = 380;
    this.findView.setBounds({ x: Math.round(L.host.x + L.host.w - w - 8), y: Math.round(L.host.y + 4), width: w, height: 56 });
  }
  closeFind() {
    if (!this.findView) return;
    this.findShown = false;
    this.findView.setVisible(false);
    const wc = this.tabWC(this.controller.current || {});
    if (wc) { wc.stopFindInPage('clearSelection'); wc.focus(); }
  }
}

// ---- find in page and the bridge --------------------------------------------
ipcMain.on('find:query', (event, { text, forward }) => {
  const o = owners.get(event.sender.id);
  if (!o || o.role !== 'find') return;
  const wc = o.host.tabWC(o.host.controller.current || {});
  if (!wc) return;
  if (!text) { wc.stopFindInPage('clearSelection'); event.sender.send('find:result', { matches: 0, active: 0 }); return; }
  wc.removeAllListeners('found-in-page');
  wc.on('found-in-page', (_e, r) => { if (!event.sender.isDestroyed()) event.sender.send('find:result', { matches: r.matches, active: r.activeMatchOrdinal }); });
  wc.findInPage(String(text), { forward: forward !== false, findNext: true });
});
ipcMain.on('find:close', (event) => { const o = owners.get(event.sender.id); if (o) o.host.closeFind(); });

ipcMain.on('sreon:web-source', (event) => { event.returnValue = shell ? shell.webSource() : ''; });

ipcMain.handle('sreon:msg', async (event, msg) => {
  // Only Sreon's own pages may talk to the engine. A website never can.
  const frame = event.senderFrame;
  const o = owners.get(event.sender.id);
  if (!frame || !o || frame !== event.sender.mainFrame || !String(frame.url).startsWith('sreon://')) return {};
  if (!msg || typeof msg.op !== 'string') return {};
  const view = typeof msg.view === 'string' ? msg.view : '';
  const data = msg.data && typeof msg.data === 'object' ? msg.data : {};
  try { return await o.host.controller.handleBridge(msg.op, view, data); }
  catch (e) { return {}; }
});

// ------------------------------------------------------------------------ menu
function buildMenu() {
  const items = shell.actions.map((a) => ({
    label: a[1], accelerator: a[2] ? a[2].replace(/cmd/gi, 'CommandOrControl').replace(/opt/gi, 'Alt') : undefined,
    registerAccelerator: false,      // shortcuts are handled in watchKeys, so pages cannot swallow them
    click: () => { const h = focusedHost(); if (h) h.controller.runCommand(a[0]); },
  }));
  const template = [
    { label: 'Sreon', submenu: items.concat([{ type: 'separator' }, { role: 'quit' }]) },
    { label: 'Edit', submenu: [{ role: 'undo' }, { role: 'redo' }, { type: 'separator' }, { role: 'cut' }, { role: 'copy' }, { role: 'paste' }, { role: 'selectAll' }] },
  ];
  Menu.setApplicationMenu(Menu.buildFromTemplate(template));
}

// ---------------------------------------------------------------------- windows
function createWindow({ private: priv = false, startURL = null, restore = false, show = true } = {}) {
  const host = new ElectronHost(priv);
  host.controller.start(startURL, restore);
  if (show) host.win.show(); else held.push(host);
  return host;
}

function reveal(host) {
  host.win.show();
  host.win.focus();
}

function revealHeld() {
  const list = held.splice(0, held.length);
  for (const h of list) reveal(h);
}

function endSplash() {
  clearTimeout(splashWatchdog);
  const sp = splash;
  if (!sp) { splashGone = true; revealHeld(); return; }
  splash = null;
  const wait = Math.max(0, SPLASH_MIN - (Date.now() - splashShownAt));
  setTimeout(() => {
    let finished = false;
    const finish = () => {
      if (finished) return;
      finished = true;
      try { if (!sp.isDestroyed()) sp.destroy(); } catch (e) { /* gone already */ }
      splashGone = true;
      revealHeld();
    };
    try { sp.webContents.executeJavaScript('window.fadeOut&&window.fadeOut()').catch(() => {}); } catch (e) { /* ignore */ }
    setTimeout(finish, 800);
  }, wait);
}

function showSplash() {
  splash = new BrowserWindow({
    width: 460, height: 360, frame: false, transparent: true, resizable: false, movable: true, show: false,
    alwaysOnTop: true, skipTaskbar: true, hasShadow: true, icon: ICON, backgroundColor: '#00000000',
    webPreferences: { sandbox: true, contextIsolation: true },
  });
  splash.loadFile(path.join(RES, 'assets', 'splash.html')).catch(() => {});
  splash.once('ready-to-show', () => splash && splash.show());
  splashShownAt = Date.now();
  splashGone = false;
  splashWatchdog = setTimeout(endSplash, 14000);     // whatever happens, the splash never stays longer
}

function prefsFile() { return path.join(app.getPath('userData'), 'prefs.json'); }
function readPrefs() { try { return JSON.parse(fs.readFileSync(prefsFile(), 'utf8')); } catch (e) { return {}; } }
function writePrefs(p) { try { fs.mkdirSync(path.dirname(prefsFile()), { recursive: true }); fs.writeFileSync(prefsFile(), JSON.stringify(p)); } catch (e) { /* ignore */ } }

function urlsFromArgs(argv) { return argv.filter((a) => /^https?:\/\//i.test(a)); }

// -------------------------------------------------------------------------- start
if (!app.requestSingleInstanceLock()) { app.quit(); } else {
  app.on('second-instance', (_e, argv) => {
    const urls = urlsFromArgs(argv);
    const h = focusedHost();
    if (h) { for (const u of urls) h.controller.open(u, 'tab'); if (h.win.isMinimized()) h.win.restore(); h.win.focus(); }
  });

  app.whenReady().then(async () => {
    if (readPrefs().bootSplash !== false) showSplash();
    shell = new Shell({ interpreter: BIN, resources: RES, systemDark: nativeTheme.shouldUseDarkColors });
    if (!shell.startEngine()) {
      if (splash) { clearTimeout(splashWatchdog); splash.destroy(); splash = null; }
      dialog.showErrorBox("Sreon can't start its engine", 'The SPRFST engine was not found at:\n' + BIN + '\n\nReinstall Sreon.');
      app.quit();
      return;
    }
    shell.core.onExit = () => {
      if (!app.isReady() || shell.quitting) return;
      dialog.showErrorBox('Sreon stopped', 'The engine stopped unexpectedly. Sreon will close.');
      app.quit();
    };
    await shell.boot();
    rebuildCombos();
    buildMenu();
    launched = true;
    writePrefs(Object.assign(readPrefs(), { bootSplash: shell.flag('boot_anim', true) }));
    pendingURLs = urlsFromArgs(process.argv);
    const restore = shell.str('startup', 'newtab') === 'restore';
    const show = splashGone;
    const first = pendingURLs.shift();
    const host = createWindow({ startURL: first || null, restore: !first && restore, show });
    for (const u of pendingURLs) host.controller.open(u, 'tab');
    endSplash();
    setInterval(() => { for (const h of hosts) h.controller.sleepIdleTabs(); }, 60 * 1000);
    nativeTheme.on('updated', async () => {
      shell.systemDark = nativeTheme.shouldUseDarkColors;
      await shell.refreshTheme();
      for (const h of hosts) { h.controller.pushTheme(); h.controller.relayoutFromSettings(); }
    });
  });

  app.on('window-all-closed', () => { app.quit(); });
  app.on('before-quit', () => { if (shell) { shell.quitting = true; shell.stop(); } });
}

'use strict';
// Boots the real main.js against a stand-in for the `electron` module and the real engine.
// It cannot prove Electron behaves as assumed, but it catches wrong names, bad startup order
// and crashes in main.js, which nothing else exercises.
const Module = require('module');
const EventEmitter = require('events');
const assert = require('node:assert');
const fs = require('fs');
const os = require('os');
const path = require('path');

process.env.SREON_HOME = fs.mkdtempSync(path.join(os.tmpdir(), 'sreon-smoke-'));
const log = [];
let ids = 0;
const handlers = {};
const t0 = Date.now();
const at = (what) => log.push([Date.now() - t0, what]);

function makeWC(tag) {
  const wc = new EventEmitter();
  Object.assign(wc, {
    id: ++ids, tag, _url: '', mainFrame: { url: '' },
    loadURL: async (u) => { wc._url = u; wc.mainFrame.url = u; wc.emit('did-start-loading'); wc.emit('page-title-updated', {}, 'T ' + u); wc.emit('did-navigate', {}, u); wc.emit('did-stop-loading'); },
    loadFile: async () => {}, getURL: () => wc._url, executeJavaScript: async () => '', isDestroyed: () => false,
    close() { at('wc.close ' + tag); }, focus() {}, stop() {}, reload() {}, reloadIgnoringCache() {}, setZoomFactor() {},
    setUserAgent() {}, getUserAgent: () => 'UA', insertCSS: async () => {}, setWindowOpenHandler(f) { wc._open = f; },
    navigationHistory: { canGoBack: () => false, canGoForward: () => false, goBack() {}, goForward() {} },
    send() {}, findInPage() {}, stopFindInPage() {}, capturePage: async () => ({ toPNG: () => Buffer.from('') }),
    printToPDF: async () => Buffer.from(''), print() {}, inspectElement() {},
  });
  return wc;
}
class View {
  constructor() { this.webContents = makeWC('view'); this.visible = false; this.bounds = null; }
  setBackgroundColor() {} setVisible(v) { this.visible = v; } setBounds(b) { this.bounds = b; } getBounds() { return this.bounds || { x: 0, y: 0 }; }
}
class BaseWindow extends EventEmitter {
  constructor(o) { super(); this.o = o; this.shown = false; this.destroyed = false; this.children = []; this.contentView = { addChildView: (v) => { this.children = this.children.filter((c) => c !== v).concat(v); }, removeChildView: (v) => { this.children = this.children.filter((c) => c !== v); } }; BaseWindow.all.push(this); }
  getContentSize() { return [1280, 800]; } show() { this.shown = true; at('window.show'); } focus() {} isFocused() { return true; } isMinimized() { return false; }
  setTitle(t) { this.title = t; } setBackgroundColor() {} setFullScreen() {} isFullScreen() { return false; } close() { this.emit('close'); }
}
BaseWindow.all = [];
class BrowserWindow extends BaseWindow {
  constructor(o) { super(o); this.webContents = makeWC('splash'); }
  loadFile() { return Promise.resolve(); } once(ev, fn) { if (ev === 'ready-to-show') setImmediate(fn); else super.once(ev, fn); return this; }
  show() { this.shown = true; at('splash.show'); } destroy() { this.destroyed = true; at('splash.destroy'); } isDestroyed() { return this.destroyed; }
}
const sessionObj = () => ({ protocol: { handle(s, h) { handlers['protocol:' + s] = h; } }, webRequest: { onBeforeRequest(f, cb) { handlers.beforeRequest = cb; } }, on() {}, clearStorageData: async () => {}, clearCache: async () => {} });
const fake = {
  app: Object.assign(new EventEmitter(), { isPackaged: false, setName() {}, requestSingleInstanceLock: () => true, whenReady: () => Promise.resolve(), getPath: (n) => path.join(process.env.SREON_HOME, n), quit() { at('app.quit'); }, isReady: () => true }),
  BaseWindow, BrowserWindow, WebContentsView: View,
  protocol: { registerSchemesAsPrivileged() {} }, session: { fromPartition: () => sessionObj() },
  ipcMain: { on(n, f) { handlers[n] = f; }, handle(n, f) { handlers[n] = f; } },
  Menu: { buildFromTemplate: (t) => ({ template: t, popup() {} }), setApplicationMenu(m) { fake.menu = m; } },
  clipboard: { writeText() {}, readText: () => '' }, shell: { showItemInFolder() {} }, dialog: { showErrorBox(t, m) { at('ERRORBOX ' + t + ' ' + m); } },
  nativeTheme: Object.assign(new EventEmitter(), { shouldUseDarkColors: true }), Notification: Object.assign(class { show() {} }, { isSupported: () => false }),
};
const real = Module._load;
Module._load = function (req, ...rest) { return req === 'electron' ? fake : real.call(this, req, ...rest); };

(async () => {
  require('../main.js');
  await new Promise((r) => setTimeout(r, 4600));       // splash minimum 2.8 s + exit animation

  // 1. Startup order: splash first, then the browser window, and the splash is gone.
  const order = log.map((l) => l[1]);
  assert.ok(order.includes('splash.show'), 'splash shown');
  assert.ok(order.includes('window.show'), 'browser window shown');
  assert.ok(order.indexOf('splash.show') < order.indexOf('splash.destroy'), 'splash destroyed after shown');
  assert.ok(order.indexOf('splash.destroy') < order.indexOf('window.show'), 'the browser window appears only after the splash has closed');
  const shownAt = log.find((l) => l[1] === 'window.show')[0];
  assert.ok(shownAt >= 2700, 'splash played its full animation first (' + shownAt + ' ms)');
  assert.ok(!order.some((o) => o.startsWith('ERRORBOX')), 'no error dialog: ' + order.filter((o) => o.startsWith('ERRORBOX')));

  // 2. A window exists with its pieces.
  const win = BaseWindow.all.find((w) => !(w instanceof BrowserWindow));
  assert.ok(win && win.shown, 'main window exists and is shown');
  const views = win.children;
  assert.ok(views.length >= 5, 'sidebar, toolbar, panel, palette and a tab: ' + views.length);
  const urls = views.map((v) => v.webContents._url);
  assert.ok(urls.includes('sreon://chrome/sidebar') && urls.includes('sreon://chrome/toolbar') && urls.includes('sreon://newtab'), 'chrome and new tab loaded: ' + urls);

  // 3. The sreon:// scheme answers from the engine, with the trailing slash Electron adds.
  const h = handlers['protocol:sreon'];
  const page = await h({ url: 'sreon://newtab/' });
  const html = await page.text();
  assert.match(html, /Search or type an address/);
  const css = await h({ url: 'sreon://assets/sreon.css' });
  assert.match(css.headers.get('content-type'), /css/);
  assert.ok((await css.text()).length > 20000, 'stylesheet served');

  // 4. The bridge only answers Sreon's own pages.
  const tab = views.find((v) => v.webContents._url === 'sreon://newtab').webContents;
  const frame = { url: 'sreon://newtab' }; tab.mainFrame = frame;
  const ok = await handlers['sreon:msg']({ sender: tab, senderFrame: frame }, { op: 'ready', view: 'newtab', data: {} });
  assert.ok(ok && typeof ok === 'object', 'engine answers a Sreon page');
  tab.mainFrame = { url: 'https://evil.example/' };
  const bad = await handlers['sreon:msg']({ sender: tab, senderFrame: tab.mainFrame }, { op: 'act', view: 'newtab', data: { name: 'open', args: { url: 'sreon://settings' } } });
  assert.deepStrictEqual(bad, {}, 'a website can never talk to the engine');
  const sub = await handlers['sreon:msg']({ sender: tab, senderFrame: { url: 'sreon://newtab' } }, { op: 'act', view: 'newtab', data: { name: 'open', args: { url: 'sreon://settings' } } });
  assert.deepStrictEqual(sub, {}, 'a subframe can never talk to the engine');

  // 5. A real click on a dock button changes the page in the tab.
  tab.mainFrame = frame;
  await handlers['sreon:msg']({ sender: tab, senderFrame: frame }, { op: 'act', view: 'newtab', data: { name: 'open', args: { url: 'sreon://studio', where: 'here' } } });
  await new Promise((r) => setTimeout(r, 300));
  assert.strictEqual(tab._url, 'sreon://studio', 'the dock button opened Studio in this tab');

  // 6. Shield blocks a tracker and spares the page itself.
  const verdict = (url, type) => new Promise((r) => handlers.beforeRequest({ url, resourceType: type }, r));
  assert.deepStrictEqual(await verdict('https://stats.doubleclick.net/a.js', 'script'), { cancel: true });
  assert.deepStrictEqual(await verdict('https://example.com/', 'mainFrame'), { cancel: false });
  assert.deepStrictEqual(await verdict('https://example.com/app.js', 'script'), { cancel: false });

  // 7. Shortcuts and the web-page script hook exist.
  assert.ok(handlers['sreon:web-source'] && handlers['find:query'] && handlers['find:close']);
  const ev = {}; handlers['sreon:web-source'](ev); assert.strictEqual(typeof ev.returnValue, 'string');
  const pressed = new Promise((r) => { tab.once('x', r); });
  let prevented = false;
  tab.emit('before-input-event', { preventDefault() { prevented = true; } }, { type: 'keyDown', control: true, shift: false, alt: false, meta: false, key: 'l', code: 'KeyL' });
  assert.ok(prevented, 'Ctrl+L is taken by the browser, not the page');

  // 8. Closing the last window quits the app and nothing throws.
  win.close();
  await new Promise((r) => setTimeout(r, 200));
  assert.ok(log.some((l) => l[1] === 'app.quit'), 'closing the last window quits');
  console.log('electron smoke test: all checks passed');
  process.exit(0);
})().catch((e) => { console.error('SMOKE FAILED:', e && e.stack || e); console.error(JSON.stringify(log.slice(-12))); process.exit(1); });

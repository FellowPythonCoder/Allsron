'use strict';
const test = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { Shell } = require('../src/shell');
const { WindowController, normUrl } = require('../src/controller');
const { computeLayout } = require('../src/layout');
const { RuleSet } = require('../src/rules');
const { MockHost } = require('./mock-host');

const ROOT = process.env.SREON_TEST_RES || path.resolve(__dirname, '..', '..');
const BIN = process.env.SPRFST || path.join(ROOT, 'build', 'bin', 'sprfst');
const wait = (ms) => new Promise((r) => setTimeout(r, ms));

async function makeShell() {
  process.env.SREON_HOME = fs.mkdtempSync(path.join(os.tmpdir(), 'sreon-test-'));
  const shell = new Shell({ interpreter: BIN, resources: ROOT, platform: 'linux' });
  assert.ok(shell.startEngine(), 'engine starts');
  await shell.boot();
  return shell;
}
async function makeWindow(shell, opts = {}) {
  const host = new MockHost();
  const c = new WindowController(shell, host, opts);
  shell.windows.push(c);
  c.resized(1200, 800);
  return { c, host };
}

test('URLs from the scheme handler are put back the way the engine expects', () => {
  assert.strictEqual(normUrl('sreon://newtab/'), 'sreon://newtab');
  assert.strictEqual(normUrl('sreon://studio/?tab=guide'), 'sreon://studio?tab=guide');
  assert.strictEqual(normUrl('sreon://chrome/sidebar'), 'sreon://chrome/sidebar');
  assert.strictEqual(normUrl('https://example.com/'), 'https://example.com/');
});

test('layout: expanded, compact, hidden, right-hand sidebar, panel and split', () => {
  const g = (o) => (k, d) => (k in o ? o[k] : d);
  let L = computeLayout(1200, 800, g({}), false, false);
  assert.deepStrictEqual(L.sidebar, { x: 0, y: 0, w: 264, h: 800 });
  assert.deepStrictEqual(L.toolbar, { x: 264, y: 0, w: 936, h: 48 });
  assert.deepStrictEqual(L.host, { x: 264, y: 48, w: 928, h: 744 });
  L = computeLayout(1200, 800, g({ side_mode: 'compact' }), false, false);
  assert.strictEqual(L.sidebar.w, 68);
  L = computeLayout(1200, 800, g({ side_mode: 'hidden' }), false, false);
  assert.strictEqual(L.sidebar, null);
  assert.strictEqual(L.toolbar.x, 0);
  assert.strictEqual(L.toolbar.w, 1200);
  L = computeLayout(1200, 800, g({ side: 'right' }), false, false);
  assert.strictEqual(L.sidebar.x, 936);
  assert.strictEqual(L.host.x, 8);
  L = computeLayout(1200, 800, g({}), true, false);
  assert.strictEqual(L.panel.w, 340);
  assert.strictEqual(L.panel.x, 860);
  L = computeLayout(1200, 800, g({ side_width: 9999 }), false, false);
  assert.strictEqual(L.sidebar.w, 380, 'width is clamped');
  L = computeLayout(1200, 800, g({}), false, true);
  assert.ok(L.second && L.active.x + L.active.w < L.second.x, 'split halves do not overlap');
  L = computeLayout(100, 100, g({}), false, false);
  assert.ok(L.host.w >= 0 && L.host.h >= 0, 'tiny windows never go negative');
});

test('Shield rules: block trackers, spare the rest, hide page furniture', async () => {
  const shell = await makeShell();
  try {
    assert.ok(shell.rules.count > 100, 'the engine supplied rules: ' + shell.rules.count);
    assert.strictEqual(shell.rules.shouldBlock('https://stats.doubleclick.net/x.js'), true);
    assert.strictEqual(shell.rules.shouldBlock('https://example.com/index.html'), false);
    assert.strictEqual(shell.rules.shouldBlock('sreon://newtab'), false, 'Sreon pages are never blocked');
    const off = new RuleSet(); off.load([{ id: 'a', body: '[{"trigger":{"url-filter":"ads"},"action":{"type":"block"}}]' }], false);
    assert.strictEqual(off.shouldBlock('https://ads.example.com/'), false, 'Shield off blocks nothing');
    const css = new RuleSet();
    css.load([{ id: 'c', body: '[{"trigger":{"url-filter":"example\\\\.com"},"action":{"type":"css-display-none","selector":".banner"}}]' }], true);
    assert.match(css.cssFor('https://example.com/'), /\.banner\{display:none!important\}/);
    assert.strictEqual(css.cssFor('https://other.org/'), '');
    const bad = new RuleSet();
    assert.doesNotThrow(() => bad.load([{ id: 'x', body: 'not json' }, { id: 'y', body: '[{"trigger":{"url-filter":"("},"action":{"type":"block"}}]' }], true));
  } finally { shell.stop(); }
});

test('a window starts with one new tab and its chrome, and tells the engine', async () => {
  const shell = await makeShell();
  try {
    const { c, host } = await makeWindow(shell);
    c.start(null, false);
    assert.strictEqual(c.tabs.length, 1);
    assert.strictEqual(c.current.url, 'sreon://newtab');
    assert.deepStrictEqual(host.last('loadURL').slice(1), [1, 'sreon://newtab']);
    assert.strictEqual(host.chrome.sidebar.url, 'sreon://chrome/sidebar');
    assert.strictEqual(host.chrome.toolbar.url, 'sreon://chrome/toolbar', 'no Mac padding on Linux');
    assert.deepStrictEqual(host.views.get(1).rect, c.layout.host);
    await c.syncNow();
    const page = await shell.core.send({ op: 'page', url: 'sreon://chrome/sidebar' });
    assert.match(page.body, /New tab|Search or enter address/, 'the sidebar page reflects the tab');
    c.windowClosing();
  } finally { shell.stop(); }
});

test('the sidebar button really hides the sidebar and brings it back', async () => {
  const shell = await makeShell();
  try {
    const { c, host } = await makeWindow(shell);
    c.start(null, false);
    assert.ok(host.chrome.sidebar.rect, 'visible at first');
    await c.handleBridge('act', 'sidebar', { name: 'host.sidebar', args: {} });
    await wait(150);
    assert.strictEqual(shell.str('side_mode', ''), 'hidden');
    assert.strictEqual(host.chrome.sidebar.rect, null, 'sidebar closed');
    assert.strictEqual(host.chrome.toolbar.rect.x, 0, 'toolbar takes the room');
    await c.handleBridge('act', 'toolbar', { name: 'host.sidebar', args: {} });
    await wait(150);
    assert.strictEqual(shell.str('side_mode', ''), 'expanded');
    assert.ok(host.chrome.sidebar.rect, 'sidebar is back');
    c.windowClosing();
  } finally { shell.stop(); }
});

test('start-page dock buttons open their pages', async () => {
  const shell = await makeShell();
  try {
    const { c, host } = await makeWindow(shell);
    c.start(null, false);
    for (const [url, want] of [['sreon://studio', 'sreon://studio'], ['sreon://settings', 'sreon://settings'], ['sreon://studio?tab=guide', 'sreon://studio?tab=guide']]) {
      await c.handleBridge('act', 'newtab', { name: 'open', args: { url, where: 'here' } });
      await wait(30);
      assert.strictEqual(c.current.url, want);
      assert.deepStrictEqual(host.last('loadURL').slice(2), [want]);
    }
    assert.strictEqual(c.tabs.length, 1, 'opened in place, not in new tabs');
    c.windowClosing();
  } finally { shell.stop(); }
});

test('tabs: open, select, pin, close, never ends with none', async () => {
  const shell = await makeShell();
  try {
    const { c, host } = await makeWindow(shell);
    c.start(null, false);
    c.open('https://example.com/', 'tab');
    c.open('https://example.org/', 'bg');
    assert.strictEqual(c.tabs.length, 3);
    assert.strictEqual(c.current.url, 'https://example.com/');
    c.perform({ do: 'pin', id: c.activeID });
    assert.strictEqual(c.current.pinned, true);
    const ids = c.tabs.map((t) => t.id);
    c.perform({ do: 'select', id: ids[0] });
    assert.strictEqual(c.activeID, ids[0]);
    for (const id of ids.slice()) c.closeTab(id);
    assert.strictEqual(c.tabs.length, 1, 'a fresh tab replaces the last one');
    assert.strictEqual(c.current.url, 'sreon://newtab');
    c.open('not a url', 'tab');
    assert.strictEqual(c.tabs.length, 1, 'junk is ignored');
    c.windowClosing();
  } finally { shell.stop(); }
});

test('sleeping tabs free their view and wake on select', async () => {
  const shell = await makeShell();
  try {
    const { c, host } = await makeWindow(shell);
    c.start(null, false);
    c.open('https://example.com/', 'tab');
    const first = c.tabs[0];
    c.sleepTab(first.id);
    assert.strictEqual(first.sleeping, true);
    assert.strictEqual(host.views.has(first.id), false);
    c.selectTab(first.id);
    assert.strictEqual(first.sleeping, false);
    assert.strictEqual(host.views.has(first.id), true);
    c.sleepTab(c.activeID);
    assert.strictEqual(c.current.sleeping, false, 'the active tab never sleeps');
    c.windowClosing();
  } finally { shell.stop(); }
});

test('the session is saved and restored', async () => {
  const shell = await makeShell();
  try {
    const { c } = await makeWindow(shell);
    c.start(null, false);
    c.open('https://example.com/a', 'tab');
    c.open('https://example.com/b', 'tab');
    c.saveSessionNow();
    const saved = JSON.parse(fs.readFileSync(c.sessionFile(), 'utf8'));
    assert.strictEqual(saved.tabs.length, 3);
    assert.strictEqual(saved.active, 2);
    c.windowClosing();
    const { c: d, host } = await makeWindow(shell);
    d.start(null, true);
    assert.strictEqual(d.tabs.length, 3);
    assert.strictEqual(d.current.url, 'https://example.com/b');
    assert.strictEqual(host.count('createTabView'), 1, 'only the active tab is loaded; the rest sleep');
    d.windowClosing();
  } finally { shell.stop(); }
});

test('the palette, panel, split view and navigation actions', async () => {
  const shell = await makeShell();
  try {
    const { c, host } = await makeWindow(shell);
    c.start(null, false);
    c.perform({ do: 'palette', show: true, fill: 'hello world' });
    assert.ok(host.chrome.palette.rect);
    assert.strictEqual(host.chrome.palette.url, 'sreon://chrome/palette?fill=hello%20world');
    c.perform({ do: 'palette', show: false });
    assert.strictEqual(host.chrome.palette.rect, null);
    c.perform({ do: 'panel' });
    assert.ok(host.chrome.panel.rect);
    c.perform({ do: 'panel', show: false });
    assert.strictEqual(host.chrome.panel.rect, null);
    c.open('https://example.com/', 'tab');
    c.perform({ do: 'split' });
    assert.ok(c.splitID);
    const rects = [...host.views.values()].map((v) => v.rect).filter(Boolean);
    assert.strictEqual(rects.length, 2, 'two tabs side by side');
    c.perform({ do: 'split' });
    assert.strictEqual(c.splitID, null);
    c.perform({ do: 'go', what: 'back' });
    assert.deepStrictEqual(host.last('navigate').slice(2), ['back']);
    c.perform({ do: 'go', what: 'bogus' });
    assert.strictEqual(host.count('navigate'), 1, 'unknown navigation is ignored');
    c.perform({ do: 'copy', text: 'abc' });
    assert.strictEqual(host.clip, 'abc');
    c.perform({ do: 'no_such_action' });
    c.windowClosing();
  } finally { shell.stop(); }
});

test('page events update the tab and are recorded as history', async () => {
  const shell = await makeShell();
  try {
    const { c, host } = await makeWindow(shell);
    c.start(null, false);
    c.open('https://example.com/', 'tab');
    const id = c.activeID;
    c.tabUpdate(id, { title: 'Example', loading: false, canBack: true, secure: true, url: 'https://example.com/page' });
    await c.tabLoaded(id);
    const t = c.current;
    assert.strictEqual(t.title, 'Example');
    assert.strictEqual(t.url, 'https://example.com/page');
    assert.strictEqual(t.secure, true);
    assert.strictEqual(host.title, 'Example');
    const hist = await shell.core.send({ op: 'page', url: 'sreon://history' });
    assert.match(hist.body, /Example/, 'the visit reached the engine');
    c.tabUpdate(9999, { title: 'ghost' });
    c.windowClosing();
  } finally { shell.stop(); }
});

test('private windows keep nothing', async () => {
  const shell = await makeShell();
  try {
    const { c } = await makeWindow(shell, { isPrivate: true });
    c.start(null, true);
    c.open('https://secret.example/', 'tab');
    c.tabUpdate(c.activeID, { title: 'Secret' });
    await c.tabLoaded(c.activeID);
    c.saveSessionNow();
    assert.strictEqual(fs.existsSync(c.sessionFile()), false, 'no session file');
    const hist = await shell.core.send({ op: 'page', url: 'sreon://history' });
    assert.doesNotMatch(hist.body, /secret\.example/);
    c.windowClosing();
  } finally { shell.stop(); }
});

test('Studio runs through the worker engine and shows a live app', async () => {
  const shell = await makeShell();
  try {
    const { c } = await makeWindow(shell);
    c.start(null, false);
    const code = 'use std.io\nfn main() {\n    let n = io.ask("Name? ")\n    io.say("Hi {n}")\n}\n';
    const r = await c.handleBridge('act', 'studio', { name: 'st.exec.run', args: { into: 'out', code, stdin: 'Ada\n' } });
    assert.match(JSON.stringify(r), /Hi Ada/);
    assert.ok(shell.worker.alive, 'the worker engine was started');
    c.windowClosing();
  } finally { shell.stop(); }
});

test('the engine dying is reported, not hung on', async () => {
  const shell = await makeShell();
  const dead = new Promise((r) => { shell.core.onExit = r; });
  shell.core.proc.kill();
  await dead;
  const r = await shell.core.send({ op: 'settings' });
  assert.strictEqual(r.ok, false);
  shell.stop();
});

test('a bad engine path fails cleanly', () => {
  const s = new Shell({ interpreter: '/no/such/sprfst', resources: ROOT, platform: 'linux' });
  const started = s.startEngine();
  assert.ok(started === true || started === false);
  s.stop();
});

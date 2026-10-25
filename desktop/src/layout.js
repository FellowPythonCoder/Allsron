// Sreon Desktop — layout.js
// Where every piece of the window goes. Pure maths, so it can be tested without a screen.
// Rectangles are { x, y, w, h } with y measured from the top.
'use strict';

const TOOLBAR_H = 48;
const clamp = (n, lo, hi) => Math.max(lo, Math.min(hi, n));

/** `get(key, fallback)` reads a setting. */
function computeLayout(width, height, get, panelShown, split) {
  const mode = String(get('side_mode', 'expanded'));
  const right = String(get('side', 'left')) === 'right';
  let sw = 0;
  if (mode === 'expanded') sw = clamp(Number(get('side_width', 264)) || 264, 200, 380);
  if (mode === 'compact') sw = 68;
  const pw = panelShown ? clamp(Number(get('panel_width', 340)) || 340, 280, 520) : 0;
  const contentW = Math.max(200, width - sw - pw);
  const x0 = right ? 0 : sw;

  const sidebar = sw > 0 ? { x: right ? width - sw : 0, y: 0, w: sw, h: height } : null;
  const panel = pw > 0 ? { x: right ? width - sw - pw : width - pw, y: 0, w: pw, h: height } : null;
  const toolbar = { x: x0, y: 0, w: contentW, h: TOOLBAR_H };
  const host = { x: x0 + (right ? 8 : 0), y: TOOLBAR_H, w: contentW - 8, h: Math.max(0, height - TOOLBAR_H - 8) };
  const palette = { x: 0, y: 0, w: width, h: height };

  let active = { x: host.x, y: host.y, w: host.w, h: host.h };
  let second = null;
  if (split) {
    const half = Math.floor(host.w / 2);
    active = { x: host.x, y: host.y, w: half - 1, h: host.h };
    second = { x: host.x + half + 1, y: host.y, w: host.w - half - 1, h: host.h };
  }
  return { sidebar, panel, toolbar, host, palette, active, second };
}

module.exports = { computeLayout, TOOLBAR_H };

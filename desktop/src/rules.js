// Sreon Desktop — rules.js
// Turns the engine's Shield lists (WebKit content-blocker JSON) into a request matcher
// and a page-level cosmetic stylesheet.
'use strict';

function hostOf(url) {
  try { return new URL(url).hostname; } catch (e) { return ''; }
}

class RuleSet {
  constructor() { this.blocks = []; this.cosmetic = []; this.on = true; }

  /** `lists` is the engine's [{ id, body }] where body is a JSON string. */
  load(lists, on) {
    this.on = on !== false;
    this.blocks = [];
    this.cosmetic = [];
    for (const l of lists || []) {
      let rules;
      try { rules = JSON.parse(l.body); } catch (e) { continue; }
      if (!Array.isArray(rules)) continue;
      for (const r of rules) {
        if (!r || !r.trigger || !r.action) continue;
        let re;
        try { re = new RegExp(r.trigger['url-filter'], r.trigger['url-filter-is-case-sensitive'] ? '' : 'i'); }
        catch (e) { continue; }                       // a pattern JS cannot read is skipped, never fatal
        const t = r.action.type;
        if (t === 'block') this.blocks.push({ re });
        else if (t === 'css-display-none' && r.action.selector) this.cosmetic.push({ re, selector: r.action.selector });
      }
    }
    return this.blocks.length + this.cosmetic.length;
  }

  get count() { return this.blocks.length + this.cosmetic.length; }

  /** True when the request should be stopped. Sreon's own pages are never blocked. */
  shouldBlock(url) {
    if (!this.on || !/^https?:|^wss?:/i.test(url)) return false;
    for (const b of this.blocks) if (b.re.test(url)) return true;
    return false;
  }

  /** CSS that hides page furniture for this page, or ''. */
  cssFor(pageUrl) {
    if (!this.on || !/^https?:/i.test(pageUrl)) return '';
    const sel = [];
    for (const c of this.cosmetic) if (c.re.test(pageUrl)) sel.push(c.selector);
    return sel.length ? sel.join(',') + '{display:none!important}' : '';
  }
}

module.exports = { RuleSet, hostOf };

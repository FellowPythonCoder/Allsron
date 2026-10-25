/* Studio's editor niceties: indentation, brackets, comments, jump to a
   problem and keyboard shortcuts. The code itself is coloured and
   checked by the SPRFST engine; this only moves the cursor. */
(function () {
  var ed = function () { return document.getElementById("ed"); };
  function fire(t) { t.dispatchEvent(new Event("input", { bubbles: true })); }
  function lineStart(v, p) { return v.lastIndexOf("\n", p - 1) + 1; }
  var pairs = { "(": ")", "[": "]", "{": "}", '"': '"' };

  document.addEventListener("keydown", function (e) {
    var t = e.target;
    if (!t || t.id !== "ed") {
      if ((e.metaKey || e.ctrlKey) && e.key === "Enter" && ed()) { e.preventDefault(); var b = document.getElementById("runbtn"); if (b) b.click(); }
      return;
    }
    var v = t.value, s = t.selectionStart, en = t.selectionEnd;
    if ((e.metaKey || e.ctrlKey) && e.key === "Enter") { e.preventDefault(); document.getElementById("runbtn").click(); return; }
    if ((e.metaKey || e.ctrlKey) && (e.key === "s" || e.key === "S")) {
      e.preventDefault(); var sv = document.querySelector('[data-act="st.code.save"]'); if (sv) sv.click(); return;
    }
    if ((e.metaKey || e.ctrlKey) && e.key === "/") {
      e.preventDefault();
      var a = lineStart(v, s), z = v.indexOf("\n", en); if (z < 0) z = v.length;
      var block = v.slice(a, z).split("\n");
      var all = block.every(function (l) { return /^\s*~~/.test(l) || l.trim() === ""; });
      var out = block.map(function (l) {
        if (l.trim() === "") return l;
        return all ? l.replace(/~~ ?/, "") : l.replace(/^(\s*)/, "$1~~ ");
      }).join("\n");
      t.value = v.slice(0, a) + out + v.slice(z);
      t.selectionStart = a; t.selectionEnd = a + out.length; fire(t); return;
    }
    if (e.key === "Enter" && !e.metaKey && !e.ctrlKey && s === en) {
      var ls = lineStart(v, s), line = v.slice(ls, s);
      var indent = (line.match(/^\s*/) || [""])[0];
      var extra = /[{(\[]\s*$/.test(line) ? "    " : "";
      var after = v.charAt(s), close = (extra && (after === "}" || after === ")" || after === "]"));
      e.preventDefault();
      var ins = "\n" + indent + extra + (close ? "\n" + indent : "");
      t.value = v.slice(0, s) + ins + v.slice(en);
      t.selectionStart = t.selectionEnd = s + 1 + indent.length + extra.length;
      fire(t); return;
    }
    if (e.key === "Tab" && e.shiftKey) {
      e.preventDefault();
      var ls2 = lineStart(v, s), seg = v.slice(ls2, en), un = seg.replace(/^( {1,4}|\t)/gm, "");
      t.value = v.slice(0, ls2) + un + v.slice(en);
      t.selectionStart = Math.max(ls2, s - 4); t.selectionEnd = ls2 + un.length; fire(t); return;
    }
    if (e.key === "Backspace" && s === en && pairs[v.charAt(s - 1)] && v.charAt(s) === pairs[v.charAt(s - 1)]) {
      e.preventDefault(); t.value = v.slice(0, s - 1) + v.slice(s + 1); t.selectionStart = t.selectionEnd = s - 1; fire(t); return;
    }
    if (!e.metaKey && !e.ctrlKey && !e.altKey) {
      if (pairs[e.key] && (e.key !== '"' || !/\w/.test(v.charAt(s - 1)))) {
        if (s !== en) { e.preventDefault(); t.value = v.slice(0, s) + e.key + v.slice(s, en) + pairs[e.key] + v.slice(en); t.selectionStart = s + 1; t.selectionEnd = en + 1; fire(t); return; }
        if (!(e.key === '"' && v.charAt(s) === '"')) {
          e.preventDefault(); t.value = v.slice(0, s) + e.key + pairs[e.key] + v.slice(s); t.selectionStart = t.selectionEnd = s + 1; fire(t); return;
        }
      }
      if ((e.key === ")" || e.key === "]" || e.key === "}" || e.key === '"') && v.charAt(s) === e.key) {
        e.preventDefault(); t.selectionStart = t.selectionEnd = s + 1; return;
      }
    }
  }, true);

  document.addEventListener("click", function (e) {
    var g = e.target.closest("[data-goto]");
    if (!g) return;
    var t = ed(); if (!t) return;
    var bits = g.getAttribute("data-goto").split(":"), line = parseInt(bits[0], 10) || 1, col = parseInt(bits[1], 10) || 1;
    var lines = t.value.split("\n"), pos = 0;
    for (var i = 0; i < line - 1 && i < lines.length; i++) pos += lines[i].length + 1;
    pos += Math.max(0, col - 1);
    t.focus(); t.selectionStart = t.selectionEnd = Math.min(pos, t.value.length);
    var lh = parseFloat(getComputedStyle(t).lineHeight) || 22;
    t.scrollTop = Math.max(0, (line - 4) * lh);
  });
})();

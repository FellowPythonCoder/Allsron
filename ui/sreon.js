/* Sreon page bridge. Pages are drawn by the SPRFST engine; this tiny
   script only carries clicks, typing and drawing to it and puts the
   answers back. It has no knowledge of any particular page. */
(function () {
  var S = window.sreon = {};
  var VIEW = document.documentElement.getAttribute("data-view") || "";
  var native = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.sreon;
  var timers = {};

  S.call = function (op, data) {
    if (!native) return Promise.resolve(null);
    return native.postMessage({ op: op, view: VIEW, data: data || {} });
  };
  S.toast = function (t) {
    var el = document.querySelector(".toast");
    if (!el) { el = document.createElement("div"); el.className = "toast"; document.body.appendChild(el); }
    el.textContent = t; el.classList.add("show");
    clearTimeout(S._tt); S._tt = setTimeout(function () { el.classList.remove("show"); }, 2200);
  };
  S.apply = function (r) {
    if (!r) return;
    var k, el, sel;
    if (r.theme) { var st = document.getElementById("theme"); if (st) st.textContent = r.theme; }
    if (r.regions) for (k in r.regions) {
      el = document.querySelector('[data-region="' + k + '"]');
      if (el) el.innerHTML = r.regions[k];
    }
    if (r.attrs) for (sel in r.attrs) {
      el = document.querySelector(sel);
      if (el) for (k in r.attrs[sel]) el.setAttribute(k, r.attrs[sel][k]);
    }
    if (r.classes) for (sel in r.classes) {
      el = document.querySelector(sel);
      if (el) el.className = r.classes[sel];
    }
    if (r.values) for (sel in r.values) {
      el = document.querySelector(sel);
      if (el) el.value = r.values[sel];
    }
    if (r.focus) { el = document.querySelector(r.focus); if (el) { el.focus(); if (el.select && r.select) el.select(); } }
    if (r.scroll) { el = document.querySelector(r.scroll); if (el) el.scrollIntoView({ block: "nearest" }); }
    if (r.toast) S.toast(r.toast);
    if (r.reload) location.reload();
    S.tick();
  };
  S.act = function (name, args) {
    return S.call("act", { name: name, args: args || {} }).then(S.apply);
  };
  S.push = function (name, payload) { return S.act("push:" + name, payload || {}); };
  S.refresh = function () { return S.act("refresh"); };

  function argsOf(el) {
    var a = {};
    var raw = el.getAttribute("data-a");
    if (raw) { try { a = JSON.parse(raw); } catch (e) {} }
    return a;
  }
  function valueOf(el) {
    if (el.type === "checkbox") return el.checked;
    return el.value;
  }

  document.addEventListener("click", function (e) {
    var el = e.target.closest("[data-act]");
    if (!el || el.disabled) return;
    if (e.target.closest("[data-stop]") && !el.hasAttribute("data-stop")) return;
    var a = argsOf(el);
    if (el.tagName === "INPUT" && el.type === "checkbox") return;
    if (el.getAttribute("data-from")) {
      var src = document.querySelector(el.getAttribute("data-from"));
      if (src) a.value = src.value;
    }
    if (el.getAttribute("data-also")) {
      var al = document.querySelector(el.getAttribute("data-also"));
      if (al) a.stdin = al.value;
    }
    a.meta = e.metaKey; a.alt = e.altKey; a.shift = e.shiftKey;
    if (el.tagName === "A") e.preventDefault();
    S.act(el.getAttribute("data-act"), a);
  }, true);

  document.addEventListener("input", function (e) {
    var el = e.target.closest("[data-input]");
    if (!el) return;
    var name = el.getAttribute("data-input"), wait = parseInt(el.getAttribute("data-wait") || "0", 10);
    var send = function () { var a = argsOf(el); a.value = valueOf(el); S.act(name, a); };
    clearTimeout(timers[name]);
    if (wait > 0) timers[name] = setTimeout(send, wait); else send();
  });
  document.addEventListener("change", function (e) {
    var el = e.target.closest("[data-change]");
    if (!el) return;
    var a = argsOf(el); a.value = valueOf(el);
    S.act(el.getAttribute("data-change"), a);
  });
  document.addEventListener("keydown", function (e) {
    var el = e.target.closest("[data-keys]");
    if (el) {
      var keys = ["ArrowDown", "ArrowUp", "Enter", "Escape", "Tab"];
      if (keys.indexOf(e.key) >= 0 || (e.ctrlKey && (e.key === "n" || e.key === "p"))) {
        e.preventDefault();
        var a = argsOf(el); a.key = e.key; a.value = el.value; a.meta = e.metaKey; a.shift = e.shiftKey; a.alt = e.altKey; a.ctrl = e.ctrlKey;
        S.act(el.getAttribute("data-keys"), a);
        return;
      }
    }
    var en = e.target.closest("[data-enter]");
    if (en && e.key === "Enter" && !e.shiftKey && !(en.tagName === "TEXTAREA" && !e.metaKey)) {
      e.preventDefault();
      var b = argsOf(en); b.value = en.value; S.act(en.getAttribute("data-enter"), b);
    }
    var tab = e.target.closest("[data-tabkey]");
    if (tab && e.key === "Tab" && !e.shiftKey) {
      e.preventDefault();
      var s = tab.selectionStart, t = tab.selectionEnd, n = parseInt(tab.getAttribute("data-tabkey") || "4", 10);
      var pad = new Array(n + 1).join(" ");
      tab.value = tab.value.slice(0, s) + pad + tab.value.slice(t);
      tab.selectionStart = tab.selectionEnd = s + n;
      tab.dispatchEvent(new Event("input", { bubbles: true }));
    }
    if (e.key === "Escape" && document.documentElement.getAttribute("data-esc")) {
      S.act(document.documentElement.getAttribute("data-esc"), {});
    }
  }, true);
  document.addEventListener("contextmenu", function (e) {
    var el = e.target.closest("[data-ctx]");
    if (!el) return;
    e.preventDefault();
    var a = argsOf(el);
    S.call("ctx", { name: el.getAttribute("data-ctx"), args: a, x: e.clientX, y: e.clientY });
  });
  document.addEventListener("dblclick", function (e) {
    var el = e.target.closest("[data-dbl]");
    if (el) S.act(el.getAttribute("data-dbl"), argsOf(el));
  });
  document.addEventListener("scroll", function (e) {
    var el = e.target.closest && e.target.closest("[data-sync]");
    if (!el) return;
    document.querySelectorAll(el.getAttribute("data-sync")).forEach(function (other) { other.scrollTop = el.scrollTop; other.scrollLeft = el.scrollLeft; });
  }, true);

  /* live countdowns: <span data-until="epoch-ms">, <span data-since="epoch-ms"> */
  S.tick = function () {
    var now = Date.now();
    document.querySelectorAll("[data-until]").forEach(function (el) {
      var left = Math.max(0, parseInt(el.getAttribute("data-until"), 10) - now);
      var s = Math.round(left / 1000), m = Math.floor(s / 60);
      el.textContent = (m < 10 ? "0" : "") + m + ":" + (s % 60 < 10 ? "0" : "") + (s % 60);
      if (left <= 0 && !el._done) { el._done = true; S.act("timer.done", {}); }
    });
    document.querySelectorAll("[data-date]").forEach(function (el) {
      el.textContent = new Date().toLocaleDateString(undefined, { weekday: "short", month: "short", day: "numeric" });
    });
    document.querySelectorAll("[data-clock]").forEach(function (el) {
      var d = new Date(), h = d.getHours(), mi = d.getMinutes();
      if (el.getAttribute("data-clock") === "12") { h = h % 12 || 12; }
      el.textContent = (h < 10 && el.getAttribute("data-clock") === "24" ? "0" : "") + h + ":" + (mi < 10 ? "0" : "") + mi;
    });
  };
  setInterval(S.tick, 1000);
  S.tick();

  /* drawing: <svg data-gesture="board.draw" data-tool="pen"> sends finished gestures */
  function pt(svg, e) {
    var p = svg.createSVGPoint(); p.x = e.clientX; p.y = e.clientY;
    var m = svg.getScreenCTM(); if (!m) return [0, 0];
    var q = p.matrixTransform(m.inverse());
    return [Math.round(q.x * 10) / 10, Math.round(q.y * 10) / 10];
  }
  document.addEventListener("pointerdown", function (e) {
    var svg = e.target.closest("[data-gesture]");
    if (!svg || e.button !== 0) return;
    var tool = svg.getAttribute("data-tool") || "pen";
    var color = svg.getAttribute("data-color") || "#fff", width = svg.getAttribute("data-width") || "3";
    var start = pt(svg, e), pts = [start], NS = "http://www.w3.org/2000/svg", prev = null;
    if (tool === "text" || tool === "note") {
      var box = document.createElement("input");
      box.type = "text"; box.placeholder = tool === "note" ? "Note…" : "Text…";
      box.style.cssText = "position:fixed;left:" + e.clientX + "px;top:" + e.clientY + "px;z-index:50;width:220px;background:var(--surface2);border:1px solid var(--accent);border-radius:8px;padding:6px 10px";
      document.body.appendChild(box); box.focus();
      var done = false, finish = function (ok) {
        if (done) return; done = true;
        var v = box.value; box.remove();
        if (ok && v.trim()) S.act(svg.getAttribute("data-gesture"), { tool: tool, pts: [start], text: v, color: color, width: width });
      };
      box.addEventListener("keydown", function (k) { if (k.key === "Enter") finish(true); if (k.key === "Escape") finish(false); });
      box.addEventListener("blur", function () { finish(true); });
      e.preventDefault(); return;
    }
    svg.setPointerCapture(e.pointerId);
    var layer = svg.querySelector("[data-preview]");
    var make = function (name) { var n = document.createElementNS(NS, name); layer.appendChild(n); return n; };
    if (tool === "pen" || tool === "highlighter") {
      prev = make("path"); prev.setAttribute("fill", "none"); prev.setAttribute("stroke", color);
      prev.setAttribute("stroke-width", tool === "highlighter" ? width * 4 : width); prev.setAttribute("stroke-linecap", "round");
      prev.setAttribute("stroke-linejoin", "round"); if (tool === "highlighter") prev.setAttribute("opacity", ".35");
    } else if (tool === "eraser") {
      prev = make("circle"); prev.setAttribute("r", 14); prev.setAttribute("fill", "rgba(255,255,255,.12)"); prev.setAttribute("stroke", "rgba(255,255,255,.4)");
    } else if (tool === "pan") {
    } else { prev = make(tool === "rect" ? "rect" : tool === "ellipse" ? "ellipse" : "line"); prev.setAttribute("stroke", color); prev.setAttribute("stroke-width", width); prev.setAttribute("fill", "none"); }
    var last = start, vb = svg.viewBox.baseVal;
    var move = function (m) {
      var p = pt(svg, m); last = p;
      if (tool === "pan") { vb.x -= p[0] - start[0]; vb.y -= p[1] - start[1]; return; }
      if (tool === "pen" || tool === "highlighter" || tool === "eraser") {
        pts.push(p);
        if (tool === "eraser") { prev.setAttribute("cx", p[0]); prev.setAttribute("cy", p[1]); }
        else { prev.setAttribute("d", "M" + pts.map(function (q) { return q.join(" "); }).join(" L")); }
      } else if (tool === "rect") {
        prev.setAttribute("x", Math.min(start[0], p[0])); prev.setAttribute("y", Math.min(start[1], p[1]));
        prev.setAttribute("width", Math.abs(p[0] - start[0])); prev.setAttribute("height", Math.abs(p[1] - start[1]));
      } else if (tool === "ellipse") {
        prev.setAttribute("cx", (start[0] + p[0]) / 2); prev.setAttribute("cy", (start[1] + p[1]) / 2);
        prev.setAttribute("rx", Math.abs(p[0] - start[0]) / 2); prev.setAttribute("ry", Math.abs(p[1] - start[1]) / 2);
      } else { prev.setAttribute("x1", start[0]); prev.setAttribute("y1", start[1]); prev.setAttribute("x2", p[0]); prev.setAttribute("y2", p[1]); }
    };
    var up = function () {
      svg.removeEventListener("pointermove", move); svg.removeEventListener("pointerup", up); svg.removeEventListener("pointercancel", up);
      if (prev) prev.remove();
      if (tool === "pan") { S.act(svg.getAttribute("data-gesture"), { tool: "pan", view: [vb.x, vb.y, vb.width, vb.height] }); return; }
      var out = (tool === "pen" || tool === "highlighter" || tool === "eraser") ? pts : [start, last];
      if (out.length > 600) { var step = Math.ceil(out.length / 600); out = out.filter(function (_, i) { return i % step === 0; }); }
      S.act(svg.getAttribute("data-gesture"), { tool: tool, pts: out, color: color, width: width });
    };
    svg.addEventListener("pointermove", move); svg.addEventListener("pointerup", up); svg.addEventListener("pointercancel", up);
    e.preventDefault();
  });
  document.addEventListener("wheel", function (e) {
    var svg = e.target.closest("[data-zoom]");
    if (!svg) return;
    e.preventDefault();
    var vb = svg.viewBox.baseVal, f = e.deltaY > 0 ? 1.1 : 1 / 1.1;
    if (e.ctrlKey || e.metaKey) {
      var p = pt(svg, e), nw = vb.width * f, nh = vb.height * f;
      vb.x = p[0] - (p[0] - vb.x) * f; vb.y = p[1] - (p[1] - vb.y) * f; vb.width = nw; vb.height = nh;
    } else { vb.x += e.deltaX * vb.width / svg.clientWidth; vb.y += e.deltaY * vb.height / svg.clientHeight; }
  }, { passive: false });

  S.tick();
  S.call("ready", {}).then(S.apply);
})();

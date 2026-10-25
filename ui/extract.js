(function () {
  var cont = [], map = new Map(), items = [];
  function cid(el) {
    if (!el) return -1;
    if (!map.has(el)) { map.set(el, cont.length); cont.push({ t: el.tagName.toLowerCase(), c: ((el.className && el.className.baseVal === undefined ? el.className : "") + " " + (el.id || "")).slice(0, 160) }); }
    return map.get(el);
  }
  function meta(n) { var m = document.querySelector('meta[property="' + n + '"],meta[name="' + n + '"]'); return m ? (m.getAttribute("content") || "") : ""; }
  var nodes = document.querySelectorAll("p,h1,h2,h3,h4,li,blockquote,pre,img");
  var count = 0;
  for (var i = 0; i < nodes.length && count < 1500; i++) {
    var n = nodes[i], tag = n.tagName.toLowerCase();
    if (n.offsetParent === null && tag !== "img") continue;
    var o = { k: tag, c: cid(n.parentElement), g: cid(n.parentElement && n.parentElement.parentElement) };
    if (tag === "img") {
      var r = n.getBoundingClientRect();
      if (r.width < 200 || r.height < 120) continue;
      o.s = n.currentSrc || n.src; o.x = (n.alt || "").slice(0, 200);
    } else {
      var t = (n.innerText || "").replace(/\s+/g, " ").trim();
      if (!t) continue;
      o.x = t.slice(0, 3000);
      var links = n.querySelectorAll("a"), ll = 0;
      for (var j = 0; j < links.length; j++) ll += (links[j].innerText || "").length;
      o.l = ll;
    }
    items.push(o); count++;
  }
  var h1 = document.querySelector("h1");
  return JSON.stringify({
    url: location.href, title: document.title || "", h1: h1 ? (h1.innerText || "").trim() : "",
    site: meta("og:site_name"), by: meta("author") || meta("article:author"), desc: meta("og:description") || meta("description"),
    image: meta("og:image"), lang: document.documentElement.lang || "", cont: cont, items: items
  });
})()

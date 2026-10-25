/* Sreon Shield page script. Runs at document start in every web page when
   the Shield is on. Written once, kept small, touches nothing it does not
   need to. */
(function () {
  if (window.__sreonShield) return; window.__sreonShield = true;
  var cfg = window.__sreonShieldConfig || {};
  try { if (cfg.gpc) Object.defineProperty(navigator, "globalPrivacyControl", { get: function () { return true; } }); } catch (e) {}
  if (cfg.videoAds) {
    var host = location.hostname;
    var skip = function () {
      try {
        var p = document.querySelector(".html5-video-player.ad-showing, .video-ads .ytp-ad-player-overlay");
        var v = document.querySelector("video.html5-main-video, .html5-video-container video");
        if (p && v && v.duration && isFinite(v.duration)) { v.muted = true; v.currentTime = v.duration; v.playbackRate = 16; }
        var b = document.querySelector(".ytp-ad-skip-button, .ytp-ad-skip-button-modern, .ytp-skip-ad-button, .videoAdUiSkipButton");
        if (b) b.click();
        var c = document.querySelector(".ytp-ad-overlay-close-button");
        if (c) c.click();
      } catch (e) {}
    };
    if (/(^|\.)youtube\.com$|(^|\.)youtube-nocookie\.com$/.test(host)) {
      setInterval(skip, 350);
      new MutationObserver(skip).observe(document.documentElement, { childList: true, subtree: true });
    }
  }
  if (cfg.cookies) {
    var words = /^(reject all|decline all|only necessary|necessary only|use necessary cookies only|reject|decline|refuse|no thanks|deny|continue without accepting|disagree)$/i;
    var tries = 0;
    var t = setInterval(function () {
      tries++; if (tries > 12) { clearInterval(t); return; }
      try {
        var btns = document.querySelectorAll("#onetrust-reject-all-handler,#CybotCookiebotDialogBodyButtonDecline,.fc-secondary-button,button[aria-label*=eject],.cc-deny,.cky-btn-reject,.didomi-continue-without-agreeing");
        for (var i = 0; i < btns.length; i++) { btns[i].click(); clearInterval(t); return; }
        var all = document.querySelectorAll("[id*=cookie] button,[class*=cookie] button,[id*=consent] button,[class*=consent] button,[role=dialog] button");
        for (var j = 0; j < all.length; j++) { if (words.test((all[j].textContent || "").trim())) { all[j].click(); clearInterval(t); return; } }
        var de = document.documentElement, bd = document.body;
        if (bd && getComputedStyle(bd).overflow === "hidden" && !document.querySelector("[role=dialog],[aria-modal=true]")) { bd.style.setProperty("overflow", "auto", "important"); de.style.setProperty("overflow", "auto", "important"); }
      } catch (e) {}
    }, 700);
  }
})();

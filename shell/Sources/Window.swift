// Sreon — Window.swift
// One browser window: sidebar, toolbar, page area, panel and the command bar.
import Cocoa
import WebKit
import UserNotifications

final class ChromeWeb: WKWebView {
    override func willOpenMenu(_ menu: NSMenu, with event: NSEvent) { menu.removeAllItems() }
}

final class ChromeGuard: NSObject, WKNavigationDelegate {
    weak var owner: BrowserWindow?
    func webView(_ webView: WKWebView, decidePolicyFor navigationAction: WKNavigationAction, decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
        if navigationAction.request.url?.scheme == "sreon" || navigationAction.request.url?.scheme == "about" {
            decisionHandler(.allow)
        } else {
            decisionHandler(.cancel)
            if let url = navigationAction.request.url?.absoluteString { owner?.open(url, where: "tab") }
        }
    }
    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        if webView === owner?.paletteWeb {
            webView.evaluateJavaScript("var i=document.getElementById('pi');if(i){i.focus();i.select();}", completionHandler: nil)
        }
    }
}

final class BrowserWindow: NSObject, NSWindowDelegate, WKNavigationDelegate, WKUIDelegate, WKDownloadDelegate {
    let window: NSWindow
    let isPrivate: Bool
    let profile: Profile
    var privateProfile: Profile? { return isPrivate ? profile : nil }

    var tabs: [Tab] = []
    var activeID = 0
    var splitID: Int? = nil
    var nextTabID = 1
    var wsName = "Personal"
    var panelShown = false
    var paletteShown = false
    var lastClick = Date.distantPast

    let root = NSView()
    let sidebarFX = NSVisualEffectView()
    let panelFX = NSVisualEffectView()
    let hostView = NSView()
    let sidebarWeb: ChromeWeb
    let toolbarWeb: ChromeWeb
    let panelWeb: ChromeWeb
    let paletteWeb: ChromeWeb
    let toast = Toast()
    let chromeGuard = ChromeGuard()
    var findBar: FindBar?
    var toolbarPad = false
    var syncScheduled = false
    var lastChromeRefresh = Date.distantPast
    var downloads: [ObjectIdentifier: URL] = [:]
    var downloadFrom: [ObjectIdentifier: String] = [:]
    var sleepTimer: Timer?
    var clickMonitor: Any?
    var saveWork: DispatchWorkItem?

    // ----------------------------------------------------------------- setup
    init(isPrivate: Bool) {
        self.isPrivate = isPrivate
        self.profile = isPrivate ? Profile(isPrivate: true) : Shell.shared.normal
        func chrome() -> ChromeWeb {
            let cfg = WKWebViewConfiguration()
            cfg.userContentController = Shell.shared.chromeUCC
            cfg.setURLSchemeHandler(Shell.shared.schemes, forURLScheme: "sreon")
            let w = ChromeWeb(frame: .zero, configuration: cfg)
            w.setValue(false, forKey: "drawsBackground")
            return w
        }
        sidebarWeb = chrome(); toolbarWeb = chrome(); panelWeb = chrome(); paletteWeb = chrome()
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1280, height: 820),
                          styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView],
                          backing: .buffered, defer: false)
        super.init()
        chromeGuard.owner = self
        for w in [sidebarWeb, toolbarWeb, panelWeb, paletteWeb] { w.navigationDelegate = chromeGuard }

        window.titlebarAppearsTransparent = true
        window.titleVisibility = .hidden
        window.minSize = NSSize(width: 780, height: 500)
        window.isReleasedWhenClosed = false
        window.delegate = self
        window.contentView = root
        window.center()
        if !isPrivate { window.setFrameAutosaveName("SreonWindow") }
        if let last = Shell.shared.windows.last?.window { window.cascadeTopLeft(from: NSPoint(x: last.frame.minX + 24, y: last.frame.maxY - 24)) }

        root.wantsLayer = true
        sidebarFX.blendingMode = .behindWindow
        sidebarFX.state = .active
        panelFX.blendingMode = .behindWindow
        panelFX.state = .active
        hostView.wantsLayer = true
        hostView.layer?.cornerRadius = 12
        hostView.layer?.masksToBounds = true

        root.addSubview(sidebarFX)
        root.addSubview(panelFX)
        root.addSubview(hostView)
        root.addSubview(sidebarWeb)
        root.addSubview(toolbarWeb)
        root.addSubview(panelWeb)
        let drag1 = DragView(), drag2 = DragView()
        drag1.identifier = NSUserInterfaceItemIdentifier("dragSidebar")
        drag2.identifier = NSUserInterfaceItemIdentifier("dragToolbar")
        root.addSubview(drag1); root.addSubview(drag2)
        root.addSubview(paletteWeb)
        paletteWeb.isHidden = true
        panelWeb.isHidden = true
        panelFX.isHidden = true
        root.postsFrameChangedNotifications = true
        NotificationCenter.default.addObserver(self, selector: #selector(rootResized), name: NSView.frameDidChangeNotification, object: root)

        clickMonitor = NSEvent.addLocalMonitorForEvents(matching: [.leftMouseDown]) { [weak self] event in
            if event.window === self?.window { self?.lastClick = Date() }
            return event
        }
        sleepTimer = Timer.scheduledTimer(withTimeInterval: 60, repeats: true) { [weak self] _ in self?.sleepIdleTabs() }
        applyThemeColors()
        layoutAll()
    }

    deinit {
        if let m = clickMonitor { NSEvent.removeMonitor(m) }
        sleepTimer?.invalidate()
    }

    func owns(_ web: WKWebView) -> Bool {
        if web === sidebarWeb || web === toolbarWeb || web === panelWeb || web === paletteWeb { return true }
        return tabs.contains { $0.web === web }
    }

    func start(startURL: String?, restore: Bool) {
        loadChrome()
        let shell = Shell.shared
        let home = shell.str("home", "sreon://newtab")
        if let u = startURL {
            _ = newTab(url: u, select: true)
        } else if restore && !isPrivate && restoreSession() {
            // restored
        } else if shell.str("startup", "newtab") == "home" {
            _ = newTab(url: home.isEmpty ? "sreon://newtab" : home, select: true)
        } else {
            _ = newTab(url: "sreon://newtab", select: true)
        }
        scheduleSync()
    }

    func loadChrome() {
        sidebarWeb.load(URLRequest(url: URL(string: "sreon://chrome/sidebar")!))
        toolbarPad = needsToolbarPad()
        toolbarWeb.load(URLRequest(url: URL(string: toolbarPad ? "sreon://chrome/toolbar?pad=1" : "sreon://chrome/toolbar")!))
    }

    func needsToolbarPad() -> Bool {
        let s = Shell.shared
        return s.str("side_mode", "expanded") == "hidden" || s.str("side", "left") == "right"
    }

    // ---------------------------------------------------------------- layout
    @objc func rootResized() { layoutAll() }

    func windowDidResize(_ notification: Notification) { layoutAll() }

    func applyThemeColors() {
        let css = Shell.shared.css
        let bg = color(from: css, name: "bg", fallback: NSColor(calibratedWhite: 0.05, alpha: 1))
        let bg2 = color(from: css, name: "bg2", fallback: NSColor(calibratedWhite: 0.07, alpha: 1))
        window.backgroundColor = bg
        root.layer?.backgroundColor = bg2.cgColor
        let glass = Shell.shared.flag("glass", true)
        sidebarFX.material = .sidebar
        panelFX.material = .sidebar
        sidebarFX.isHidden = !glass
        panelFX.isHidden = !glass || !panelShown
        hostView.layer?.backgroundColor = bg.cgColor
    }

    func layoutAll() {
        let s = Shell.shared
        let b = root.bounds
        let mode = s.str("side_mode", "expanded")
        let right = s.str("side", "left") == "right"
        var sw: CGFloat = 0
        if mode == "expanded" { sw = CGFloat(max(200, min(380, s.int("side_width", 264)))) }
        if mode == "compact" { sw = 68 }
        let pw: CGFloat = panelShown ? CGFloat(max(280, min(520, s.int("panel_width", 340)))) : 0
        let toolbarH: CGFloat = 48
        let contentW = max(200, b.width - sw - pw)
        let x0: CGFloat = right ? 0 : sw

        sidebarFX.frame = NSRect(x: right ? b.width - sw : 0, y: 0, width: sw, height: b.height)
        sidebarWeb.frame = sidebarFX.frame
        sidebarFX.isHidden = sw == 0 || !s.flag("glass", true)
        sidebarWeb.isHidden = sw == 0

        let panelX: CGFloat = right ? b.width - sw - pw : b.width - pw
        panelFX.frame = NSRect(x: panelX, y: 0, width: pw, height: b.height)
        panelWeb.frame = panelFX.frame
        panelWeb.isHidden = !panelShown
        panelFX.isHidden = !panelShown || !s.flag("glass", true)

        toolbarWeb.frame = NSRect(x: x0, y: b.height - toolbarH, width: contentW, height: toolbarH)
        hostView.frame = NSRect(x: x0 + (right ? 8 : 0), y: 8, width: contentW - 8, height: b.height - toolbarH - 8)
        paletteWeb.frame = b

        for v in root.subviews {
            if v.identifier?.rawValue == "dragSidebar" { v.frame = NSRect(x: sidebarFX.frame.minX, y: b.height - 34, width: sw, height: 34) }
            if v.identifier?.rawValue == "dragToolbar" { v.frame = NSRect(x: x0, y: b.height - 6, width: contentW, height: 6) }
        }
        layoutTabs()
        if let f = findBar {
            f.frame.origin = NSPoint(x: hostView.frame.maxX - f.frame.width - 14, y: hostView.frame.maxY - f.frame.height - 12)
        }
    }

    func layoutTabs() {
        let bounds = hostView.bounds
        let active = tab(activeID)
        let second = splitID.flatMap { tab($0) }
        for t in tabs {
            guard let w = t.web else { continue }
            if w.superview !== hostView { hostView.addSubview(w) }
            if t === active {
                w.isHidden = false
                w.frame = second == nil ? bounds : NSRect(x: 0, y: 0, width: bounds.width / 2 - 1, height: bounds.height)
            } else if t === second {
                w.isHidden = false
                w.frame = NSRect(x: bounds.width / 2 + 1, y: 0, width: bounds.width / 2 - 1, height: bounds.height)
            } else {
                w.isHidden = true
            }
        }
    }

    // ------------------------------------------------------------------ tabs
    func tab(_ id: Int) -> Tab? { return tabs.first { $0.id == id } }
    var current: Tab? { return tab(activeID) }

    @discardableResult
    func newTab(url: String, select: Bool, background: Bool = false, configuration: WKWebViewConfiguration? = nil) -> Tab {
        let t = Tab(id: nextTabID, url: url, ws: wsName, isPrivate: isPrivate)
        nextTabID += 1
        let s = Shell.shared
        var index = tabs.count
        if s.flag("new_tab_next", true), let i = tabs.firstIndex(where: { $0.id == activeID }) { index = i + 1 }
        tabs.insert(t, at: index)
        makeWeb(for: t, configuration: configuration)
        if select && !background { selectTab(t.id) }
        scheduleSync()
        return t
    }

    @discardableResult
    func makeWeb(for t: Tab, configuration: WKWebViewConfiguration? = nil) -> WKWebView {
        let cfg: WKWebViewConfiguration
        if let c = configuration { cfg = c } else {
            cfg = WKWebViewConfiguration()
            cfg.websiteDataStore = profile.store
            cfg.userContentController = profile.ucc
            cfg.setURLSchemeHandler(Shell.shared.schemes, forURLScheme: "sreon")
            cfg.mediaTypesRequiringUserActionForPlayback = Shell.shared.flag("block_autoplay", true) ? .all : []
            cfg.preferences.javaScriptCanOpenWindowsAutomatically = false
        }
        let w = WKWebView(frame: hostView.bounds, configuration: cfg)
        w.navigationDelegate = self
        w.uiDelegate = self
        w.allowsBackForwardNavigationGestures = true
        w.allowsMagnification = true
        if #available(macOS 13.3, *) { w.isInspectable = Shell.shared.flag("inspect", true) }
        let ua = Shell.shared.str("ua", "")
        if !ua.isEmpty { w.customUserAgent = ua }
        t.web = w
        t.sleeping = false
        observe(w, for: t)
        hostView.addSubview(w)
        w.isHidden = true
        if configuration == nil, !t.url.isEmpty, let u = URL(string: t.url) { w.load(URLRequest(url: u)) }
        return w
    }

    func observe(_ w: WKWebView, for t: Tab) {
        t.observers.removeAll()
        t.observers.append(w.observe(\.title, options: [.new]) { [weak self, weak t] web, _ in
            guard let t = t else { return }
            t.title = web.title ?? t.title
            self?.scheduleSync()
        })
        t.observers.append(w.observe(\.url, options: [.new]) { [weak self, weak t] web, _ in
            guard let t = t, let u = web.url?.absoluteString else { return }
            t.url = u
            self?.scheduleSync()
            self?.scheduleSessionSave()
        })
        t.observers.append(w.observe(\.isLoading, options: [.new]) { [weak self, weak t] web, _ in
            t?.loading = web.isLoading
            self?.scheduleSync()
        })
        t.observers.append(w.observe(\.estimatedProgress, options: [.new]) { [weak self, weak t] web, _ in
            guard let t = t else { return }
            if abs(web.estimatedProgress - t.progress) >= 0.12 || web.estimatedProgress >= 1 {
                t.progress = web.estimatedProgress
                self?.scheduleSync()
            }
        })
        t.observers.append(w.observe(\.canGoBack, options: [.new]) { [weak self, weak t] web, _ in t?.canBack = web.canGoBack; self?.scheduleSync() })
        t.observers.append(w.observe(\.canGoForward, options: [.new]) { [weak self, weak t] web, _ in t?.canForward = web.canGoForward; self?.scheduleSync() })
        t.observers.append(w.observe(\.hasOnlySecureContent, options: [.new]) { [weak self, weak t] web, _ in t?.secure = web.hasOnlySecureContent; self?.scheduleSync() })
    }

    func selectTab(_ id: Int) {
        guard let t = tab(id) else { return }
        if t.ws != wsName { wsName = t.ws }
        if t.web == nil { makeWeb(for: t) }
        activeID = id
        t.lastActive = Date()
        layoutTabs()
        window.makeFirstResponder(t.web)
        scheduleSync()
        if panelShown { refreshChrome(only: [panelWeb]) }
        updateWindowTitle()
        scheduleSessionSave()
    }

    func updateWindowTitle() {
        if Shell.shared.flag("rtl", true) { window.title = current?.title.isEmpty == false ? current!.title : "Sreon" } else { window.title = "Sreon" }
    }

    func closeTab(_ id: Int) {
        guard let idx = tabs.firstIndex(where: { $0.id == id }) else { return }
        let t = tabs[idx]
        if !t.isPrivate && !t.url.isEmpty && !t.url.hasPrefix("sreon://newtab") {
            Shell.shared.core.send(["op": "closed", "url": t.url]) { _ in }
        }
        t.observers.removeAll()
        t.web?.stopLoading()
        t.web?.navigationDelegate = nil
        t.web?.uiDelegate = nil
        t.web?.removeFromSuperview()
        t.web = nil
        tabs.remove(at: idx)
        if splitID == id { splitID = nil }
        let inWS = tabs.filter { $0.ws == wsName }
        if activeID == id {
            if !inWS.isEmpty {
                selectTab(inWS[min(idx, inWS.count - 1)].id)
            } else if tabs.isEmpty && Shell.shared.windows.count > 1 {
                window.close()
                return
            } else {
                newTab(url: "sreon://newtab", select: true)
            }
        }
        scheduleSync()
        scheduleSessionSave()
    }

    func sleepTab(_ id: Int) {
        guard let t = tab(id), let w = t.web, id != activeID, id != splitID, !t.sleeping else { return }
        t.url = w.url?.absoluteString ?? t.url
        t.observers.removeAll()
        w.stopLoading()
        w.navigationDelegate = nil
        w.uiDelegate = nil
        w.removeFromSuperview()
        t.web = nil
        t.sleeping = true
        t.loading = false
        Shell.shared.core.send(["op": "count", "what": "sleeps"]) { _ in }
        scheduleSync()
    }

    func sleepIdleTabs() {
        let s = Shell.shared
        if !s.flag("sleep", true) { return }
        let limit = TimeInterval(max(2, s.int("sleep_min", 15)) * 60)
        for t in tabs where t.web != nil && t.id != activeID && t.id != splitID && !t.loading {
            if Date().timeIntervalSince(t.lastActive) > limit { sleepTab(t.id) }
        }
    }

    // ----------------------------------------------------------------- open
    func open(_ url: String, where target: String) {
        guard URL(string: url) != nil else { return }
        if target == "here", let t = current {
            if t.web == nil { makeWeb(for: t) }
            t.url = url
            if let u = URL(string: url) { t.web?.load(URLRequest(url: u)) }
            t.title = ""
            layoutTabs()
            window.makeFirstResponder(t.web)
            scheduleSync()
        } else if target == "bg" {
            newTab(url: url, select: false, background: true)
        } else {
            newTab(url: url, select: true)
        }
    }

    func load(_ url: String, in web: WKWebView) {
        if let u = URL(string: url) { web.load(URLRequest(url: u)) }
    }

    // ----------------------------------------------------------- engine sync
    func scheduleSync() {
        if syncScheduled { return }
        syncScheduled = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.06) { [weak self] in
            guard let self = self else { return }
            self.syncScheduled = false
            self.syncNow()
        }
    }

    func syncNow() {
        let payload: [String: Any] = ["op": "tabs", "tabs": tabs.map { $0.dictionary }, "active": activeID, "ws": wsName]
        Shell.shared.core.send(payload) { [weak self] _ in self?.refreshChrome(only: nil) }
        updateWindowTitle()
    }

    func refreshChrome(only list: [ChromeWeb]?) {
        let targets: [ChromeWeb] = list ?? (panelShown ? [sidebarWeb, toolbarWeb, panelWeb] : [sidebarWeb, toolbarWeb])
        for w in targets {
            let arg = (w === toolbarWeb && needsToolbarPad()) ? "{pad:true}" : "{}"
            w.evaluateJavaScript("window.sreon&&sreon.act('refresh',\(arg))", completionHandler: nil)
        }
    }

    func refreshEverything() {
        loadChrome()
        for t in tabs where t.url.hasPrefix("sreon://") { t.web?.reload() }
        scheduleSync()
    }

    /// Replaces the colours in every Sreon page without reloading.
    func pushTheme() {
        let js = "var t=document.getElementById('theme');if(t){t.textContent=\(jsLiteral(Shell.shared.css));}"
        for w in [sidebarWeb, toolbarWeb, panelWeb, paletteWeb] { w.evaluateJavaScript(js, completionHandler: nil) }
        for t in tabs where t.url.hasPrefix("sreon://") { t.web?.evaluateJavaScript(js, completionHandler: nil) }
    }

    func windowDidBecomeKey(_ notification: Notification) { scheduleSync() }

    func windowWillClose(_ notification: Notification) {
        saveSessionNow()
        for t in tabs { t.observers.removeAll(); t.web?.navigationDelegate = nil; t.web?.uiDelegate = nil; t.web?.stopLoading() }
        Shell.shared.remove(self)
    }

    // -------------------------------------------------------------- bridge
    func handleBridge(op: String, view: String, data: [String: Any], from web: WKWebView, reply: @escaping (Any?, String?) -> Void) {
        let shell = Shell.shared
        switch op {
        case "act":
            let name = data["name"] as? String ?? ""
            var target = shell.core
            var request: [String: Any] = ["op": "act", "view": view, "data": data]
            if name.hasPrefix("st.exec.") {
                shell.startWorkerIfNeeded()
                if shell.worker.alive { target = shell.worker } else { request["view"] = view }
            }
            target.send(request) { [weak self] r in
                guard let self = self else { reply([:], nil); return }
                self.finish(r, view: view, reply: reply)
            }
        case "ctx":
            shell.core.send(["op": "ctx", "view": view, "data": data]) { [weak self] r in
                let items = r["menu"] as? [[String: Any]] ?? []
                if !items.isEmpty {
                    let x = (data["x"] as? NSNumber)?.doubleValue ?? 0
                    let y = (data["y"] as? NSNumber)?.doubleValue ?? 0
                    self?.popMenu(items, in: web, x: CGFloat(x), y: CGFloat(y))
                }
                reply([:], nil)
            }
        case "ready":
            shell.core.send(["op": "ready"]) { r in
                var out = r
                out.removeValue(forKey: "id")
                reply(out, nil)
            }
        default:
            reply([:], nil)
        }
    }

    func finish(_ r: [String: Any], view: String, reply: @escaping (Any?, String?) -> Void) {
        var out = r
        out.removeValue(forKey: "id")
        let host = out.removeValue(forKey: "host") as? [[String: Any]] ?? []
        if let text = out["toast"] as? String, ["sidebar", "toolbar", "panel", "palette"].contains(view) { toast.show(text, in: root) }
        reply(out, nil)
        for action in host { perform(action) }
    }

    func popMenu(_ items: [[String: Any]], in web: WKWebView, x: CGFloat, y: CGFloat) {
        let menu = NSMenu()
        for it in items {
            if (it["sep"] as? Bool) == true { menu.addItem(NSMenuItem.separator()); continue }
            let mi = NSMenuItem(title: it["title"] as? String ?? "", action: #selector(menuPicked(_:)), keyEquivalent: "")
            mi.target = self
            mi.representedObject = ["act": it["act"] as? String ?? "", "args": it["args"] as? [String: Any] ?? [:]] as [String: Any]
            menu.addItem(mi)
        }
        let point = NSPoint(x: x, y: web.isFlipped ? y : web.bounds.height - y)
        menu.popUp(positioning: nil, at: point, in: web)
    }

    @objc func menuPicked(_ sender: NSMenuItem) {
        guard let d = sender.representedObject as? [String: Any], let act = d["act"] as? String else { return }
        let args = d["args"] as? [String: Any] ?? [:]
        runAct(act, args)
    }

    /// Runs an engine act on behalf of the window (menus, shortcuts).
    func runAct(_ name: String, _ args: [String: Any], view: String = "window") {
        Shell.shared.core.send(["op": "act", "view": view, "data": ["name": name, "args": args]]) { [weak self] r in
            guard let self = self else { return }
            if let text = r["toast"] as? String { self.toast.show(text, in: self.root) }
            for action in (r["host"] as? [[String: Any]] ?? []) { self.perform(action) }
        }
    }

    func runCommand(_ id: String) { runAct("cmd", ["id": id]) }

    // -------------------------------------------------------- host actions
    func perform(_ a: [String: Any]) {
        let what = a["do"] as? String ?? ""
        let s = Shell.shared
        switch what {
        case "open":
            open(a["url"] as? String ?? "", where: a["where"] as? String ?? "tab")
        case "select":
            if let id = (a["id"] as? NSNumber)?.intValue { selectTab(id) }
        case "close":
            closeTab((a["id"] as? NSNumber)?.intValue ?? activeID)
        case "pin":
            if let t = tab((a["id"] as? NSNumber)?.intValue ?? activeID) { t.pinned.toggle(); scheduleSync() }
        case "sleep":
            sleepTab((a["id"] as? NSNumber)?.intValue ?? activeID)
        case "sleep_others":
            for t in tabs where t.id != activeID { sleepTab(t.id) }
        case "close_others":
            let keep = (a["id"] as? NSNumber)?.intValue ?? activeID
            for t in tabs where t.id != keep && !t.pinned && t.ws == wsName { closeTab(t.id) }
            selectTab(keep)
        case "move_ws":
            if let t = tab((a["id"] as? NSNumber)?.intValue ?? 0), let ws = a["ws"] as? String {
                t.ws = ws
                if t.id == activeID && ws != wsName { switchWorkspace(wsName) }
                scheduleSync()
            }
        case "workspace":
            switchWorkspace(a["name"] as? String ?? wsName)
        case "go":
            doGo(a["what"] as? String ?? "")
        case "sidebar", "layout":
            s.fetchSettings { [weak self] in self?.relayoutFromSettings() }
        case "panel":
            panelShown = (a["show"] as? Bool) ?? !panelShown
            layoutAll()
            applyThemeColors()
            if panelShown { panelWeb.load(URLRequest(url: URL(string: "sreon://chrome/panel")!)) }
        case "palette":
            showPalette((a["show"] as? Bool) ?? false, fill: a["fill"] as? String ?? "")
        case "rules":
            s.installRules { [weak self] in
                if let t = self?.current, t.url.hasPrefix("http") { t.web?.reload() }
            }
        case "webconfig":
            s.loadScripts { [weak self] in
                guard let self = self else { return }
                let ua = s.str("ua", "")
                for t in self.tabs { t.web?.customUserAgent = ua.isEmpty ? nil : ua }
            }
        case "theme", "appearance":
            s.fetchSettings {
                s.refreshTheme { [weak self] in
                    for w in s.windows { w.pushTheme() }
                    self?.relayoutFromSettings()
                }
            }
        case "menu":
            s.rebuildMenu()
        case "refresh":
            refreshChrome(only: nil)
        case "extract":
            extractPage(then: a["then"] as? String ?? "brief")
        case "copy":
            NSPasteboard.general.clearContents()
            NSPasteboard.general.setString(a["text"] as? String ?? "", forType: .string)
        case "find":
            showFind()
        case "zoom":
            if let level = (a["level"] as? NSNumber)?.doubleValue { current?.web?.pageZoom = CGFloat(level) }
        case "split":
            toggleSplit()
        case "screenshot":
            takeScreenshot()
        case "pdf":
            savePDF()
        case "print":
            printPage()
        case "window":
            s.newWindow(isPrivate: (a["private"] as? Bool) ?? false)
        case "clear_data":
            let types = WKWebsiteDataStore.allWebsiteDataTypes()
            profile.store.removeData(ofTypes: types, modifiedSince: Date.distantPast) { [weak self] in
                guard let self = self else { return }
                self.toast.show("Browsing data cleared", in: self.root)
            }
        case "fullscreen":
            window.toggleFullScreen(nil)
        case "notify":
            notify(title: a["title"] as? String ?? "Sreon", body: a["body"] as? String ?? "")
        case "reveal":
            if let p = a["path"] as? String { NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: p)]) }
        case "paste_settings":
            let text = NSPasteboard.general.string(forType: .string) ?? ""
            s.core.send(["op": "paste_settings", "text": text]) { [weak self] r in
                guard let self = self else { return }
                if (r["ok"] as? Bool) == true {
                    s.fetchSettings {
                        s.refreshTheme {
                            s.loadScripts { s.installRules { for w in s.windows { w.pushTheme(); w.relayoutFromSettings() } } }
                        }
                    }
                    self.toast.show("Settings applied", in: self.root)
                } else {
                    self.toast.show("The clipboard doesn't hold Sreon settings", in: self.root)
                }
            }
        default:
            break
        }
    }

    func relayoutFromSettings() {
        applyThemeColors()
        layoutAll()
        let pad = needsToolbarPad()
        if pad != toolbarPad { toolbarPad = pad }
        refreshChrome(only: nil)
    }

    func switchWorkspace(_ name: String) {
        wsName = name
        if let t = current, t.ws == name { scheduleSync(); return }
        if let first = tabs.first(where: { $0.ws == name }) { selectTab(first.id) }
        else { newTab(url: "sreon://newtab", select: true) }
    }

    func doGo(_ what: String) {
        guard let w = current?.web else { return }
        switch what {
        case "back": w.goBack()
        case "forward": w.goForward()
        case "reload": if w.url == nil, let t = current, let u = URL(string: t.url) { w.load(URLRequest(url: u)) } else { w.reload() }
        case "stop": w.stopLoading()
        case "reload_hard": w.reloadFromOrigin()
        default: break
        }
    }

    func showPalette(_ show: Bool, fill: String) {
        paletteShown = show
        paletteWeb.isHidden = !show
        if show {
            var url = "sreon://chrome/palette"
            if !fill.isEmpty, let q = fill.addingPercentEncoding(withAllowedCharacters: .urlQueryAllowed) { url += "?fill=" + q }
            paletteWeb.load(URLRequest(url: URL(string: url)!))
            window.makeFirstResponder(paletteWeb)
        } else {
            window.makeFirstResponder(current?.web)
        }
    }

    func toggleSplit() {
        if splitID != nil { splitID = nil; layoutTabs(); return }
        let other = tabs.filter { $0.id != activeID && $0.ws == wsName }.sorted { $0.lastActive > $1.lastActive }.first
        if let o = other {
            if o.web == nil { makeWeb(for: o) }
            splitID = o.id
        } else {
            let t = newTab(url: "sreon://newtab", select: false, background: true)
            splitID = t.id
        }
        layoutTabs()
    }

    // --------------------------------------------------------- page actions
    func extractPage(then kind: String) {
        guard let w = current?.web, let js = Shell.shared.scripts["extract_js"] as? String else { return }
        w.evaluateJavaScript(js) { [weak self] result, _ in
            guard let self = self else { return }
            let text = result as? String ?? ""
            Shell.shared.core.send(["op": "digest", "kind": kind, "data": text]) { r in
                if let u = r["open"] as? String { self.open(u, where: "tab") }
                if let t = r["toast"] as? String { self.toast.show(t, in: self.root) }
                if let apply = r["apply"] as? [String: Any], (apply["view"] as? String) == "panel" {
                    let json = jsonString(apply)
                    self.panelWeb.evaluateJavaScript("window.sreon&&sreon.apply(\(json))", completionHandler: nil)
                }
            }
        }
    }

    func takeScreenshot() {
        guard let w = current?.web else { return }
        w.takeSnapshot(with: nil) { [weak self] image, _ in
            guard let self = self, let image = image,
                  let tiff = image.tiffRepresentation, let rep = NSBitmapImageRep(data: tiff),
                  let png = rep.representation(using: .png, properties: [:]) else { return }
            let f = DateFormatter(); f.dateFormat = "yyyy-MM-dd 'at' HH.mm.ss"
            let dir = FileManager.default.urls(for: .downloadsDirectory, in: .userDomainMask).first ?? URL(fileURLWithPath: NSHomeDirectory())
            let file = dir.appendingPathComponent("Sreon Screenshot \(f.string(from: Date())).png")
            try? png.write(to: file)
            NSPasteboard.general.clearContents()
            NSPasteboard.general.writeObjects([image])
            self.toast.show("Screenshot saved to Downloads and copied", in: self.root)
        }
    }

    func savePDF() {
        guard let w = current?.web else { return }
        w.createPDF { [weak self] result in
            guard let self = self else { return }
            if case .success(let data) = result {
                let f = DateFormatter(); f.dateFormat = "yyyy-MM-dd 'at' HH.mm.ss"
                let dir = FileManager.default.urls(for: .downloadsDirectory, in: .userDomainMask).first ?? URL(fileURLWithPath: NSHomeDirectory())
                let file = dir.appendingPathComponent("Sreon Page \(f.string(from: Date())).pdf")
                try? data.write(to: file)
                self.toast.show("PDF saved to Downloads", in: self.root)
            }
        }
    }

    func printPage() {
        guard let w = current?.web else { return }
        let op = w.printOperation(with: NSPrintInfo.shared)
        op.view?.frame = w.bounds
        op.runModal(for: window, delegate: nil, didRun: nil, contextInfo: nil)
    }

    func showFind() {
        if findBar == nil {
            let f = FindBar()
            f.onFind = { [weak self] text, backwards in
                guard let w = self?.current?.web, !text.isEmpty else { return }
                let cfg = WKFindConfiguration()
                cfg.backwards = backwards
                cfg.wraps = true
                cfg.caseSensitive = false
                w.find(text, configuration: cfg) { _ in }
            }
            f.onClose = { [weak self] in self?.hideFind() }
            findBar = f
            root.addSubview(f)
        }
        layoutAll()
        findBar?.isHidden = false
        window.makeFirstResponder(findBar?.field)
    }

    func hideFind() {
        findBar?.isHidden = true
        window.makeFirstResponder(current?.web)
    }

    func notify(title: String, body: String) {
        let center = UNUserNotificationCenter.current()
        center.requestAuthorization(options: [.alert, .sound]) { granted, _ in
            if !granted { DispatchQueue.main.async { NSSound.beep() }; return }
            let c = UNMutableNotificationContent()
            c.title = title
            c.body = body
            c.sound = .default
            center.add(UNNotificationRequest(identifier: UUID().uuidString, content: c, trigger: nil))
        }
    }

    // ------------------------------------------------------------- session
    func scheduleSessionSave() {
        if isPrivate { return }
        saveWork?.cancel()
        let work = DispatchWorkItem { [weak self] in self?.saveSessionNow() }
        saveWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5, execute: work)
    }

    var sessionFile: URL {
        let base = ProcessInfo.processInfo.environment["SREON_HOME"].map { URL(fileURLWithPath: $0) }
            ?? URL(fileURLWithPath: NSHomeDirectory()).appendingPathComponent(".sreon")
        return base.appendingPathComponent("session.json")
    }

    func saveSessionNow() {
        if isPrivate || Shell.shared.windows.first !== self { return }
        let list = tabs.filter { !$0.url.isEmpty && !$0.url.hasPrefix("sreon://error") }.map { t -> [String: Any] in
            return ["url": t.url, "title": t.title, "pinned": t.pinned, "ws": t.ws]
        }
        let active = tabs.firstIndex { $0.id == activeID } ?? 0
        let obj: [String: Any] = ["tabs": list, "active": active, "ws": wsName]
        try? FileManager.default.createDirectory(at: sessionFile.deletingLastPathComponent(), withIntermediateDirectories: true)
        if let data = try? JSONSerialization.data(withJSONObject: obj, options: []) { try? data.write(to: sessionFile) }
    }

    func restoreSession() -> Bool {
        guard let data = try? Data(contentsOf: sessionFile),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let list = obj["tabs"] as? [[String: Any]], !list.isEmpty else { return false }
        wsName = obj["ws"] as? String ?? "Personal"
        var made: [Tab] = []
        for item in list {
            guard let url = item["url"] as? String else { continue }
            let t = Tab(id: nextTabID, url: url, ws: item["ws"] as? String ?? "Personal", isPrivate: false)
            nextTabID += 1
            t.title = item["title"] as? String ?? ""
            t.pinned = (item["pinned"] as? Bool) ?? false
            t.sleeping = true
            tabs.append(t)
            made.append(t)
        }
        if made.isEmpty { return false }
        let idx = max(0, min(made.count - 1, (obj["active"] as? Int) ?? 0))
        selectTab(made[idx].id)
        return true
    }

    // ----------------------------------------------------------- navigation
    func webView(_ webView: WKWebView, decidePolicyFor navigationAction: WKNavigationAction, preferences: WKWebpagePreferences, decisionHandler: @escaping (WKNavigationActionPolicy, WKWebpagePreferences) -> Void) {
        let s = Shell.shared
        preferences.allowsContentJavaScript = s.flag("js", true) || (navigationAction.request.url?.scheme == "sreon")
        guard let url = navigationAction.request.url else { decisionHandler(.cancel, preferences); return }
        let scheme = url.scheme?.lowercased() ?? ""
        if scheme == "sreon" || scheme == "about" || scheme == "blob" || scheme == "data" { decisionHandler(.allow, preferences); return }
        if scheme != "http" && scheme != "https" {
            if scheme == "mailto" || scheme == "tel" || scheme == "facetime" || scheme == "sms" { NSWorkspace.shared.open(url) }
            decisionHandler(.cancel, preferences)
            return
        }
        if navigationAction.shouldPerformDownload { decisionHandler(.download, preferences); return }
        if navigationAction.targetFrame?.isMainFrame == false { decisionHandler(.allow, preferences); return }
        s.core.send(["op": "nav", "url": url.absoluteString]) { [weak self] r in
            if (r["allow"] as? Bool) == false {
                decisionHandler(.cancel, preferences)
                if let next = r["load"] as? String { self?.load(next, in: webView) }
            } else {
                decisionHandler(.allow, preferences)
            }
        }
    }

    func webView(_ webView: WKWebView, decidePolicyFor navigationResponse: WKNavigationResponse, decisionHandler: @escaping (WKNavigationResponsePolicy) -> Void) {
        if let http = navigationResponse.response as? HTTPURLResponse,
           let disposition = http.value(forHTTPHeaderField: "Content-Disposition"), disposition.lowercased().hasPrefix("attachment") {
            decisionHandler(.download)
            return
        }
        decisionHandler(navigationResponse.canShowMIMEType ? .allow : .download)
    }

    func webView(_ webView: WKWebView, navigationAction: WKNavigationAction, didBecome download: WKDownload) { download.delegate = self; downloadFrom[ObjectIdentifier(download)] = webView.url?.absoluteString ?? "" }
    func webView(_ webView: WKWebView, navigationResponse: WKNavigationResponse, didBecome download: WKDownload) { download.delegate = self; downloadFrom[ObjectIdentifier(download)] = webView.url?.absoluteString ?? "" }

    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        guard let t = tabs.first(where: { $0.web === webView }) else { return }
        t.loading = false
        t.title = webView.title ?? t.title
        let url = webView.url?.absoluteString ?? t.url
        t.url = url
        if url.hasPrefix("http") {
            if !isPrivate { Shell.shared.core.send(["op": "visit", "url": url, "title": t.title]) { _ in } }
            Shell.shared.core.send(["op": "zoomfor", "url": url]) { r in
                if let level = (r["level"] as? NSNumber)?.doubleValue, abs(level - 1.0) > 0.001 { webView.pageZoom = CGFloat(level) }
            }
        }
        scheduleSync()
        if t.id == activeID && panelShown { refreshChrome(only: [panelWeb]) }
    }

    func webView(_ webView: WKWebView, didFailProvisionalNavigation navigation: WKNavigation!, withError error: Error) { navigationFailed(webView, error) }
    func webView(_ webView: WKWebView, didFail navigation: WKNavigation!, withError error: Error) { navigationFailed(webView, error) }

    func navigationFailed(_ webView: WKWebView, _ error: Error) {
        let ns = error as NSError
        if ns.code == NSURLErrorCancelled || ns.domain == "WebKitErrorDomain" { return }
        guard let t = tabs.first(where: { $0.web === webView }) else { return }
        t.loading = false
        let failing = (ns.userInfo[NSURLErrorFailingURLStringErrorKey] as? String) ?? t.url
        if failing.hasPrefix("sreon://") { return }
        let u = failing.addingPercentEncoding(withAllowedCharacters: .urlQueryAllowed) ?? ""
        let r = ns.localizedDescription.addingPercentEncoding(withAllowedCharacters: .urlQueryAllowed) ?? ""
        load("sreon://error?u=\(u)&r=\(r)", in: webView)
    }

    func webViewWebContentProcessDidTerminate(_ webView: WKWebView) { webView.reload() }

    // -------------------------------------------------------------- UI hooks
    func webView(_ webView: WKWebView, createWebViewWith configuration: WKWebViewConfiguration, for navigationAction: WKNavigationAction, windowFeatures: WKWindowFeatures) -> WKWebView? {
        let recent = Date().timeIntervalSince(lastClick) < 1.6
        let blockPopups = Shell.shared.flag("popups", true)
        if navigationAction.navigationType == .linkActivated {
            if let u = navigationAction.request.url?.absoluteString { open(u, where: "tab") }
            return nil
        }
        if blockPopups && !recent {
            Shell.shared.core.send(["op": "count", "what": "popups"]) { _ in }
            toast.show("Blocked a pop-up window", in: root)
            return nil
        }
        let t = newTab(url: "", select: true, configuration: configuration)
        return t.web
    }

    func webViewDidClose(_ webView: WKWebView) {
        if let t = tabs.first(where: { $0.web === webView }) { closeTab(t.id) }
    }

    func webView(_ webView: WKWebView, runJavaScriptAlertPanelWithMessage message: String, initiatedByFrame frame: WKFrameInfo, completionHandler: @escaping () -> Void) {
        let a = NSAlert()
        a.messageText = frame.request.url?.host ?? "Sreon"
        a.informativeText = message
        a.addButton(withTitle: "OK")
        a.beginSheetModal(for: window) { _ in completionHandler() }
    }

    func webView(_ webView: WKWebView, runJavaScriptConfirmPanelWithMessage message: String, initiatedByFrame frame: WKFrameInfo, completionHandler: @escaping (Bool) -> Void) {
        let a = NSAlert()
        a.messageText = frame.request.url?.host ?? "Sreon"
        a.informativeText = message
        a.addButton(withTitle: "OK")
        a.addButton(withTitle: "Cancel")
        a.beginSheetModal(for: window) { r in completionHandler(r == .alertFirstButtonReturn) }
    }

    func webView(_ webView: WKWebView, runJavaScriptTextInputPanelWithPrompt prompt: String, defaultText: String?, initiatedByFrame frame: WKFrameInfo, completionHandler: @escaping (String?) -> Void) {
        let a = NSAlert()
        a.messageText = frame.request.url?.host ?? "Sreon"
        a.informativeText = prompt
        let field = NSTextField(frame: NSRect(x: 0, y: 0, width: 280, height: 24))
        field.stringValue = defaultText ?? ""
        a.accessoryView = field
        a.addButton(withTitle: "OK")
        a.addButton(withTitle: "Cancel")
        a.beginSheetModal(for: window) { r in completionHandler(r == .alertFirstButtonReturn ? field.stringValue : nil) }
    }

    func webView(_ webView: WKWebView, runOpenPanelWith parameters: WKOpenPanelParameters, initiatedByFrame frame: WKFrameInfo, completionHandler: @escaping ([URL]?) -> Void) {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = parameters.allowsMultipleSelection
        panel.canChooseDirectories = parameters.allowsDirectories
        panel.canChooseFiles = true
        panel.beginSheetModal(for: window) { r in completionHandler(r == .OK ? panel.urls : nil) }
    }

    // ------------------------------------------------------------ downloads
    func download(_ download: WKDownload, decideDestinationUsing response: URLResponse, suggestedFilename: String, completionHandler: @escaping (URL?) -> Void) {
        let dir = FileManager.default.urls(for: .downloadsDirectory, in: .userDomainMask).first ?? URL(fileURLWithPath: NSHomeDirectory())
        var name = suggestedFilename.isEmpty ? "download" : suggestedFilename
        name = name.replacingOccurrences(of: "/", with: "-")
        var dest = dir.appendingPathComponent(name)
        var n = 1
        let base = (name as NSString).deletingPathExtension
        let ext = (name as NSString).pathExtension
        while FileManager.default.fileExists(atPath: dest.path) {
            let candidate = ext.isEmpty ? "\(base) \(n)" : "\(base) \(n).\(ext)"
            dest = dir.appendingPathComponent(candidate)
            n += 1
        }
        downloads[ObjectIdentifier(download)] = dest
        completionHandler(dest)
    }

    func downloadDidFinish(_ download: WKDownload) {
        let id = ObjectIdentifier(download)
        guard let dest = downloads.removeValue(forKey: id) else { return }
        let from = downloadFrom.removeValue(forKey: id) ?? ""
        Shell.shared.core.send(["op": "download", "name": dest.lastPathComponent, "path": dest.path, "from": from, "state": "done"]) { _ in }
        toast.show("Downloaded " + dest.lastPathComponent, in: root)
    }

    func download(_ download: WKDownload, didFailWithError error: Error, resumeData: Data?) {
        downloads.removeValue(forKey: ObjectIdentifier(download))
        downloadFrom.removeValue(forKey: ObjectIdentifier(download))
        toast.show("A download failed", in: root)
    }
}

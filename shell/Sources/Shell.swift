// Sreon — Shell.swift
// The app-wide pieces: the engine, ad-block rule lists, user scripts,
// the sreon:// scheme, and the bridge pages use to talk to the engine.
import Cocoa
import WebKit

final class Profile {
    let store: WKWebsiteDataStore
    let ucc = WKUserContentController()
    let isPrivate: Bool

    init(isPrivate: Bool) {
        self.isPrivate = isPrivate
        store = isPrivate ? WKWebsiteDataStore.nonPersistent() : WKWebsiteDataStore.default()
        ucc.addScriptMessageHandler(Bridge.shared, contentWorld: .page, name: "sreon")
    }
}

final class Shell: NSObject {
    static let shared = Shell()

    let core = Engine(role: "core")
    let worker = Engine(role: "worker")
    let schemes = SreonScheme()
    let normal = Profile(isPrivate: false)
    let chromeUCC = WKUserContentController()
    var windows: [BrowserWindow] = []
    var settings: [String: Any] = [:]
    var actions: [[String]] = []
    var css = ""
    var scripts: [String: Any] = [:]
    var ruleLists: [WKContentRuleList] = []
    var rulesOn = true
    var restarts = 0
    var workerStarted = false

    var resources: String {
        if let env = ProcessInfo.processInfo.environment["SREON_RES"], !env.isEmpty { return env }
        return Bundle.main.resourcePath ?? "."
    }
    var interpreter: String {
        if let env = ProcessInfo.processInfo.environment["SREON_SPRFST"], !env.isEmpty { return env }
        return resources + "/bin/sprfst"
    }
    var systemDark: Bool {
        return NSApp.effectiveAppearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
    }

    override init() {
        super.init()
        chromeUCC.addScriptMessageHandler(Bridge.shared, contentWorld: .page, name: "sreon")
    }

    // ---------------------------------------------------------- engine
    func startEngine() -> Bool {
        let ok = core.start(interpreter: interpreter, entry: resources + "/engine/main.spf", resources: resources)
        core.onExit = { [weak self] in self?.engineStopped() }
        return ok
    }

    func startWorkerIfNeeded() {
        if workerStarted && worker.alive { return }
        workerStarted = worker.start(interpreter: interpreter, entry: resources + "/engine/main.spf", resources: resources)
    }

    private func engineStopped() {
        restarts += 1
        if restarts > 3 {
            let alert = NSAlert()
            alert.messageText = "Sreon's engine stopped"
            alert.informativeText = "Quit and open Sreon again. If this keeps happening, delete ~/.sreon and try once more."
            alert.runModal()
            return
        }
        if startEngine() {
            boot { for w in self.windows { w.refreshEverything() } }
        }
    }

    func boot(_ done: @escaping () -> Void) {
        core.send(["op": "boot", "dark": systemDark]) { [weak self] r in
            guard let self = self else { return }
            self.settings = r["settings"] as? [String: Any] ?? [:]
            self.actions = (r["actions"] as? [[Any]] ?? []).map { $0.map { "\($0)" } }
            self.refreshTheme {
                self.loadScripts {
                    self.installRules(done)
                }
            }
        }
    }

    func fetchSettings(_ done: @escaping () -> Void) {
        core.send(["op": "settings"]) { [weak self] r in
            if let s = r["settings"] as? [String: Any] { self?.settings = s }
            done()
        }
    }

    func str(_ key: String, _ fallback: String = "") -> String {
        if let v = settings[key] as? String { return v }
        return fallback
    }
    func int(_ key: String, _ fallback: Int) -> Int {
        if let v = settings[key] as? NSNumber { return v.intValue }
        if let v = settings[key] as? String, let n = Int(v) { return n }
        return fallback
    }
    func flag(_ key: String, _ fallback: Bool = false) -> Bool {
        if let v = settings[key] as? Bool { return v }
        if let v = settings[key] as? NSNumber { return v.boolValue }
        return fallback
    }

    var allProfiles: [Profile] {
        return [normal] + windows.compactMap { $0.privateProfile }
    }

    // ----------------------------------------------------------- theme
    func refreshTheme(_ done: @escaping () -> Void) {
        core.send(["op": "theme", "dark": systemDark]) { [weak self] r in
            guard let self = self else { return }
            self.css = r["css"] as? String ?? ""
            let dark = (r["dark"] as? Bool) ?? true
            let themeSetting = self.str("theme", "midnight")
            for w in self.windows {
                w.window.appearance = themeSetting == "auto" ? nil : NSAppearance(named: dark ? .darkAqua : .aqua)
                w.applyThemeColors()
            }
            done()
        }
    }

    // --------------------------------------------------------- scripts
    func loadScripts(_ done: @escaping () -> Void) {
        core.send(["op": "scripts"]) { [weak self] r in
            guard let self = self else { return }
            self.scripts = r
            for p in self.allProfiles { self.applyScripts(to: p) }
            done()
        }
    }

    func applyScripts(to profile: Profile) {
        let ucc = profile.ucc
        ucc.removeAllUserScripts()
        let on = (scripts["shield_on"] as? Bool) ?? false
        if on {
            let config = scripts["config"] as? String ?? ""
            let js = scripts["shield_js"] as? String ?? ""
            let source = "(function(){if(location.protocol==='sreon:')return;" + config + "})();" + js
            ucc.addUserScript(WKUserScript(source: source, injectionTime: .atDocumentStart, forMainFrameOnly: false))
        }
        if (scripts["dark_pages"] as? Bool) ?? false {
            let cssText = scripts["dark_css"] as? String ?? ""
            let source = "(function(){if(location.protocol==='sreon:'||!document.documentElement)return;var s=document.createElement('style');s.id='sreon-dark';s.textContent=" + jsLiteral(cssText) + ";document.documentElement.appendChild(s);})();"
            ucc.addUserScript(WKUserScript(source: source, injectionTime: .atDocumentStart, forMainFrameOnly: true))
        }
    }

    // ------------------------------------------------------------ rules
    func installRules(_ done: @escaping () -> Void) {
        core.send(["op": "rules"]) { [weak self] r in
            guard let self = self else { return }
            self.rulesOn = (r["on"] as? Bool) ?? true
            let lists = r["lists"] as? [[String: Any]] ?? []
            var wanted: [(String, String)] = []
            for l in lists {
                if let id = l["id"] as? String, let body = l["body"] as? String { wanted.append((id, body)) }
            }
            self.compile(wanted, index: 0, collected: []) { compiled in
                self.ruleLists = compiled
                for p in self.allProfiles { self.applyRules(to: p) }
                self.removeStaleRules(keeping: wanted.map { $0.0 })
                done()
            }
        }
    }

    private func compile(_ wanted: [(String, String)], index: Int, collected: [WKContentRuleList], _ done: @escaping ([WKContentRuleList]) -> Void) {
        if index >= wanted.count { done(collected); return }
        let (id, body) = wanted[index]
        let store: WKContentRuleListStore? = WKContentRuleListStore.default()
        let next: (WKContentRuleList?) -> Void = { list in
            var more = collected
            if let list = list { more.append(list) }
            self.compile(wanted, index: index + 1, collected: more, done)
        }
        store?.lookUpContentRuleList(forIdentifier: id) { found, _ in
            if let found = found { DispatchQueue.main.async { next(found) }; return }
            store?.compileContentRuleList(forIdentifier: id, encodedContentRuleList: body) { list, error in
                if let error = error { NSLog("Sreon: rule list %@ skipped: %@", id, "\(error)") }
                DispatchQueue.main.async { next(list) }
            }
        }
    }

    func applyRules(to profile: Profile) {
        profile.ucc.removeAllContentRuleLists()
        if !rulesOn { return }
        for l in ruleLists { profile.ucc.add(l) }
    }

    private func removeStaleRules(keeping: [String]) {
        let store: WKContentRuleListStore? = WKContentRuleListStore.default()
        store?.getAvailableContentRuleListIdentifiers { ids in
            for id in ids ?? [] where id.hasPrefix("sreon-") && !keeping.contains(id) {
                store?.removeContentRuleList(forIdentifier: id) { _ in }
            }
        }
    }

    // ----------------------------------------------------------- windows
    @discardableResult
    func newWindow(isPrivate: Bool, startURL: String? = nil, restore: Bool = false, show: Bool = true) -> BrowserWindow {
        let w = BrowserWindow(isPrivate: isPrivate)
        windows.append(w)
        if isPrivate { applyScripts(to: w.profile); applyRules(to: w.profile) }
        w.start(startURL: startURL, restore: restore)
        if show { w.window.makeKeyAndOrderFront(nil) }
        return w
    }

    func window(owning web: WKWebView) -> BrowserWindow? {
        return windows.first { $0.owns(web) }
    }

    func remove(_ w: BrowserWindow) {
        windows.removeAll { $0 === w }
    }

    func rebuildMenu() {
        core.send(["op": "boot", "dark": systemDark]) { [weak self] r in
            guard let self = self else { return }
            self.actions = (r["actions"] as? [[Any]] ?? []).map { $0.map { "\($0)" } }
            MenuBuilder.build()
        }
    }
}

// ============================================================== scheme
final class SreonScheme: NSObject, WKURLSchemeHandler {
    private var cache: [String: (Data, String)] = [:]
    private var stopped = Set<ObjectIdentifier>()

    func webView(_ webView: WKWebView, start urlSchemeTask: WKURLSchemeTask) {
        guard let url = urlSchemeTask.request.url else { return }
        stopped.remove(ObjectIdentifier(urlSchemeTask))
        let key = url.absoluteString
        let cacheable = key.contains("/assets/")
        if cacheable, let hit = cache[key] {
            respond(urlSchemeTask, url: url, data: hit.0, mime: hit.1, status: 200)
            return
        }
        Shell.shared.core.send(["op": "page", "url": key]) { [weak self] r in
            guard let self = self else { return }
            let body = r["body"] as? String ?? ""
            let mime = r["mime"] as? String ?? "text/html; charset=utf-8"
            let status = (r["status"] as? NSNumber)?.intValue ?? 200
            let data = Data(body.utf8)
            if cacheable && status == 200 { self.cache[key] = (data, mime) }
            self.respond(urlSchemeTask, url: url, data: data, mime: mime, status: status)
        }
    }

    func webView(_ webView: WKWebView, stop urlSchemeTask: WKURLSchemeTask) {
        stopped.insert(ObjectIdentifier(urlSchemeTask))
    }

    private func respond(_ task: WKURLSchemeTask, url: URL, data: Data, mime: String, status: Int) {
        let id = ObjectIdentifier(task)
        if stopped.contains(id) { stopped.remove(id); return }
        let headers = ["Content-Type": mime, "Content-Length": "\(data.count)", "Cache-Control": "no-store"]
        guard let response = HTTPURLResponse(url: url, statusCode: status, httpVersion: "HTTP/1.1", headerFields: headers) else { return }
        task.didReceive(response)
        task.didReceive(data)
        task.didFinish()
    }
}

// ============================================================== bridge
final class Bridge: NSObject, WKScriptMessageHandlerWithReply {
    static let shared = Bridge()

    func userContentController(_ userContentController: WKUserContentController,
                               didReceive message: WKScriptMessage,
                               replyHandler: @escaping (Any?, String?) -> Void) {
        // Only Sreon's own pages may talk to the engine. A website never can.
        guard message.frameInfo.isMainFrame,
              message.frameInfo.request.url?.scheme == "sreon",
              let body = message.body as? [String: Any],
              let op = body["op"] as? String,
              let web = message.webView,
              let win = Shell.shared.window(owning: web) else {
            replyHandler(nil, nil)
            return
        }
        let view = body["view"] as? String ?? ""
        let data = body["data"] as? [String: Any] ?? [:]
        win.handleBridge(op: op, view: view, data: data, from: web, reply: replyHandler)
    }
}

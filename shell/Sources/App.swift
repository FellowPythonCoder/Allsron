// Sreon — App.swift
// Application life cycle and the menu bar.
import Cocoa
import WebKit
import CoreServices

let sreonDelegate = AppDelegate()

final class AppDelegate: NSObject, NSApplicationDelegate {
    var appearanceToken: NSKeyValueObservation?
    var pendingURLs: [URL] = []
    var launched = false

    var splash: SplashWindow?
    var splashShownAt = Date()
    var splashWatchdog: DispatchWorkItem?
    var heldWindows: [BrowserWindow] = []
    /// True once no splash is on screen, so windows may be shown.
    var splashGone = true
    let splashMinimum: TimeInterval = 2.8

    /// Shows a window that was kept back while the splash played.
    func reveal(_ w: BrowserWindow) {
        w.window.alphaValue = 0
        w.window.makeKeyAndOrderFront(nil)
        NSAnimationContext.runAnimationGroup { ctx in
            ctx.duration = 0.3
            w.window.animator().alphaValue = 1
        }
    }

    /// Ends the splash once its animation has had time to play, then reveals the held windows.
    func endSplash() {
        splashWatchdog?.cancel()
        splashWatchdog = nil
        guard let sp = splash else {
            splashGone = true
            revealHeld()
            return
        }
        splash = nil
        let wait = max(0, splashMinimum - Date().timeIntervalSince(splashShownAt))
        DispatchQueue.main.asyncAfter(deadline: .now() + wait) { [weak self] in
            sp.dismiss {
                self?.splashGone = true
                self?.revealHeld()
            }
        }
    }

    func revealHeld() {
        let held = heldWindows
        heldWindows = []
        for w in held { reveal(w) }
        if !held.isEmpty { NSApp.activate(ignoringOtherApps: true) }
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        let shell = Shell.shared
        if UserDefaults.standard.object(forKey: "bootSplash") as? Bool ?? true {
            splash = SplashWindow(resources: shell.resources)
            splash?.present()
            splashShownAt = Date()
            splashGone = false
            // Whatever happens during start-up, the splash never stays longer than this.
            let dog = DispatchWorkItem { [weak self] in self?.endSplash() }
            splashWatchdog = dog
            DispatchQueue.main.asyncAfter(deadline: .now() + 14, execute: dog)
        }
        if !shell.startEngine() {
            let alert = NSAlert()
            splash?.dismiss(); splash = nil; splashGone = true
            splashWatchdog?.cancel()
            alert.messageText = "Sreon can't start its engine"
            alert.informativeText = "The SPRFST engine wasn't found inside the app. Reinstall Sreon."
            alert.runModal()
            NSApp.terminate(nil)
            return
        }
        MenuBuilder.build()
        shell.boot { [weak self] in
            guard let self = self else { return }
            MenuBuilder.build()
            self.launched = true
            UserDefaults.standard.set(shell.flag("boot_anim", true), forKey: "bootSplash")
            let restore = shell.str("startup", "newtab") == "restore"
            // While the splash plays, the browser window is made but kept hidden.
            let show = self.splashGone
            let made: BrowserWindow
            if let first = self.pendingURLs.first {
                made = shell.newWindow(isPrivate: false, startURL: first.absoluteString, show: show)
                for u in self.pendingURLs.dropFirst() { made.open(u.absoluteString, where: "tab") }
                self.pendingURLs = []
            } else {
                made = shell.newWindow(isPrivate: false, restore: restore, show: show)
            }
            if show { NSApp.activate(ignoringOtherApps: true) } else { self.heldWindows.append(made) }
            self.endSplash()
            self.watchAppearance()
            DispatchQueue.main.asyncAfter(deadline: .now() + self.splashMinimum + 1.0) { [weak self] in self?.offerDefaultBrowser() }
        }
    }

    func watchAppearance() {
        appearanceToken = NSApp.observe(\.effectiveAppearance, options: [.new]) { _, _ in
            let shell = Shell.shared
            if shell.str("theme", "midnight") != "auto" { return }
            shell.refreshTheme { for w in shell.windows { w.pushTheme() } }
        }
    }

    func offerDefaultBrowser() {
        let shell = Shell.shared
        if !shell.flag("default_hint", true) { return }
        if UserDefaults.standard.bool(forKey: "askedDefaultBrowser") { return }
        guard let https = URL(string: "https://example.com"),
              let current = NSWorkspace.shared.urlForApplication(toOpen: https) else { return }
        if current.standardizedFileURL == Bundle.main.bundleURL.standardizedFileURL { return }
        UserDefaults.standard.set(true, forKey: "askedDefaultBrowser")
        let alert = NSAlert()
        alert.messageText = "Make Sreon your default browser?"
        alert.informativeText = "Links from other apps will open in Sreon. You can change this any time in System Settings."
        alert.addButton(withTitle: "Make default")
        alert.addButton(withTitle: "Not now")
        if alert.runModal() == .alertFirstButtonReturn, let id = Bundle.main.bundleIdentifier {
            LSSetDefaultHandlerForURLScheme("http" as CFString, id as CFString)
            LSSetDefaultHandlerForURLScheme("https" as CFString, id as CFString)
        }
    }

    func application(_ application: NSApplication, open urls: [URL]) {
        let web = urls.filter { $0.scheme == "http" || $0.scheme == "https" || $0.isFileURL }
        if web.isEmpty { return }
        if !launched { pendingURLs.append(contentsOf: web); return }
        let shell = Shell.shared
        if let w = shell.windows.first(where: { !$0.isPrivate }) ?? shell.windows.first {
            for u in web { w.open(u.absoluteString, where: "tab") }
            if splashGone { w.window.makeKeyAndOrderFront(nil) }
        } else {
            shell.newWindow(isPrivate: false, startURL: web[0].absoluteString)
        }
        NSApp.activate(ignoringOtherApps: true)
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        if !flag && launched { Shell.shared.newWindow(isPrivate: false) }
        return true
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { return false }

    func applicationWillTerminate(_ notification: Notification) {
        let shell = Shell.shared
        shell.windows.first?.saveSessionNow()
        if shell.flag("clear_quit", false) {
            var done = false
            WKWebsiteDataStore.default().removeData(ofTypes: WKWebsiteDataStore.allWebsiteDataTypes(), modifiedSince: Date.distantPast) { done = true }
            let end = Date().addingTimeInterval(1.5)
            while !done && Date() < end { RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.02)) }
        }
        shell.core.send(["op": "quit"]) { _ in }
        Thread.sleep(forTimeInterval: 0.15)
        shell.core.stop()
        shell.worker.stop()
    }

    // ------------------------------------------------------------ menu hooks
    var keyBrowser: BrowserWindow? {
        let shell = Shell.shared
        return shell.windows.first { $0.window.isKeyWindow } ?? shell.windows.first
    }

    @objc func runCommand(_ sender: NSMenuItem) {
        guard let id = sender.representedObject as? String else { return }
        if keyBrowser == nil && (id == "new_window" || id == "new_tab") { Shell.shared.newWindow(isPrivate: false); return }
        keyBrowser?.runCommand(id)
    }

    @objc func showAbout(_ sender: Any?) { keyBrowser?.open("sreon://about", where: "tab") }
    @objc func openGuide(_ sender: Any?) { keyBrowser?.open("sreon://studio?tab=guide", where: "tab") }
    @objc func openShield(_ sender: Any?) { keyBrowser?.open("sreon://shield", where: "tab") }
}

enum MenuBuilder {
    static func parse(_ spec: String) -> (String, NSEvent.ModifierFlags)? {
        let parts = spec.lowercased().split(separator: "+").map(String.init)
        guard let key = parts.last, !key.isEmpty else { return nil }
        var mods: NSEvent.ModifierFlags = []
        for p in parts.dropLast() {
            switch p {
            case "cmd": mods.insert(.command)
            case "shift": mods.insert(.shift)
            case "alt", "opt", "option": mods.insert(.option)
            case "ctrl", "control": mods.insert(.control)
            default: break
            }
        }
        var k = key
        if key == "tab" { k = "\t" } else if key == "enter" || key == "return" { k = "\r" } else if key == "space" { k = " " } else if key == "esc" { k = "\u{1b}" }
        if k.count != 1 { return nil }
        return (k, mods)
    }

    static func build() {
        let shell = Shell.shared
        let main = NSMenu()
        var spec: [String: (String, String)] = [:]
        for a in shell.actions where a.count >= 3 { spec[a[0]] = (a[1], a[2]) }

        func item(_ id: String, into menu: NSMenu, title: String? = nil) {
            guard let s = spec[id] else { return }
            let mi = NSMenuItem(title: title ?? s.0, action: #selector(AppDelegate.runCommand(_:)), keyEquivalent: "")
            mi.target = sreonDelegate
            mi.representedObject = id
            if let (k, mods) = parse(s.1) { mi.keyEquivalent = k; mi.keyEquivalentModifierMask = mods }
            menu.addItem(mi)
        }
        func submenu(_ title: String) -> NSMenu {
            let top = NSMenuItem()
            let m = NSMenu(title: title)
            top.submenu = m
            main.addItem(top)
            return m
        }

        // Sreon
        let app = submenu("Sreon")
        let about = NSMenuItem(title: "About Sreon", action: #selector(AppDelegate.showAbout(_:)), keyEquivalent: "")
        about.target = sreonDelegate
        app.addItem(about)
        app.addItem(NSMenuItem.separator())
        item("settings", into: app, title: "Settings…")
        app.addItem(NSMenuItem.separator())
        app.addItem(NSMenuItem(title: "Hide Sreon", action: #selector(NSApplication.hide(_:)), keyEquivalent: "h"))
        let hideOthers = NSMenuItem(title: "Hide Others", action: #selector(NSApplication.hideOtherApplications(_:)), keyEquivalent: "h")
        hideOthers.keyEquivalentModifierMask = [.command, .option]
        app.addItem(hideOthers)
        app.addItem(NSMenuItem(title: "Show All", action: #selector(NSApplication.unhideAllApplications(_:)), keyEquivalent: ""))
        app.addItem(NSMenuItem.separator())
        app.addItem(NSMenuItem(title: "Quit Sreon", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q"))

        // File
        let file = submenu("File")
        for id in ["new_tab", "new_window", "private", "close_tab", "reopen_tab"] { item(id, into: file) }
        file.addItem(NSMenuItem.separator())
        for id in ["pin_tab", "sleep_tab", "copy_link", "screenshot"] { item(id, into: file) }

        // Edit
        let edit = submenu("Edit")
        edit.addItem(NSMenuItem(title: "Undo", action: Selector(("undo:")), keyEquivalent: "z"))
        let redo = NSMenuItem(title: "Redo", action: Selector(("redo:")), keyEquivalent: "z")
        redo.keyEquivalentModifierMask = [.command, .shift]
        edit.addItem(redo)
        edit.addItem(NSMenuItem.separator())
        edit.addItem(NSMenuItem(title: "Cut", action: #selector(NSText.cut(_:)), keyEquivalent: "x"))
        edit.addItem(NSMenuItem(title: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c"))
        edit.addItem(NSMenuItem(title: "Paste", action: #selector(NSText.paste(_:)), keyEquivalent: "v"))
        edit.addItem(NSMenuItem(title: "Select All", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a"))
        edit.addItem(NSMenuItem.separator())
        item("find", into: edit, title: "Find in Page…")

        // View
        let view = submenu("View")
        for id in ["toggle_sidebar", "toggle_panel", "split"] { item(id, into: view) }
        view.addItem(NSMenuItem.separator())
        for id in ["reload", "zoom_in", "zoom_out", "zoom_reset"] { item(id, into: view) }
        view.addItem(NSMenuItem.separator())
        for id in ["reader", "brief", "focus"] { item(id, into: view) }
        view.addItem(NSMenuItem.separator())
        let fs = NSMenuItem(title: "Enter Full Screen", action: #selector(NSWindow.toggleFullScreen(_:)), keyEquivalent: "f")
        fs.keyEquivalentModifierMask = [.command, .control]
        view.addItem(fs)

        // Go
        let go = submenu("Go")
        for id in ["back", "forward", "palette", "search_tabs", "next_tab", "prev_tab"] { item(id, into: go) }

        // Tools
        let tools = submenu("Tools")
        for id in ["studio", "whiteboard"] { item(id, into: tools) }
        let shield = NSMenuItem(title: "Shield Report", action: #selector(AppDelegate.openShield(_:)), keyEquivalent: "")
        shield.target = sreonDelegate
        tools.addItem(shield)

        // Window
        let win = submenu("Window")
        win.addItem(NSMenuItem(title: "Minimize", action: #selector(NSWindow.performMiniaturize(_:)), keyEquivalent: "m"))
        win.addItem(NSMenuItem(title: "Zoom", action: #selector(NSWindow.performZoom(_:)), keyEquivalent: ""))
        NSApp.windowsMenu = win

        // Help
        let help = submenu("Help")
        let guide = NSMenuItem(title: "Sreon Guidebook", action: #selector(AppDelegate.openGuide(_:)), keyEquivalent: "")
        guide.target = sreonDelegate
        help.addItem(guide)
        NSApp.helpMenu = help

        NSApp.mainMenu = main
    }
}

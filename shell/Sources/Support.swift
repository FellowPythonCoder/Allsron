// Sreon — Support.swift
// Small helpers: JSON, tabs, a toast, a find bar.
import Cocoa
import WebKit

func jsonString(_ object: Any) -> String {
    guard JSONSerialization.isValidJSONObject(object),
          let data = try? JSONSerialization.data(withJSONObject: object, options: []),
          let text = String(data: data, encoding: .utf8) else { return "{}" }
    return text
}

func jsLiteral(_ text: String) -> String {
    guard let data = try? JSONSerialization.data(withJSONObject: [text], options: []),
          let t = String(data: data, encoding: .utf8) else { return "\"\"" }
    return String(t.dropFirst().dropLast())
}

func color(from css: String, name: String, fallback: NSColor) -> NSColor {
    guard let range = css.range(of: "--\(name):#") else { return fallback }
    let start = range.upperBound
    let hex = String(css[start...].prefix(6))
    guard hex.count == 6, let value = UInt32(hex, radix: 16) else { return fallback }
    return NSColor(calibratedRed: CGFloat((value >> 16) & 255) / 255,
                   green: CGFloat((value >> 8) & 255) / 255,
                   blue: CGFloat(value & 255) / 255, alpha: 1)
}

final class Tab {
    let id: Int
    var url: String
    var title: String
    var loading = false
    var progress = 0.0
    var pinned = false
    var sleeping = false
    var ws: String
    var canBack = false
    var canForward = false
    var secure = false
    var isPrivate: Bool
    var web: WKWebView?
    var lastActive = Date()
    var observers: [NSKeyValueObservation] = []

    init(id: Int, url: String, ws: String, isPrivate: Bool) {
        self.id = id
        self.url = url
        self.title = ""
        self.ws = ws
        self.isPrivate = isPrivate
    }

    var dictionary: [String: Any] {
        return ["id": id, "title": title, "url": url, "loading": loading, "pinned": pinned,
                "sleeping": sleeping, "ws": ws, "back": canBack, "forward": canForward,
                "secure": secure, "progress": progress, "private": isPrivate]
    }
}

/// A short message that floats above the page and fades away.
final class Toast: NSVisualEffectView {
    private let label = NSTextField(labelWithString: "")
    private var hideWork: DispatchWorkItem?

    init() {
        super.init(frame: .zero)
        material = .hudWindow
        blendingMode = .withinWindow
        state = .active
        wantsLayer = true
        layer?.cornerRadius = 16
        layer?.masksToBounds = true
        label.font = NSFont.systemFont(ofSize: 13, weight: .medium)
        label.textColor = .labelColor
        addSubview(label)
        alphaValue = 0
        isHidden = true
    }
    required init?(coder: NSCoder) { fatalError("not used") }

    func show(_ text: String, in host: NSView) {
        label.stringValue = text
        label.sizeToFit()
        let w = label.frame.width + 36
        let h: CGFloat = 32
        frame = NSRect(x: (host.bounds.width - w) / 2, y: 28, width: w, height: h)
        label.frame = NSRect(x: 18, y: (h - label.frame.height) / 2, width: label.frame.width, height: label.frame.height)
        autoresizingMask = [.minXMargin, .maxXMargin, .maxYMargin]
        if superview !== host { host.addSubview(self) }
        isHidden = false
        NSAnimationContext.runAnimationGroup { ctx in
            ctx.duration = 0.18
            self.animator().alphaValue = 1
        }
        hideWork?.cancel()
        let work = DispatchWorkItem { [weak self] in
            NSAnimationContext.runAnimationGroup({ ctx in
                ctx.duration = 0.3
                self?.animator().alphaValue = 0
            }, completionHandler: { self?.isHidden = true })
        }
        hideWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.2, execute: work)
    }
}

/// A small find-in-page bar.
final class FindBar: NSVisualEffectView, NSSearchFieldDelegate {
    let field = NSSearchField()
    var onFind: ((String, Bool) -> Void)?
    var onClose: (() -> Void)?

    init() {
        super.init(frame: NSRect(x: 0, y: 0, width: 330, height: 40))
        material = .hudWindow
        blendingMode = .withinWindow
        state = .active
        wantsLayer = true
        layer?.cornerRadius = 12
        layer?.masksToBounds = true
        field.frame = NSRect(x: 10, y: 7, width: 200, height: 26)
        field.placeholderString = "Find in page"
        field.delegate = self
        field.sendsSearchStringImmediately = true
        addSubview(field)
        let prev = NSButton(title: "‹", target: self, action: #selector(previous))
        prev.frame = NSRect(x: 216, y: 7, width: 30, height: 26)
        prev.bezelStyle = .texturedRounded
        let next = NSButton(title: "›", target: self, action: #selector(nextMatch))
        next.frame = NSRect(x: 248, y: 7, width: 30, height: 26)
        next.bezelStyle = .texturedRounded
        let done = NSButton(title: "Done", target: self, action: #selector(close))
        done.frame = NSRect(x: 282, y: 7, width: 42, height: 26)
        done.bezelStyle = .texturedRounded
        done.font = NSFont.systemFont(ofSize: 11)
        addSubview(prev); addSubview(next); addSubview(done)
    }
    required init?(coder: NSCoder) { fatalError("not used") }

    func controlTextDidChange(_ obj: Notification) { onFind?(field.stringValue, false) }
    func control(_ control: NSControl, textView: NSTextView, doCommandBy commandSelector: Selector) -> Bool {
        if commandSelector == #selector(NSResponder.insertNewline(_:)) { onFind?(field.stringValue, false); return true }
        if commandSelector == #selector(NSResponder.cancelOperation(_:)) { onClose?(); return true }
        return false
    }
    @objc private func previous() { onFind?(field.stringValue, true) }
    @objc private func nextMatch() { onFind?(field.stringValue, false) }
    @objc private func close() { onClose?() }
}

/// A window-wide drag handle for the transparent title area.
final class DragView: NSView {
    override var mouseDownCanMoveWindow: Bool { return true }
    override func mouseDown(with event: NSEvent) {
        if event.clickCount == 2 { window?.zoom(nil) } else { window?.performDrag(with: event) }
    }
}

// MARK: - Boot splash
// A small borderless window that plays the Sreon mark while the engine starts.
// It is shown alone: the browser window stays hidden until the splash has gone.
final class SplashWindow: NSObject {
    let window: NSWindow
    let web: WKWebView
    private var closing = false
    private var torn = false

    init(resources: String) {
        let size = NSSize(width: 460, height: 360)
        let screen = NSScreen.main?.visibleFrame ?? NSRect(x: 0, y: 0, width: 1200, height: 800)
        let frame = NSRect(x: screen.midX - size.width / 2, y: screen.midY - size.height / 2, width: size.width, height: size.height)
        window = NSWindow(contentRect: frame, styleMask: [.borderless], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.isOpaque = false
        window.backgroundColor = .clear
        window.hasShadow = true
        window.level = .floating
        window.isMovableByWindowBackground = true
        let cfg = WKWebViewConfiguration()
        web = WKWebView(frame: NSRect(origin: .zero, size: size), configuration: cfg)
        web.setValue(false, forKey: "drawsBackground")
        web.wantsLayer = true
        web.layer?.cornerRadius = 22
        web.layer?.masksToBounds = true
        window.contentView = web
        super.init()
        var url = URL(fileURLWithPath: resources).appendingPathComponent("splash.html")
        if !FileManager.default.fileExists(atPath: url.path) {
            url = URL(fileURLWithPath: resources).appendingPathComponent("assets/splash.html")
        }
        if FileManager.default.fileExists(atPath: url.path) {
            web.loadFileURL(url, allowingReadAccessTo: url.deletingLastPathComponent())
        }
    }

    func present() {
        window.alphaValue = 0
        window.makeKeyAndOrderFront(nil)
        NSAnimationContext.runAnimationGroup { ctx in
            ctx.duration = 0.18
            window.animator().alphaValue = 1
        }
    }

    /// Removes the window for good. Safe to call more than once.
    private func teardown() {
        if torn { return }
        torn = true
        web.stopLoading()
        window.orderOut(nil)
        window.contentView = nil
        window.close()
    }

    /// Plays the exit, removes the window, then calls `done` exactly once.
    func dismiss(_ done: @escaping () -> Void = {}) {
        if closing { return }
        closing = true
        var finished = false
        let finish: () -> Void = { [weak self] in
            if finished { return }
            finished = true
            self?.teardown()
            done()
        }
        web.evaluateJavaScript("window.fadeOut && window.fadeOut()", completionHandler: nil)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.6) { [weak self] in
            guard let self = self else { finish(); return }
            NSAnimationContext.runAnimationGroup({ ctx in
                ctx.duration = 0.2
                self.window.animator().alphaValue = 0
            }, completionHandler: { finish() })
        }
        // If the animation never reports back, the window is removed anyway.
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.4) { finish() }
    }
}

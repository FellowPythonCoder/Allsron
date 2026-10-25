// Sreon — Engine.swift
// Talks to the SPRFST engine: one JSON request per line in, one JSON answer per line out.
import Foundation

final class Engine {
    let role: String
    private var process: Process?
    private var inPipe = Pipe()
    private var outPipe = Pipe()
    private var buffer = Data()
    private var nextID = 1
    private var waiting: [Int: ([String: Any]) -> Void] = [:]
    private let lock = NSLock()
    private(set) var alive = false
    var onExit: (() -> Void)?

    init(role: String) { self.role = role }

    @discardableResult
    func start(interpreter: String, entry: String, resources: String) -> Bool {
        // Fresh pipes every launch: Process closes its ends of a pipe after launch, so a restart cannot reuse them.
        signal(SIGPIPE, SIG_IGN)
        inPipe = Pipe()
        outPipe = Pipe()
        buffer = Data()
        let p = Process()
        p.executableURL = URL(fileURLWithPath: interpreter)
        p.arguments = ["run", entry, "--", role == "worker" ? "--worker" : "--serve"]
        var env = ProcessInfo.processInfo.environment
        env["SREON_RES"] = resources
        env["SPRFST_HOME"] = resources
        env["SREON_SPRFST"] = interpreter
        env["NO_COLOR"] = "1"
        p.environment = env
        p.currentDirectoryURL = URL(fileURLWithPath: resources)
        p.standardInput = inPipe
        p.standardOutput = outPipe
        p.standardError = FileHandle.nullDevice
        outPipe.fileHandleForReading.readabilityHandler = { [weak self, weak p] handle in
            let data = handle.availableData
            guard let self = self else { return }
            if data.isEmpty {
                handle.readabilityHandler = nil
                DispatchQueue.main.async { self.finished(p) }
                return
            }
            self.consume(data)
        }
        p.terminationHandler = { [weak self, weak p] _ in
            DispatchQueue.main.async { self?.finished(p) }
        }
        do { try p.run() } catch { return false }
        process = p
        alive = true
        return true
    }

    /// Only the current process may declare the engine finished (a stale EOF/termination from an earlier launch is ignored).
    private func finished(_ from: Process?) {
        guard let from = from, process === from else { return }
        if !alive { return }
        alive = false
        lock.lock()
        let pending = waiting
        waiting.removeAll()
        lock.unlock()
        for (_, done) in pending { done(["ok": false, "error": "engine stopped"]) }
        onExit?()
    }

    private func consume(_ data: Data) {
        buffer.append(data)
        while let nl = buffer.firstIndex(of: 0x0A) {
            let line = buffer.subdata(in: buffer.startIndex..<nl)
            buffer.removeSubrange(buffer.startIndex...nl)
            guard !line.isEmpty,
                  let obj = try? JSONSerialization.jsonObject(with: line, options: []),
                  let dict = obj as? [String: Any] else { continue }
            guard let id = (dict["id"] as? NSNumber)?.intValue else { continue }
            lock.lock()
            let done = waiting.removeValue(forKey: id)
            lock.unlock()
            if let done = done {
                DispatchQueue.main.async { done(dict) }
            }
        }
    }

    /// Sends a request. The answer arrives on the main queue.
    func send(_ request: [String: Any], _ done: @escaping ([String: Any]) -> Void) {
        guard alive else { DispatchQueue.main.async { done(["ok": false, "error": "engine not running"]) }; return }
        lock.lock()
        let id = nextID
        nextID += 1
        waiting[id] = done
        lock.unlock()
        var req = request
        req["id"] = id
        guard JSONSerialization.isValidJSONObject(req),
              var line = try? JSONSerialization.data(withJSONObject: req, options: []) else {
            lock.lock(); waiting.removeValue(forKey: id); lock.unlock()
            DispatchQueue.main.async { done(["ok": false, "error": "bad request"]) }
            return
        }
        line.append(0x0A)
        try? inPipe.fileHandleForWriting.write(contentsOf: line)
    }

    func stop() {
        alive = false
        outPipe.fileHandleForReading.readabilityHandler = nil
        process?.terminate()
    }
}

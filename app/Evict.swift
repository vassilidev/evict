import CryptoKit
import Quartz
import SwiftUI
import WebKit

@main
struct EvictApp: App {
    @StateObject private var model = Model()

    init() {
        // `Evict --update`: check and install the latest release without any window (scripts, tests)
        if CommandLine.arguments.contains("--update") { Updater.commandLine() }
    }

    var body: some Scene {
        WindowGroup("Evict") {
            ContentView()
                .environmentObject(model)
                .frame(minWidth: 900, minHeight: 600)
                .preferredColorScheme(.dark)
                .onAppear {
                    if model.state == .idle { model.start() }
                    model.watchForUpdates()
                }
                // A compact toolbar in the page's color, so the window reads as one surface
                .toolbarBackground(Color(red: 0.047, green: 0.055, blue: 0.071), for: .windowToolbar)
                .toolbarBackground(.visible, for: .windowToolbar)
        }
        .windowToolbarStyle(.unifiedCompact(showsTitle: false))
        .commands {
            CommandGroup(after: .appInfo) {
                Button("Check for Updates…") { Task { await model.checkForUpdates(manual: true) } }
            }
            CommandGroup(after: .newItem) {
                Button("Rescan") { model.scan() }.keyboardShortcut("r")
                Button("Full Disk Access…") { model.openFullDiskAccess() }
            }
        }
    }
}

@MainActor
final class Model: ObservableObject {
    enum State: Equatable { case idle, needsAccess, scanning, ready(URL), failed(String) }
    @Published var state = State.idle
    @Published var update: Updater.Release?
    @Published var updating = false
    @Published var page: URL?  // live map while scanning, then the final one
    weak var webView: WKWebView?

    init() {
        Task.detached { _ = Model.claude }  // a login shell may be needed: keep it off the main thread
    }
    private var watching = false

    // At launch, then every 6 hours for Macs that keep Evict open
    func watchForUpdates() {
        guard !watching else { return }
        watching = true
        Task {
            while true {
                await checkForUpdates(manual: false)
                try? await Task.sleep(for: .seconds(6 * 3600))
            }
        }
    }

    func checkForUpdates(manual: Bool) async {
        do {
            let release = try await Updater.latest()
            if Updater.isNewer(release) {
                update = release
                // Each new version is announced once; the toolbar button stays as the reminder
                let announced = UserDefaults.standard.string(forKey: "announcedVersion")
                if manual || announced != release.version {
                    UserDefaults.standard.set(release.version, forKey: "announcedVersion")
                    promptUpdate()
                }
            } else if manual {
                inform("Evict is up to date", "You’re running the latest version (\(Updater.current)).")
            }
        } catch {
            if manual { inform("Couldn’t check for updates", error.localizedDescription) }
        }
    }

    func promptUpdate() {
        guard let release = update, !updating else { return }
        let alert = NSAlert()
        alert.messageText = "Evict \(release.version) is available"
        let notes = release.body.map { "\n\n" + $0.replacingOccurrences(of: "**", with: "") } ?? ""
        alert.informativeText = "You’re running version \(Updater.current).\(notes)"
        alert.addButton(withTitle: "Install and Relaunch")
        alert.addButton(withTitle: "Later")
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        updating = true
        Task {
            do {
                try await Task.detached { try await Updater.install(release) }.value
                relaunch()
            } catch {
                updating = false
                let fail = NSAlert()
                fail.messageText = "Update failed"
                fail.informativeText = "\(error.localizedDescription)\n\nYou can download the latest version from the Evict website."
                fail.addButton(withTitle: "Open Website")
                fail.addButton(withTitle: "Close")
                if fail.runModal() == .alertFirstButtonReturn { NSWorkspace.shared.open(Updater.site) }
            }
        }
    }

    private func inform(_ title: String, _ text: String) {
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = text
        alert.runModal()
    }

    // The new bundle is already in place: reopen it once this process has quit
    private func relaunch() {
        let shell = Process()
        shell.executableURL = URL(fileURLWithPath: "/bin/sh")
        shell.arguments = ["-c", "sleep 1; /usr/bin/open \"$0\"", Bundle.main.bundlePath]
        try? shell.run()
        NSApp.terminate(nil)
    }

    var isScanning: Bool { if case .scanning = state { return true } else { return false } }

    // TCC.db can only be opened with Full Disk Access, which makes it a reliable probe
    var hasFullDiskAccess: Bool {
        FileHandle(forReadingAtPath: "/Library/Application Support/com.apple.TCC/TCC.db") != nil
    }

    func openFullDiskAccess() {
        NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles")!)
    }

    // Without Full Disk Access, every protected folder (Desktop, Documents, other apps' data…)
    // makes macOS pop a consent dialog and the scan waits on it, so ask for the access first
    func start() {
        if hasFullDiskAccess { scan() } else { state = .needsAccess }
    }

    func scan() {
        if isScanning { return }
        state = .scanning
        guard let scanner = Bundle.main.url(forAuxiliaryExecutable: "evict-scan"),
              let template = Bundle.main.url(forResource: "index", withExtension: "html") else {
            state = .failed("App files are missing, please reinstall Evict.")
            return
        }
        page = Model.livePage(template: template) ?? page
        Task {
            let result = await Task.detached(priority: .userInitiated) {
                Model.runScan(scanner: scanner, template: template) { line in
                    Task { @MainActor in self.showLive(line) }
                }
            }.value
            switch result {
            case .success(let url): page = url; state = .ready(url)
            case .failure(let error): state = .failed(error.localizedDescription)
            }
        }
    }

    private func showLive(_ line: String) {
        webView?.evaluateJavaScript("evictLive(\(line))")
    }

    // The page in live mode: the scanner's progress lines draw the map while it fills up
    private static func livePage(template: URL) -> URL? {
        guard let html = try? String(contentsOf: template, encoding: .utf8) else { return nil }
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("evict-live.html")
        let live = html.replacingOccurrences(of: "window.DATA = null; /* __DATA__ */", with: "window.DATA = null; window.LIVE = {root: '/'};")
        return (try? live.write(to: url, atomically: true, encoding: .utf8)) != nil ? url : nil
    }

    nonisolated static func runScan(scanner: URL, template: URL, progress: @escaping @Sendable (String) -> Void) -> Result<URL, Error> {
        let process = Process(), out = Pipe(), err = Pipe()
        process.executableURL = scanner
        process.arguments = ["/"]
        process.environment = ["EVICT_PROGRESS": "1", "HOME": NSHomeDirectory()]
        process.standardOutput = out
        process.standardError = err
        // With EVICT_PROGRESS the scanner writes a JSON progress line on stderr every 300 ms; only the latest matters
        let buffer = LineBuffer()
        err.fileHandleForReading.readabilityHandler = { handle in
            if let line = buffer.lines(handle.availableData).last(where: { $0.hasPrefix("{") }) { progress(line) }
        }
        do {
            try process.run()
            let json = out.fileHandleForReading.readDataToEndOfFile()
            process.waitUntilExit()
            err.fileHandleForReading.readabilityHandler = nil
            guard process.terminationStatus == 0, !json.isEmpty else {
                throw NSError(domain: "Evict", code: 1, userInfo: [NSLocalizedDescriptionKey: "The scanner stopped (code \(process.terminationStatus))."])
            }
            let html = try String(contentsOf: template, encoding: .utf8)
            guard let slot = html.range(of: "window.DATA = null; /* __DATA__ */") else {
                throw NSError(domain: "Evict", code: 2, userInfo: [NSLocalizedDescriptionKey: "Invalid page template."])
            }
            var page = Data(html[..<slot.lowerBound].utf8)
            page.append(Data("window.DATA=".utf8))
            page.append(json)
            page.append(Data(html[slot.upperBound...].utf8))
            let url = FileManager.default.temporaryDirectory.appendingPathComponent("evict.html")
            try page.write(to: url)
            return .success(url)
        } catch {
            err.fileHandleForReading.readabilityHandler = nil
            return .failure(error)
        }
    }

    // Decimal units like Finder and the web page, always in English
    nonisolated static func bytes(_ n: Int64) -> String {
        let units = ["B", "KB", "MB", "GB", "TB"]
        var value = Double(n), i = 0
        while value >= 1000 && i < units.count - 1 { value /= 1000; i += 1 }
        return String(format: i > 0 && value < 10 ? "%.1f %@" : "%.0f %@", value, units[i])
    }

    // Same rules as canRm() in index.html: never the disk root, top-level folders, homes or system paths
    static func canRemove(_ path: String) -> Bool {
        guard path.hasPrefix("/"), (path as NSString).standardizingPath == path,
              path.split(separator: "/").count >= 2 else { return false }
        if path.range(of: #"^/(System|private|usr|bin|sbin|cores)(/|$)"#, options: .regularExpression) != nil { return false }
        if path.range(of: #"^/Users/[^/]+(/Library|/\.Trash)?$"#, options: .regularExpression) != nil { return false }
        if path.range(of: #"^/Users/[^/]+/(\.ssh|\.gnupg|\.aws|\.azure|\.kube|\.password-store|\.config/gcloud|Library/Keychains)(/|$)"#, options: .regularExpression) != nil { return false }
        return true
    }

    static let trashPath = (NSHomeDirectory() as NSString).appendingPathComponent(".Trash")

    // One or many items: everything goes to the Trash, or everything is deleted for good (items already in the Trash always are)
    func confirmDelete(_ items: [(path: String, size: Int64)]) {
        let items = items.filter { Model.canRemove($0.path) }
        guard let first = items.first else { return }
        let inTrash = { (path: String) in path.hasPrefix(Model.trashPath + "/") }
        let allInTrash = items.allSatisfy { inTrash($0.path) }, total = items.reduce(0) { $0 + $1.size }
        let alert = NSAlert()
        alert.alertStyle = .warning
        if items.count == 1 {
            alert.messageText = "Delete “\((first.path as NSString).lastPathComponent)”?"
            alert.informativeText = "\(Model.bytes(total)) · \(first.path)\n\n"
        } else {
            let names = items.prefix(8).map { "• " + ($0.path as NSString).lastPathComponent }.joined(separator: "\n")
            alert.messageText = "Delete \(items.count) items?"
            alert.informativeText = "\(Model.bytes(total)) in total\n\n\(names)\(items.count > 8 ? "\n• and \(items.count - 8) more" : "")\n\n"
        }
        alert.informativeText += allInTrash ? "Already in the Trash: deleted permanently."
            : "The Trash keeps \(items.count > 1 ? "them" : "it") recoverable, but the space is only freed once the Trash is emptied."
        let secrets = Model.secrets(in: items.map(\.path))
        if !secrets.isEmpty {
            let home = NSHomeDirectory(), shown = secrets.prefix(6).map { "• " + $0.replacingOccurrences(of: home, with: "~") }
            alert.informativeText += "\n\n⚠️ Secrets inside, often in no git repository or backup:\n" + shown.joined(separator: "\n")
                + (secrets.count > 6 ? "\n• and more" : "")
        }
        if !allInTrash { alert.addButton(withTitle: "Move to Trash") }
        alert.addButton(withTitle: "Delete Permanently").hasDestructiveAction = true
        alert.addButton(withTitle: "Cancel")
        let permanent: Bool
        switch (alert.runModal(), allInTrash) {
        case (.alertFirstButtonReturn, false): permanent = false
        case (.alertFirstButtonReturn, true), (.alertSecondButtonReturn, false): permanent = true
        default: return
        }
        var trashed: [String] = [], deleted: [String] = [], failed: [String] = [], reason = ""
        for item in items {
            let url = URL(fileURLWithPath: item.path)
            do {
                if permanent || inTrash(item.path) {
                    try FileManager.default.removeItem(at: url)
                    deleted.append(item.path)
                } else {
                    try FileManager.default.trashItem(at: url, resultingItemURL: nil)
                    trashed.append(item.path)
                }
            } catch {
                failed.append(item.path)
                reason = error.localizedDescription
            }
        }
        removed(trashed, freed: false)
        removed(deleted, freed: true)
        if !failed.isEmpty { offerAdmin(failed, reason: reason) }
    }

    // Empties the user's Trash item by item; the page can only ask for this exact folder
    func confirmEmptyTrash(path: String, size: Int64) {
        guard path == Model.trashPath else { return }
        let items = (try? FileManager.default.contentsOfDirectory(atPath: path)) ?? []
        guard !items.isEmpty else { return inform("The Trash is already empty", "Rescan to update the map.") }
        let alert = NSAlert()
        alert.alertStyle = .warning
        alert.messageText = "Empty the Trash?"
        alert.informativeText = "\(items.count) item\(items.count > 1 ? "s" : ""), \(Model.bytes(size)), will be deleted permanently. This can’t be undone."
        alert.addButton(withTitle: "Empty Trash").hasDestructiveAction = true
        alert.addButton(withTitle: "Cancel")
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        let failed = items.filter { (try? FileManager.default.removeItem(atPath: (path as NSString).appendingPathComponent($0))) == nil }
        if !failed.isEmpty {
            inform("Some items couldn’t be deleted", "\(failed.count) item\(failed.count > 1 ? "s belong" : " belongs") to another user or are in use. Empty the Trash from the Finder to remove them.")
        }
        guard let arg = try? String(data: JSONEncoder().encode(path), encoding: .utf8) else { return }
        webView?.evaluateJavaScript("evictEmptied(\(arg))")
    }

    // Secrets (.env, private keys, credentials) found in what's about to be deleted. A quick look: heavy dependency
    // folders are skipped and the walk stops after a second, so a huge folder can hide some
    nonisolated static func secrets(in paths: [String]) -> [String] {
        let deadline = Date().addingTimeInterval(1)
        var found: [String] = []
        for path in paths {
            if isSecret((path as NSString).lastPathComponent) { found.append(path); continue }
            guard let walker = FileManager.default.enumerator(at: URL(fileURLWithPath: path), includingPropertiesForKeys: nil) else { continue }
            for case let url as URL in walker {
                let name = url.lastPathComponent
                if name.range(of: #"^(node_modules|\.git|vendor|Pods|DerivedData|\.venv|venv|__pycache__|\.next|target)$"#, options: .regularExpression) != nil {
                    walker.skipDescendants()
                } else if isSecret(name) {
                    found.append(url.path)
                }
                if found.count >= 50 || Date() > deadline { return found }
            }
        }
        return found
    }

    nonisolated static func isSecret(_ name: String) -> Bool {
        name.range(of: #"^(\.env(\..+)?|id_(rsa|dsa|ecdsa|ed25519)|\.npmrc|\.netrc|\.pgpass|credentials(\.json)?|.+\.(pem|key|p12|pfx|keystore|jks|kdbx|ovpn))$"#,
                   options: [.regularExpression, .caseInsensitive]) != nil
            && name.range(of: #"\.(example|sample|template)$"#, options: .regularExpression) == nil
    }

    // Files owned by root or another user: retry with the native administrator password prompt
    private func offerAdmin(_ paths: [String], reason: String) {
        let alert = NSAlert()
        alert.messageText = "Permission denied"
        alert.informativeText = "\(reason)\n\nDelete \(paths.count > 1 ? "these \(paths.count) items" : "it") permanently using an administrator password?"
        alert.addButton(withTitle: "Delete as Administrator").hasDestructiveAction = true
        alert.addButton(withTitle: "Cancel")
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        let quoted = paths.map { "quoted form of \"" + $0.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"") + "\"" }
        let script = NSAppleScript(source: "do shell script \"/bin/rm -rf -- \" & " + quoted.joined(separator: " & \" \" & ") + " with administrator privileges")
        var error: NSDictionary?
        script?.executeAndReturnError(&error)
        if let error {
            if (error[NSAppleScript.errorNumber] as? Int) == -128 { return }  // cancelled by the user
            let fail = NSAlert()
            fail.messageText = "Couldn’t delete"
            fail.informativeText = error[NSAppleScript.errorMessage] as? String ?? "Unknown error"
            fail.runModal()
            return
        }
        removed(paths, freed: true)
    }

    private func removed(_ paths: [String], freed: Bool) {
        guard !paths.isEmpty, let arg = try? String(data: JSONEncoder().encode(paths), encoding: .utf8) else { return }
        webView?.evaluateJavaScript("evictRemoved(\(arg), \(freed))")
    }

    // GUI apps don't get the shell's PATH: look where Claude Code's installers put the CLI, then ask a login shell
    nonisolated static let claude: String? = {
        let home = NSHomeDirectory()
        let known = ["\(home)/.local/bin/claude", "\(home)/.claude/local/claude", "/opt/homebrew/bin/claude", "/usr/local/bin/claude"]
        if let path = known.first(where: FileManager.default.isExecutableFile(atPath:)) { return path }
        let process = Process(), out = Pipe()
        process.executableURL = URL(fileURLWithPath: "/bin/zsh")
        process.arguments = ["-lc", "command -v claude"]
        process.standardOutput = out
        process.standardError = FileHandle.nullDevice
        guard (try? process.run()) != nil else { return nil }
        let path = String(decoding: out.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
        process.waitUntilExit()
        return path.hasPrefix("/") && FileManager.default.isExecutableFile(atPath: path) ? path : nil
    }()

    private var running: Process?

    // Streams Claude's answer into the page as it's written; a new question or closing the panel stops the previous one
    func ask(id: Int, prompt: String) {
        cancelAsk()
        guard let claude = Model.claude else { return answer(id, "Claude Code isn’t installed.", "error") }
        let process = Process(), out = Pipe()
        process.executableURL = URL(fileURLWithPath: claude)
        // No tools at all: Claude can only answer, it can't read, run or delete anything.
        // Opus at low effort, measured: a quick wins plan in ~20 s (minutes at default effort), smarter advice than Sonnet
        process.arguments = ["-p", "--model", "opus", "--effort", "low", "--tools", "", "--no-session-persistence",
                             "--output-format", "stream-json", "--include-partial-messages", "--verbose", prompt]
        process.currentDirectoryURL = FileManager.default.temporaryDirectory
        process.standardInput = FileHandle.nullDevice
        process.standardOutput = out
        process.standardError = out
        do { try process.run() } catch { return answer(id, error.localizedDescription, "error") }
        running = process
        Task {
            var done = false, problem = ""
            do {
                for try await line in out.fileHandleForReading.bytes.lines {
                    guard let event = (try? JSONSerialization.jsonObject(with: Data(line.utf8))) as? [String: Any] else {
                        problem += line + "\n"
                        continue
                    }
                    if let delta = ((event["event"] as? [String: Any])?["delta"] as? [String: Any])?["text"] as? String {
                        answer(id, delta, "delta")
                    } else if event["type"] as? String == "result" {
                        done = event["is_error"] as? Bool != true
                        if !done { problem += event["result"] as? String ?? "" }
                    }
                }
            } catch {}
            if running === process { running = nil }
            answer(id, problem, done ? "done" : "error")
        }
    }

    // Lists an archive with the system's bsdtar (zip, rar, 7z, tar.*, iso, xar, cpio…), biggest files first
    func listArchive(id: Int, path: String) {
        cancelAsk()
        let process = Process(), out = Pipe(), err = Pipe()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/tar")
        process.arguments = ["-tvf", path]
        process.environment = ["LC_ALL": "en_US.UTF-8"]  // names in UTF-8, not escaped
        process.standardInput = FileHandle.nullDevice
        process.standardOutput = out
        process.standardError = err
        do { try process.run() } catch { return send("evictArchive", id, ["error": error.localizedDescription]) }
        running = process
        Task {
            var files: [(String, Int64)] = [], total: Int64 = 0
            do {
                for try await line in out.fileHandleForReading.bytes.lines {
                    // mode, links, owner, group, size, month, day, time or year, name
                    let field = line.split(separator: " ", maxSplits: 8, omittingEmptySubsequences: true)
                    guard field.count == 9, !field[0].hasPrefix("d"), let size = Int64(field[4]) else { continue }
                    files.append((String(field[8]), size))
                    total += size
                }
            } catch {}
            if running === process { running = nil }
            guard !files.isEmpty else {
                let message = String(decoding: err.fileHandleForReading.availableData, as: UTF8.self)
                    .replacingOccurrences(of: "tar: ", with: "").trimmingCharacters(in: .whitespacesAndNewlines)
                return send("evictArchive", id, ["error": message.isEmpty ? "This archive is empty." : message])
            }
            files.sort { $0.1 > $1.1 }
            send("evictArchive", id, ["count": files.count, "total": total, "entries": files.prefix(500).map { [$0.0, $0.1] }])
        }
    }

    private func send(_ function: String, _ id: Int, _ payload: [String: Any]) {
        guard let data = try? JSONSerialization.data(withJSONObject: payload), let arg = String(data: data, encoding: .utf8) else { return }
        webView?.evaluateJavaScript("\(function)(\(id), \(arg))")
    }

    private var job: Task<Void, Never>?

    // Candidate sets from the page (same size and type) checked for real: same byte count and same sampled content
    func findDuplicates(id: Int, groups: [[String]]) {
        cancelAsk()
        job = Task {
            let sets = await Model.compare(groups)
            if !Task.isCancelled { send("evictDupes", id, ["sets": sets]) }
        }
    }

    // Runs off the main thread, and stops when the panel closes (the task is cancelled)
    nonisolated static func compare(_ groups: [[String]]) async -> [[[Any]]] {
        var sets: [[[Any]]] = []
        for group in groups {
            if Task.isCancelled { break }
            var byPrint: [String: [[Any]]] = [:]
            for path in group {
                if let key = fingerprint(path) { byPrint[key, default: []].append([path, privateSize(path) ?? -1]) }
            }
            sets += byPrint.values.filter { $0.count > 1 }
        }
        return sets
    }

    // Size, then SHA-256 of the first, middle and last megabyte: enough to tell copies apart without reading them whole
    nonisolated static func fingerprint(_ path: String) -> String? {
        guard let handle = FileHandle(forReadingAtPath: path) else { return nil }
        defer { try? handle.close() }
        guard let size = try? handle.seekToEnd() else { return nil }
        let chunk: UInt64 = 1 << 20, small = size <= 3 * chunk
        var hash = SHA256()
        for offset in small ? [0] : [0, size / 2, size - chunk] {
            try? handle.seek(toOffset: offset)
            if let data = try? handle.read(upToCount: Int(small ? size : chunk)) { hash.update(data: data) }
        }
        return "\(size):" + hash.finalize().map { String(format: "%02x", $0) }.joined()
    }

    // Bytes only this file owns: an APFS clone shares its blocks with the original, so deleting it frees about nothing
    nonisolated static func privateSize(_ path: String) -> Int64? {
        var list = attrlist()
        list.bitmapcount = u_short(ATTR_BIT_MAP_COUNT)
        list.forkattr = attrgroup_t(ATTR_CMNEXT_PRIVATESIZE)
        var buf = [UInt8](repeating: 0, count: 64)
        guard getattrlist(path, &list, &buf, buf.count, UInt32(FSOPT_ATTR_CMN_EXTENDED)) == 0 else { return nil }
        return buf.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 4, as: Int64.self) }
    }

    func cancelAsk() {
        job?.cancel()
        job = nil
        running?.terminate()
        running = nil
    }

    private func answer(_ id: Int, _ text: String, _ kind: String) {
        guard let arg = try? String(data: JSONEncoder().encode(text), encoding: .utf8) else { return }
        webView?.evaluateJavaScript("evictAnswer(\(id), \(arg), '\(kind)')")
    }
}

struct ContentView: View {
    @EnvironmentObject var model: Model

    var body: some View {
        Group {
            switch model.state {
            case .needsAccess: AccessView()
            case .failed(let message):
                VStack(spacing: 12) {
                    Text("The scan failed").font(.title2.weight(.semibold))
                    Text(message).foregroundStyle(.secondary)
                    Button("Try Again") { model.scan() }
                }
            default:
                // Below the toolbar, not under it: the page's header must stay visible
                if let page = model.page { WebView(url: page, model: model) } else { ScanningView() }
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Color(red: 0.047, green: 0.055, blue: 0.071))
        .toolbar {
            ToolbarItem {
                if model.updating {
                    ProgressView().controlSize(.small).help("Updating…")
                } else if let update = model.update {
                    Button { model.promptUpdate() } label: { Label("Update to \(update.version)", systemImage: "arrow.down.circle") }
                        .labelStyle(.titleAndIcon)
                        .help("Evict \(update.version) is available")
                }
            }
            ToolbarItem {
                Button { model.scan() } label: { Label("Rescan", systemImage: "arrow.clockwise") }
                    .disabled(model.isScanning)
                    .help("Rescan the disk (⌘R)")
            }
        }
    }
}

struct AccessView: View {
    @EnvironmentObject var model: Model

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Image(nsImage: NSApp.applicationIconImage).resizable().frame(width: 72, height: 72)
            Text("Give Evict Full Disk Access").font(.title.weight(.semibold))
            Text("Without it, macOS interrupts the scan with a series of permission prompts (Desktop, Documents, other apps’ data…) and part of the disk stays invisible.")
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            VStack(alignment: .leading, spacing: 6) {
                Text("1.  Open System Settings with the button below")
                Text("2.  Turn on Evict (or click + and choose Evict in Applications)")
                Text("3.  The scan starts on its own")
            }
            .font(.callout)
            HStack {
                Button("Open System Settings") { model.openFullDiskAccess() }
                    .buttonStyle(.borderedProminent)
                    .keyboardShortcut(.defaultAction)
                Button("Scan Anyway") { model.scan() }
            }
            .controlSize(.large)
            .padding(.top, 6)
        }
        .frame(maxWidth: 520)
        .padding(40)
        .task {
            while model.state == .needsAccess {
                try? await Task.sleep(for: .seconds(1))
                if model.state == .needsAccess && model.hasFullDiskAccess { model.scan() }
            }
        }
    }
}

struct ScanningView: View {
    @EnvironmentObject var model: Model

    var body: some View {
        VStack(spacing: 14) {
            ProgressView().controlSize(.large)
            Text("Scanning the disk…").font(.title2.weight(.semibold))
            if !model.hasFullDiskAccess {
                VStack(spacing: 8) {
                    Text("Without Full Disk Access, Mail, Messages and Safari aren’t counted.")
                        .font(.callout).foregroundStyle(.secondary)
                    Button("Open System Settings") { model.openFullDiskAccess() }
                }
                .padding(.top, 18)
            }
        }
        .padding(40)
    }
}

struct WebView: NSViewRepresentable {
    let url: URL
    let model: Model

    func makeCoordinator() -> Bridge { Bridge(model: model) }

    func makeNSView(context: Context) -> WKWebView {
        let config = WKWebViewConfiguration()
        config.userContentController.add(context.coordinator, name: "evict")
        let flags = "window.evictClaude = \(Model.claude != nil); window.evictLang = '\(Locale.preferredLanguages.first ?? "en")';"
        config.userContentController.addUserScript(WKUserScript(source: flags, injectionTime: .atDocumentStart, forMainFrameOnly: true))
        let view = EvictWebView(frame: .zero, configuration: config)
        view.setValue(false, forKey: "drawsBackground")  // no white flash before the page paints
        model.webView = view
        context.coordinator.loaded = url
        view.loadFileURL(url, allowingReadAccessTo: url.deletingLastPathComponent())
        return view
    }

    // Same web view from the live map to the final one
    func updateNSView(_ view: WKWebView, context: Context) {
        guard context.coordinator.loaded != url else { return }
        context.coordinator.loaded = url
        view.loadFileURL(url, allowingReadAccessTo: url.deletingLastPathComponent())
    }
}

// Splits a pipe's chunks into lines
final class LineBuffer: @unchecked Sendable {
    private var pending = Data()

    func lines(_ chunk: Data) -> [String] {
        pending.append(chunk)
        guard let end = pending.lastIndex(of: 10) else { return [] }
        let text = String(decoding: pending[..<end], as: UTF8.self)
        pending.removeSubrange(...end)
        return text.split(separator: "\n").map(String.init)
    }
}

// Quick Look: while it's the first responder, the web view feeds the shared preview panel
final class EvictWebView: WKWebView, QLPreviewPanelDataSource {
    private var items: [URL] = []

    func preview(_ urls: [URL]) {
        items = urls
        window?.makeFirstResponder(self)
        guard let panel = QLPreviewPanel.shared() else { return }
        panel.updateController()
        panel.reloadData()
        panel.makeKeyAndOrderFront(nil)
    }

    override func acceptsPreviewPanelControl(_ panel: QLPreviewPanel!) -> Bool { true }
    override func beginPreviewPanelControl(_ panel: QLPreviewPanel!) { panel.dataSource = self }
    override func endPreviewPanelControl(_ panel: QLPreviewPanel!) { panel.dataSource = nil }
    func numberOfPreviewItems(in panel: QLPreviewPanel!) -> Int { items.count }
    func previewPanel(_ panel: QLPreviewPanel!, previewItemAt index: Int) -> QLPreviewItem! { items[index] as NSURL }
}

final class Bridge: NSObject, WKScriptMessageHandler {
    let model: Model
    var loaded: URL?
    init(model: Model) { self.model = model }

    func userContentController(_ controller: WKUserContentController, didReceive message: WKScriptMessage) {
        guard let body = message.body as? [String: Any], let action = body["action"] as? String else { return }
        Task { @MainActor in
            switch action {
            case "copy":
                guard let text = body["text"] as? String else { return }
                NSPasteboard.general.clearContents()
                NSPasteboard.general.setString(text, forType: .string)
            case "reveal":
                let paths = body["paths"] as? [String] ?? [body["path"] as? String].compactMap { $0 }
                if !paths.isEmpty { NSWorkspace.shared.activateFileViewerSelecting(paths.map { URL(fileURLWithPath: $0) }) }
            case "delete":
                guard let path = body["path"] as? String else { return }
                model.confirmDelete([(path, (body["size"] as? NSNumber)?.int64Value ?? 0)])
            case "preview":
                guard let paths = body["paths"] as? [String], !paths.isEmpty else { return }
                (model.webView as? EvictWebView)?.preview(paths.map { URL(fileURLWithPath: $0) })
            case "open":
                guard let path = body["path"] as? String else { return }
                NSWorkspace.shared.open(URL(fileURLWithPath: path))
            case "archive":
                guard let id = body["id"] as? Int, let path = body["path"] as? String else { return }
                model.listArchive(id: id, path: path)
            case "dupes":
                guard let id = body["id"] as? Int, let groups = body["groups"] as? [[String]] else { return }
                model.findDuplicates(id: id, groups: groups)
            case "deleteMany":
                guard let items = body["items"] as? [[Any]] else { return }
                model.confirmDelete(items.compactMap { item in
                    (item.first as? String).map { ($0, (item.last as? NSNumber)?.int64Value ?? 0) }
                })
            case "emptyTrash":
                guard let path = body["path"] as? String else { return }
                model.confirmEmptyTrash(path: path, size: (body["size"] as? NSNumber)?.int64Value ?? 0)
            case "ask":
                guard let id = body["id"] as? Int, let prompt = body["prompt"] as? String else { return }
                model.ask(id: id, prompt: prompt)
            case "cancel":
                model.cancelAsk()
            default:
                break
            }
        }
    }
}

// Self-update from the latest GitHub release, which ships the DMG made by `./build.sh dmg`
enum Updater {
    struct Release: Decodable {
        struct Asset: Decodable {
            let name: String
            let browser_download_url: URL
        }
        let tag_name: String
        let body: String?
        let assets: [Asset]
        var version: String { tag_name.trimmingCharacters(in: CharacterSet(charactersIn: "vV")) }
        var dmg: URL? { assets.first { $0.name == "Evict.dmg" }?.browser_download_url }
    }

    static let site = URL(string: "https://vassilidev.github.io/evict/")!
    static var current: String { Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "0" }

    static func latest() async throws -> Release {
        var request = URLRequest(url: URL(string: "https://api.github.com/repos/vassilidev/evict/releases/latest")!)
        request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        let (data, response) = try await URLSession.shared.data(for: request)
        guard (response as? HTTPURLResponse)?.statusCode == 200 else { throw failure("GitHub isn’t responding, try again later.") }
        return try JSONDecoder().decode(Release.self, from: data)
    }

    static func isNewer(_ release: Release) -> Bool {
        release.version.compare(current, options: .numeric) == .orderedDescending
    }

    // Copies the app out of the downloaded DMG next to the running one, then swaps them atomically
    static func install(_ release: Release, over app: URL = Bundle.main.bundleURL) async throws {
        guard let url = release.dmg else { throw failure("Version \(release.version) has no DMG.") }
        let (dmg, _) = try await URLSession.shared.download(from: url)
        let mount = FileManager.default.temporaryDirectory.appendingPathComponent("evict-update-\(UUID().uuidString)")
        try run("/usr/bin/hdiutil", "attach", "-nobrowse", "-readonly", "-noautoopen", "-mountpoint", mount.path, dmg.path)
        defer {
            try? run("/usr/bin/hdiutil", "detach", "-quiet", "-force", mount.path)
            try? FileManager.default.removeItem(at: dmg)
        }
        let staged = app.deletingLastPathComponent().appendingPathComponent(".Evict-update.app")
        try? FileManager.default.removeItem(at: staged)
        try run("/usr/bin/ditto", mount.appendingPathComponent("Evict.app").path, staged.path)
        guard Bundle(url: staged)?.bundleIdentifier == Bundle.main.bundleIdentifier else {
            try? FileManager.default.removeItem(at: staged)
            throw failure("The downloaded file doesn’t contain Evict.")
        }
        _ = try FileManager.default.replaceItemAt(app, withItemAt: staged)
    }

    static func commandLine() -> Never {
        Task.detached {
            do {
                let release = try await latest()
                guard isNewer(release) else { print("Evict \(current) is up to date."); exit(0) }
                print("Updating \(current) → \(release.version)…")
                try await install(release)
                print("✓ Evict \(release.version) installed in \(Bundle.main.bundlePath)")
                exit(0)
            } catch {
                print("Update failed: \(error.localizedDescription)")
                exit(1)
            }
        }
        dispatchMain()
    }

    private static func run(_ tool: String, _ args: String...) throws {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: tool)
        process.arguments = args
        process.standardOutput = FileHandle.nullDevice
        try process.run()
        process.waitUntilExit()
        if process.terminationStatus != 0 {
            throw failure("\((tool as NSString).lastPathComponent) failed (code \(process.terminationStatus)).")
        }
    }

    private static func failure(_ message: String) -> NSError {
        NSError(domain: "Evict", code: 3, userInfo: [NSLocalizedDescriptionKey: message])
    }
}

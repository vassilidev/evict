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
        }
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
    enum State: Equatable { case idle, needsAccess, scanning(UInt64), ready(URL), failed(String) }
    @Published var state = State.idle
    @Published var update: Updater.Release?
    @Published var updating = false
    weak var webView: WKWebView?
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
        state = .scanning(0)
        guard let scanner = Bundle.main.url(forAuxiliaryExecutable: "evict-scan"),
              let template = Bundle.main.url(forResource: "index", withExtension: "html") else {
            state = .failed("App files are missing, please reinstall Evict.")
            return
        }
        Task {
            let result = await Task.detached(priority: .userInitiated) {
                Model.runScan(scanner: scanner, template: template) { count in
                    Task { @MainActor in if self.isScanning { self.state = .scanning(count) } }
                }
            }.value
            switch result {
            case .success(let url): state = .ready(url)
            case .failure(let error): state = .failed(error.localizedDescription)
            }
        }
    }

    nonisolated static func runScan(scanner: URL, template: URL, progress: @escaping @Sendable (UInt64) -> Void) -> Result<URL, Error> {
        _ = claude  // resolved during the scan rather than on the main thread when the page opens
        let process = Process(), out = Pipe(), err = Pipe()
        process.executableURL = scanner
        process.arguments = ["/"]
        process.environment = ["EVICT_PROGRESS": "1", "HOME": NSHomeDirectory()]
        process.standardOutput = out
        process.standardError = err
        // The scanner writes "\r<count> items…" to stderr while it runs
        err.fileHandleForReading.readabilityHandler = { handle in
            let text = String(decoding: handle.availableData, as: UTF8.self)
            if let last = text.split(separator: "\r").last, let count = UInt64(last.prefix(while: \.isNumber)) {
                progress(count)
            }
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
        if path.range(of: #"^/Users/[^/]+(/Library)?$"#, options: .regularExpression) != nil { return false }
        return true
    }

    func confirmDelete(path: String, size: Int64) {
        guard Model.canRemove(path) else { return }
        let alert = NSAlert()
        alert.alertStyle = .warning
        alert.messageText = "Delete “\((path as NSString).lastPathComponent)”?"
        alert.informativeText = "\(Model.bytes(size)) · \(path)\n\nThe Trash keeps it recoverable, but the space is only freed once the Trash is emptied."
        alert.addButton(withTitle: "Move to Trash")
        alert.addButton(withTitle: "Delete Permanently").hasDestructiveAction = true
        alert.addButton(withTitle: "Cancel")
        switch alert.runModal() {
        case .alertFirstButtonReturn: trash(path)
        case .alertSecondButtonReturn: delete(path)
        default: break
        }
    }

    private func trash(_ path: String) {
        do {
            try FileManager.default.trashItem(at: URL(fileURLWithPath: path), resultingItemURL: nil)
            removed(path, freed: false)
        } catch {
            offerAdmin(path, reason: error.localizedDescription)
        }
    }

    private func delete(_ path: String) {
        do {
            try FileManager.default.removeItem(atPath: path)
            removed(path, freed: true)
        } catch {
            offerAdmin(path, reason: error.localizedDescription)
        }
    }

    // Files owned by root or another user: retry with the native administrator password prompt
    private func offerAdmin(_ path: String, reason: String) {
        let alert = NSAlert()
        alert.messageText = "Permission denied"
        alert.informativeText = "\(reason)\n\nDelete permanently using an administrator password?"
        alert.addButton(withTitle: "Delete as Administrator").hasDestructiveAction = true
        alert.addButton(withTitle: "Cancel")
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        let literal = path.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"")
        let script = NSAppleScript(source: "do shell script \"/bin/rm -rf -- \" & quoted form of \"\(literal)\" with administrator privileges")
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
        removed(path, freed: true)
    }

    private func removed(_ path: String, freed: Bool) {
        guard let arg = try? String(data: JSONEncoder().encode(path), encoding: .utf8) else { return }
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

    func ask(id: Int, prompt: String) {
        Task {
            let (text, ok) = await Task.detached { Model.askClaude(prompt) }.value
            answer(id, text, ok)
        }
    }

    private func answer(_ id: Int, _ text: String, _ ok: Bool) {
        guard let arg = try? String(data: JSONEncoder().encode(text), encoding: .utf8) else { return }
        webView?.evaluateJavaScript("evictAnswer(\(id), \(arg), \(ok))")
    }

    nonisolated static func askClaude(_ prompt: String) -> (String, Bool) {
        guard let claude else { return ("Claude Code isn’t installed.", false) }
        let process = Process(), out = Pipe()
        process.executableURL = URL(fileURLWithPath: claude)
        // No tools at all: Claude can only answer, it can't read, run or delete anything
        process.arguments = ["-p", "--tools", "", "--no-session-persistence", "--append-system-prompt", "Reply in plain text, without Markdown.", prompt]
        process.currentDirectoryURL = FileManager.default.temporaryDirectory
        process.standardInput = FileHandle.nullDevice
        process.standardOutput = out
        process.standardError = out
        do {
            try process.run()
            let text = String(decoding: out.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
            process.waitUntilExit()
            return (text, process.terminationStatus == 0)
        } catch {
            return (error.localizedDescription, false)
        }
    }
}

struct ContentView: View {
    @EnvironmentObject var model: Model

    var body: some View {
        Group {
            switch model.state {
            case .needsAccess: AccessView()
            case .idle, .scanning: ScanningView()
            case .ready(let url): WebView(url: url, model: model).ignoresSafeArea()
            case .failed(let message):
                VStack(spacing: 12) {
                    Text("The scan failed").font(.title2.weight(.semibold))
                    Text(message).foregroundStyle(.secondary)
                    Button("Try Again") { model.scan() }
                }
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
            if case .scanning(let count) = model.state {
                Text("\(count.formatted(.number.locale(Locale(identifier: "en_US")))) items").monospacedDigit().foregroundStyle(.secondary)
            }
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
        let view = WKWebView(frame: .zero, configuration: config)
        view.setValue(false, forKey: "drawsBackground")  // no white flash before the page paints
        model.webView = view
        view.loadFileURL(url, allowingReadAccessTo: url.deletingLastPathComponent())
        return view
    }

    func updateNSView(_ view: WKWebView, context: Context) {}
}

final class Bridge: NSObject, WKScriptMessageHandler {
    let model: Model
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
                guard let path = body["path"] as? String else { return }
                NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: path)])
            case "delete":
                guard let path = body["path"] as? String else { return }
                model.confirmDelete(path: path, size: (body["size"] as? NSNumber)?.int64Value ?? 0)
            case "ask":
                guard let id = body["id"] as? Int, let prompt = body["prompt"] as? String else { return }
                model.ask(id: id, prompt: prompt)
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

import SwiftUI
import WebKit

@main
struct EvictApp: App {
    @StateObject private var model = Model()

    var body: some Scene {
        WindowGroup("Evict") {
            ContentView()
                .environmentObject(model)
                .frame(minWidth: 900, minHeight: 600)
                .preferredColorScheme(.dark)
                .onAppear { if model.state == .idle { model.start() } }
        }
        .commands {
            CommandGroup(after: .newItem) {
                Button("Rescanner") { model.scan() }.keyboardShortcut("r")
                Button("Accès complet au disque…") { model.openFullDiskAccess() }
            }
        }
    }
}

@MainActor
final class Model: ObservableObject {
    enum State: Equatable { case idle, needsAccess, scanning(UInt64), ready(URL), failed(String) }
    @Published var state = State.idle
    weak var webView: WKWebView?

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
            state = .failed("Fichiers de l'app introuvables, réinstalle Evict.")
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
        let process = Process(), out = Pipe(), err = Pipe()
        process.executableURL = scanner
        process.arguments = ["/"]
        process.environment = ["EVICT_PROGRESS": "1", "HOME": NSHomeDirectory()]
        process.standardOutput = out
        process.standardError = err
        // The scanner writes "\r<count> éléments…" to stderr while it runs
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
                throw NSError(domain: "Evict", code: 1, userInfo: [NSLocalizedDescriptionKey: "Le scanner s'est arrêté (code \(process.terminationStatus))."])
            }
            let html = try String(contentsOf: template, encoding: .utf8)
            guard let slot = html.range(of: "window.DATA = null; /* __DATA__ */") else {
                throw NSError(domain: "Evict", code: 2, userInfo: [NSLocalizedDescriptionKey: "Modèle de page invalide."])
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
        alert.messageText = "Supprimer « \((path as NSString).lastPathComponent) » ?"
        alert.informativeText = "\(ByteCountFormatter.string(fromByteCount: size, countStyle: .file)) · \(path)\n\nLa corbeille permet de récupérer l'élément, mais l'espace n'est libéré qu'une fois la corbeille vidée."
        alert.addButton(withTitle: "Mettre à la corbeille")
        alert.addButton(withTitle: "Supprimer définitivement").hasDestructiveAction = true
        alert.addButton(withTitle: "Annuler")
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
        alert.messageText = "Permission refusée"
        alert.informativeText = "\(reason)\n\nSupprimer définitivement avec le mot de passe administrateur ?"
        alert.addButton(withTitle: "Supprimer en administrateur").hasDestructiveAction = true
        alert.addButton(withTitle: "Annuler")
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        let literal = path.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"")
        let script = NSAppleScript(source: "do shell script \"/bin/rm -rf -- \" & quoted form of \"\(literal)\" with administrator privileges")
        var error: NSDictionary?
        script?.executeAndReturnError(&error)
        if let error {
            if (error[NSAppleScript.errorNumber] as? Int) == -128 { return }  // cancelled by the user
            let fail = NSAlert()
            fail.messageText = "Suppression impossible"
            fail.informativeText = error[NSAppleScript.errorMessage] as? String ?? "Erreur inconnue"
            fail.runModal()
            return
        }
        removed(path, freed: true)
    }

    private func removed(_ path: String, freed: Bool) {
        guard let arg = try? String(data: JSONEncoder().encode(path), encoding: .utf8) else { return }
        webView?.evaluateJavaScript("evictRemoved(\(arg), \(freed))")
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
                    Text("Le scan a échoué").font(.title2.weight(.semibold))
                    Text(message).foregroundStyle(.secondary)
                    Button("Réessayer") { model.scan() }
                }
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Color(red: 0.047, green: 0.055, blue: 0.071))
        .toolbar {
            ToolbarItem {
                Button { model.scan() } label: { Label("Rescanner", systemImage: "arrow.clockwise") }
                    .disabled(model.isScanning)
                    .help("Rescanner le disque (⌘R)")
            }
        }
    }
}

struct AccessView: View {
    @EnvironmentObject var model: Model

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Image(nsImage: NSApp.applicationIconImage).resizable().frame(width: 72, height: 72)
            Text("Donne l'Accès complet au disque à Evict").font(.title.weight(.semibold))
            Text("Sans cet accès, macOS interrompt le scan avec une série de demandes d'autorisation (Bureau, Documents, données des autres apps…) et une partie du disque reste invisible.")
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            VStack(alignment: .leading, spacing: 6) {
                Text("1.  Ouvre les réglages avec le bouton ci-dessous")
                Text("2.  Active Evict (ou clique sur + et choisis Evict dans Applications)")
                Text("3.  Le scan démarre tout seul")
            }
            .font(.callout)
            HStack {
                Button("Ouvrir les réglages") { model.openFullDiskAccess() }
                    .buttonStyle(.borderedProminent)
                    .keyboardShortcut(.defaultAction)
                Button("Scanner quand même") { model.scan() }
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
            Text("Scan du disque…").font(.title2.weight(.semibold))
            if case .scanning(let count) = model.state {
                Text("\(count.formatted()) éléments").monospacedDigit().foregroundStyle(.secondary)
            }
            if !model.hasFullDiskAccess {
                VStack(spacing: 8) {
                    Text("Sans l'Accès complet au disque, Mail, Messages et Safari ne sont pas comptés.")
                        .font(.callout).foregroundStyle(.secondary)
                    Button("Ouvrir les réglages") { model.openFullDiskAccess() }
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
            case "delete":
                guard let path = body["path"] as? String else { return }
                model.confirmDelete(path: path, size: (body["size"] as? NSNumber)?.int64Value ?? 0)
            default:
                break
            }
        }
    }
}

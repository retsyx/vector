import AppKit
import SwiftUI

@main
struct KindleUploaderApp: App {
    var body: some Scene {
        Window("Kindle Uploader", id: "uploader") {
            UploaderView()
                .frame(minWidth: 520, minHeight: 480)
        }
        .defaultSize(width: 580, height: 520)
    }
}

@MainActor
private final class UploaderState: ObservableObject {
    @Published var files: [URL] = []
    @Published var selection: Set<URL> = []
    @Published var folder = "documents"
    @Published var output = ""
    @Published var running = false
    @Published var failed = false
}

private struct UploaderView: View {
    @StateObject private var state = UploaderState()

    private var destination: String? {
        let path = state.folder.trimmingCharacters(in: CharacterSet(charactersIn: "/"))
        let parts = path.components(separatedBy: "/")
        guard !parts.contains(where: { $0.isEmpty || $0 == "." || $0 == ".." }),
              !path.contains("\0") else { return nil }
        return path
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack(spacing: 12) {
                if let iconURL = Bundle.main.url(forResource: "AppIcon", withExtension: "icns"),
                   let icon = NSImage(contentsOf: iconURL) {
                    Image(nsImage: icon)
                        .resizable()
                        .scaledToFit()
                        .frame(width: 48, height: 48)
                        .accessibilityHidden(true)
                }
                VStack(alignment: .leading, spacing: 3) {
                    Text("Kindle Uploader").font(.title2.bold())
                    Text("Copy files to your Kindle over USB.")
                        .foregroundStyle(.secondary)
                }
            }

            TextField("Kindle folder", text: $state.folder)
                .disabled(state.running)
                .help("Enter an existing folder, such as documents/manga-viewer.")

            VStack(spacing: 0) {
                List(state.files, id: \.self, selection: $state.selection) { file in
                    Label(file.lastPathComponent, systemImage: "doc")
                        .help(file.path)
                }
                .overlay {
                    if state.files.isEmpty {
                        Text("Choose the files you want to copy.")
                            .foregroundStyle(.secondary)
                            .allowsHitTesting(false)
                    }
                }
                Divider()
                HStack {
                    Button("Add Files…", action: chooseFiles)
                        .keyboardShortcut("o", modifiers: .command)
                    Button("Remove") {
                        state.files.removeAll { state.selection.contains($0) }
                        state.selection.removeAll()
                    }
                    .disabled(state.selection.isEmpty)
                    Spacer()
                    Text("\(state.files.count) selected for upload")
                        .foregroundStyle(.secondary)
                }
                .padding(8)
            }
            .background(.background)
            .clipShape(.rect(cornerRadius: 8))
            .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(.quaternary))
            .disabled(state.running)

            if !state.output.isEmpty {
                ScrollView {
                    Text(state.output)
                        .foregroundStyle(state.failed ? Color.red : Color.secondary)
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                .frame(height: 80)
                .padding(8)
                .background(.quaternary.opacity(0.45), in: .rect(cornerRadius: 8))
                .accessibilityLabel("Transfer status")
            }

            HStack {
                Button("Check Connection") { startTransfer(checkOnly: true) }
                    .disabled(state.running || destination == nil)
                Spacer()
                if state.running {
                    ProgressView().controlSize(.small)
                        .accessibilityLabel("USB operation in progress")
                }
                Button("Upload") { startTransfer(checkOnly: false) }
                    .keyboardShortcut(.defaultAction)
                    .buttonStyle(.borderedProminent)
                    .disabled(state.running || state.files.isEmpty || destination == nil)
            }
        }
        .padding(20)
    }

    private func chooseFiles() {
        let picker = NSOpenPanel()
        picker.title = "Choose Files for Kindle"
        picker.prompt = "Add"
        picker.allowsMultipleSelection = true
        picker.canChooseDirectories = false
        picker.begin { response in
            guard response == .OK else { return }
            for file in picker.urls where !state.files.contains(file) {
                state.files.append(file)
            }
        }
    }

    private func startTransfer(checkOnly: Bool) {
        guard let destination, !state.running else { return }
        guard let helper = Bundle.main.url(forAuxiliaryExecutable: "kindle-transfer") else {
            state.failed = true
            state.output = "The USB transfer helper is missing from the app."
            return
        }
        let arguments = [destination] + (checkOnly ? [] : state.files.map(\.path))
        state.output = "Connecting to Kindle…"
        state.failed = false
        state.running = true

        DispatchQueue.global(qos: .userInitiated).async {
            let process = Process()
            let pipe = Pipe()
            process.executableURL = helper
            process.arguments = arguments
            process.standardOutput = pipe
            process.standardError = pipe

            do {
                try process.run()
                // Drain the pipe before waiting so a large batch cannot block the helper.
                let data = pipe.fileHandleForReading.readDataToEndOfFile()
                process.waitUntilExit()
                let message = String(decoding: data, as: UTF8.self)
                    .trimmingCharacters(in: .whitespacesAndNewlines)
                let success = process.terminationReason == .exit && process.terminationStatus == 0
                DispatchQueue.main.async {
                    finish(message.isEmpty ? (success ? "Done." : "The USB operation failed.") : message,
                           success: success)
                }
            } catch {
                let message = error.localizedDescription
                DispatchQueue.main.async { finish(message, success: false) }
            }
        }
    }

    private func finish(_ message: String, success: Bool) {
        state.output = message
        state.failed = !success
        state.running = false
    }
}

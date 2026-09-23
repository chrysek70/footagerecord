import AppKit
import SwiftUI
import RecordingCore

@main
struct FootageRecordApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @State private var recorder = Recorder()

    var body: some Scene {
        Window("Footage Record", id: "recorder") {
            RecorderView(recorder: recorder)
                .background(WindowConnection(recorder: recorder, delegate: delegate))
        }
        .windowResizability(.contentSize)
        .defaultPosition(.center)
        .commands {
            CommandGroup(replacing: .newItem) { }
            CommandMenu("Recording") {
                Button("Show Footage Record") { delegate.showMainWindow() }
                    .keyboardShortcut("0", modifiers: .command)
                Button("Stop Recording") { recorder.stop() }
                    .keyboardShortcut(".", modifiers: .command)
                    .disabled(recorder.phase != .recording && recorder.phase != .starting)
            }
        }

        MenuBarExtra {
            Text(recorder.phase == .recording ? "Recording · \(RecordingFiles.elapsed(recorder.elapsed))" : "Footage Record")
            if recorder.phase == .recording || recorder.phase == .starting {
                Button("Stop Recording") { recorder.stop() }
                    .keyboardShortcut(".", modifiers: .command)
            }
            if case .countdown = recorder.phase {
                Button("Cancel Countdown") { recorder.cancelCountdown() }
            }
            Button("Show Footage Record") { delegate.showMainWindow() }
            if recorder.savedURL != nil {
                Button("Show Last Recording in Finder") { recorder.revealSaved() }
            }
            Divider()
            Button("Quit Footage Record") { NSApp.terminate(nil) }
                .keyboardShortcut("q")
        } label: {
            Image(systemName: recorder.phase.isBusy ? "record.circle.fill" : "record.circle")
                .accessibilityLabel(recorder.phase.isBusy ? "Footage Record is recording" : "Footage Record")
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    weak var mainWindow: NSWindow?
    weak var recorder: Recorder?

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
    }

    func showMainWindow() {
        mainWindow?.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        showMainWindow()
        return true
    }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard let recorder else { return .terminateNow }
        if recorder.phase == .choosing { recorder.cancelSelection() }
        guard recorder.phase.isBusy else { return .terminateNow }
        if case .countdown = recorder.phase {
            recorder.cancelCountdown()
            return .terminateNow
        }
        showMainWindow()
        let alert = NSAlert()
        alert.messageText = "Finish your recording first"
        alert.informativeText = "Stop the recording and wait for it to save before quitting."
        alert.addButton(withTitle: "OK")
        alert.runModal()
        return .terminateCancel
    }
}

private struct WindowConnection: NSViewRepresentable {
    let recorder: Recorder
    let delegate: AppDelegate

    func makeNSView(context: Context) -> NSView { NSView() }

    func updateNSView(_ nsView: NSView, context: Context) {
        DispatchQueue.main.async { [delegate] in
            guard let window = nsView.window else { return }
            delegate.mainWindow = window
            delegate.recorder = recorder
            window.isReleasedWhenClosed = false
            recorder.showWindow = { [weak delegate] in delegate?.showMainWindow() }
            recorder.hideWindow = { [weak window] in window?.orderOut(nil) }
        }
    }
}

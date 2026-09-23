import SwiftUI
import AppKit
import RecordingCore

struct RecorderView: View {
    @Bindable var recorder: Recorder
    @State private var showingSettings = false
    private let accent = Color(red: 0.83, green: 0.17, blue: 0.19)

    var body: some View {
        VStack(alignment: .leading, spacing: 24) {
            HStack(spacing: 9) {
                Image(systemName: "record.circle")
                    .font(.system(size: 23, weight: .medium))
                    .foregroundStyle(accent)
                Text("Footage Record").font(.system(size: 15, weight: .semibold))
                Spacer()
                Button { showingSettings = true } label: {
                    Image(systemName: "gearshape").font(.system(size: 15))
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Settings")
                .help("Settings")
                .disabled(recorder.phase.isBusy || recorder.phase == .choosing)
            }

            if recorder.phase == .saved {
                savedContent
            } else if recorder.phase.isBusy {
                recordingContent
            } else {
                readyContent
            }

            if let message = recorder.message {
                VStack(alignment: .leading, spacing: 8) {
                    Label(message, systemImage: "exclamationmark.circle")
                        .font(.callout)
                        .fixedSize(horizontal: false, vertical: true)
                    HStack {
                        if let file = recorder.unfinishedURL {
                            Button("Show unfinished file") {
                                NSWorkspace.shared.activateFileViewerSelecting([file])
                            }
                        }
                        Spacer()
                        Button("Dismiss") { recorder.dismissMessage() }
                    }
                    .font(.caption)
                }
                .padding(12)
                .background(Color.orange.opacity(0.09), in: RoundedRectangle(cornerRadius: 10))
            }

            Divider()
            HStack(spacing: 6) {
                Image(systemName: "internaldrive")
                Text("Saved on your Mac. Only on your Mac.")
            }
            .font(.system(size: 11))
            .foregroundStyle(.secondary)
        }
        .padding(28)
        .frame(width: 430)
        .background(Color(nsColor: .windowBackgroundColor))
        .sheet(isPresented: $showingSettings) { settingsContent }
    }

    private var readyContent: some View {
        VStack(alignment: .leading, spacing: 20) {
            VStack(alignment: .leading, spacing: 6) {
                Text("A recording.\nWithout the fuss.")
                    .font(.system(size: 29, weight: .semibold, design: .rounded))
                    .tracking(-0.7)
                    .fixedSize(horizontal: false, vertical: true)
                Text("Choose what to show. We’ll save the video.")
                    .font(.system(size: 13))
                    .foregroundStyle(.secondary)
            }

            HStack(spacing: 10) {
                sourceButton("Window", icon: "macwindow", display: false)
                sourceButton("Screen", icon: "display", display: true)
            }
            .disabled(recorder.phase == .choosing)

            if let source = recorder.sourceName {
                Label(source, systemImage: "checkmark.circle.fill")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .lineLimit(2)
                    .accessibilityLabel("Selected source: \(source)")
            } else if recorder.phase == .choosing {
                HStack(spacing: 8) {
                    ProgressView().controlSize(.small)
                    Text(recorder.isDisplay
                         ? "Move your pointer to the screen you want to record, then click Share."
                         : "Move your pointer over the window you want to record, then click Share This Window.")
                        .font(.callout)
                        .fixedSize(horizontal: false, vertical: true)
                    Spacer()
                    Button("Cancel", action: recorder.cancelSelection)
                        .buttonStyle(.link).keyboardShortcut(.cancelAction)
                }
            }

            VStack(spacing: 16) {
                audioToggle(
                    title: "Microphone",
                    subtitle: recorder.requestingMicrophone ? "Waiting for permission…" : "Add your voice.",
                    icon: "mic",
                    binding: Binding(get: { recorder.microphoneEnabled }, set: { recorder.setMicrophone($0) })
                )
                .disabled(recorder.requestingMicrophone)
                audioToggle(
                    title: recorder.audioTitle,
                    subtitle: recorder.audioDetail,
                    icon: "speaker.wave.2",
                    binding: $recorder.systemAudioEnabled
                )
            }

            Button(action: recorder.start) {
                Label("Record", systemImage: "record.circle.fill")
                    .font(.system(size: 15, weight: .semibold))
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 8)
            }
            .buttonStyle(.borderedProminent)
            .tint(accent)
            .controlSize(.large)
            .disabled(!recorder.canRecord)
            .keyboardShortcut(.return, modifiers: [])
            .help(recorder.sourceName == nil ? "Choose a window or screen first" : "Start recording")

            HStack(alignment: .firstTextBaseline) {
                Text("MP4 · \(recorder.quality.summary) · 30 fps")
                Spacer()
                Button("Save location") { recorder.chooseDestination() }
                    .buttonStyle(.link)
                    .help(recorder.destination.path)
            }
            .font(.system(size: 11))
            .foregroundStyle(.secondary)
        }
    }

    private func sourceButton(_ title: String, icon: String, display: Bool) -> some View {
        Button { recorder.chooseSource(display: display) } label: {
            VStack(spacing: 9) {
                Image(systemName: icon).font(.system(size: 25, weight: .light))
                Text(title).font(.system(size: 13, weight: .medium))
            }
            .frame(maxWidth: .infinity)
            .padding(.vertical, 17)
            .background(Color.primary.opacity(0.035), in: RoundedRectangle(cornerRadius: 12))
            .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(Color.primary.opacity(0.1)))
        }
        .buttonStyle(.plain)
        .accessibilityLabel("Choose \(title.lowercased()) to record")
    }

    private func audioToggle(title: String, subtitle: String, icon: String, binding: Binding<Bool>) -> some View {
        HStack(spacing: 12) {
            Image(systemName: icon).font(.system(size: 17)).frame(width: 24)
                .foregroundStyle(.secondary)
            VStack(alignment: .leading, spacing: 3) {
                Text(title).font(.system(size: 13, weight: .medium))
                Text(subtitle).font(.system(size: 11)).foregroundStyle(.secondary)
            }
            Spacer(minLength: 4)
            Toggle(title, isOn: binding).labelsHidden().toggleStyle(.switch)
                .controlSize(.small)
                .accessibilityLabel(title)
        }
    }

    private var recordingContent: some View {
        VStack(spacing: 18) {
            switch recorder.phase {
            case .countdown(let seconds):
                Text("Get ready").font(.title3)
                Text("\(seconds)").font(.system(size: 68, weight: .light, design: .rounded))
                    .monospacedDigit()
                Button("Cancel", action: recorder.cancelCountdown).keyboardShortcut(.cancelAction)
            case .starting:
                ProgressView()
                Text("Starting your recording…").font(.headline)
                Button("Stop", action: recorder.stop)
            case .stopping:
                ProgressView()
                Text("Finishing your video…").font(.headline)
                Text("Wait for the file to finish saving.").font(.callout).foregroundStyle(.secondary)
            default:
                Label("Recording", systemImage: "record.circle.fill").foregroundStyle(accent)
                Text(RecordingFiles.elapsed(recorder.elapsed))
                    .font(.system(size: 48, weight: .light, design: .rounded)).monospacedDigit()
                Text("Stop anytime from the menu bar.")
                    .font(.callout).foregroundStyle(.secondary)
                Button(action: recorder.stop) {
                    Label("Stop recording", systemImage: "stop.fill")
                        .frame(maxWidth: .infinity).padding(.vertical, 8)
                }
                .buttonStyle(.borderedProminent).tint(accent).controlSize(.large)
            }
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 24)
    }

    private var savedContent: some View {
        VStack(alignment: .leading, spacing: 18) {
            Image(systemName: "checkmark.circle").font(.system(size: 36, weight: .light))
                .foregroundStyle(.green)
            Text("That’s a wrap.").font(.system(size: 29, weight: .semibold, design: .rounded))
            VStack(alignment: .leading, spacing: 5) {
                Text(recorder.savedURL?.lastPathComponent ?? "Recording saved")
                    .font(.callout).textSelection(.enabled)
                Text("\(RecordingFiles.elapsed(recorder.elapsed)) · \(ByteCountFormatter.string(fromByteCount: recorder.savedBytes, countStyle: .file))")
                    .font(.caption).foregroundStyle(.secondary)
            }
            HStack {
                Button("Open video", action: recorder.openSaved).buttonStyle(.borderedProminent)
                Button("Show in Finder", action: recorder.revealSaved)
            }
            .controlSize(.large)
            Button("Record another", action: recorder.recordAnother).buttonStyle(.link)
        }
    }

    private var settingsContent: some View {
        VStack(alignment: .leading, spacing: 22) {
            Text("Keep it simple.").font(.title2.weight(.semibold))
            VStack(alignment: .leading, spacing: 6) {
                Text("Video size").font(.headline)
                Picker("Video size", selection: $recorder.quality) {
                    ForEach(RecordingQuality.allCases) { quality in
                        Text(quality.title).tag(quality)
                    }
                }
                .pickerStyle(.segmented).labelsHidden()
                Text(recorder.quality.detail).font(.caption).foregroundStyle(.secondary)
            }
            Toggle("Start recording as soon as I choose", isOn: $recorder.startsAfterChoosing)
            Toggle("Three-second countdown", isOn: $recorder.countdownEnabled)
            Toggle("Show cursor in the recording", isOn: $recorder.showsCursor)
            VStack(alignment: .leading, spacing: 8) {
                Text("Save recordings to").font(.headline)
                Text(recorder.destination.path).font(.caption).foregroundStyle(.secondary)
                    .textSelection(.enabled).lineLimit(3)
                Button("Change folder…", action: recorder.chooseDestination)
            }
            Divider()
            Text("Footage Record 0.1 · Development preview\nNo account. No uploads. No tracking.")
                .font(.caption).foregroundStyle(.secondary)
            HStack { Spacer(); Button("Done") { showingSettings = false }.keyboardShortcut(.defaultAction) }
        }
        .padding(28)
        .frame(width: 390)
    }
}

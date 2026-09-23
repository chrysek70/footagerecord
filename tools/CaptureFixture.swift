// A separate, synthetic window for manual end-to-end capture checks.
// Never records or accesses the microphone. Tone is generated in memory.
import AppKit
import AVFoundation
import SwiftUI

@MainActor
final class Tone {
    let engine = AVAudioEngine()
    let player = AVAudioPlayerNode()

    init() throws {
        guard let format = AVAudioFormat(standardFormatWithSampleRate: 48_000, channels: 2),
              let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: 48_000),
              let channels = buffer.floatChannelData else { return }
        buffer.frameLength = 48_000
        for i in 0..<48_000 {
            let sample = Float(sin(Double(i) * 2 * .pi * 440 / 48_000)) * 0.035
            channels[0][i] = sample
            channels[1][i] = sample
        }
        engine.attach(player)
        engine.connect(player, to: engine.mainMixerNode, format: format)
        try engine.start()
        player.scheduleBuffer(buffer, at: nil, options: .loops)
    }
}

struct FixtureView: View {
    let tone: Tone
    @State private var playing = false
    let start = Date.now
    var body: some View {
        VStack(alignment: .leading, spacing: 24) {
            Text("FOOTAGE RECORD / CAPTURE CHECK").font(.caption.monospaced()).foregroundStyle(.secondary)
            Text("A little motion.\nA clear picture.").font(.system(size: 44, weight: .semibold))
            TimelineView(.animation(minimumInterval: 1.0 / 30)) { context in
                let time = context.date.timeIntervalSince(start)
                ZStack(alignment: .leading) {
                    RoundedRectangle(cornerRadius: 20).fill(Color.indigo.opacity(0.15))
                    Circle().fill(.indigo).frame(width: 70, height: 70)
                        .offset(x: 20 + (sin(time * 1.5) + 1) * 230)
                }
                .frame(height: 110)
                Text(String(format: "Elapsed %.1f seconds", time)).monospacedDigit()
            }
            Text("Readability check: 0123456789 · Aa Bb Cc · The quick brown fox.")
                .font(.system(size: 14))
            HStack {
                Button(playing ? "Stop test tone" : "Play 440 Hz test tone") {
                    playing.toggle()
                    if playing { tone.player.play() } else { tone.player.pause() }
                }
                Text("Generated locally. No microphone.").font(.caption).foregroundStyle(.secondary)
            }
        }
        .padding(36)
        .frame(width: 620, height: 450)
        .background(Color(nsColor: .windowBackgroundColor))
    }
}

let app = NSApplication.shared
app.setActivationPolicy(.regular)
let tone = try Tone()
let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 620, height: 450),
    styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
window.title = "Footage Capture Fixture"
window.contentView = NSHostingView(rootView: FixtureView(tone: tone))
window.center()
window.makeKeyAndOrderFront(nil)
app.activate(ignoringOtherApps: true)
app.run()

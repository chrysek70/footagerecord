import AppKit
import AVFoundation
import Observation
import RecordingCore
// The Objective-C picker passes an SDK-owned filter without Sendable annotations.
// We hand it to the main actor once and never mutate it from callback threads.
@preconcurrency import ScreenCaptureKit

enum RecorderPhase: Equatable {
    case ready, choosing, countdown(Int), starting, recording, stopping, saved

    var isBusy: Bool {
        switch self {
        case .countdown, .starting, .recording, .stopping: true
        default: false
        }
    }
}

@Observable
final class Recorder: NSObject {
    private(set) var phase: RecorderPhase = .ready
    private(set) var sourceName: String?
    private(set) var isDisplay = false
    private(set) var microphoneEnabled = false
    private(set) var requestingMicrophone = false
    private(set) var savedURL: URL?
    private(set) var savedBytes: Int64 = 0
    private(set) var elapsed: TimeInterval = 0
    private(set) var message: String?
    private(set) var unfinishedURL: URL?
    var systemAudioEnabled: Bool {
        didSet { UserDefaults.standard.set(systemAudioEnabled, forKey: "systemAudio") }
    }
    var quality: RecordingQuality {
        didSet { UserDefaults.standard.set(quality.rawValue, forKey: "quality") }
    }
    var countdownEnabled: Bool {
        didSet { UserDefaults.standard.set(countdownEnabled, forKey: "countdown") }
    }
    var showsCursor: Bool {
        didSet { UserDefaults.standard.set(showsCursor, forKey: "cursor") }
    }
    var startsAfterChoosing: Bool {
        didSet { UserDefaults.standard.set(startsAfterChoosing, forKey: "startsAfterChoosing") }
    }
    private(set) var destination: URL
    var showWindow: (() -> Void)?
    var hideWindow: (() -> Void)?

    private var filter: SCContentFilter?
    private var stream: SCStream?
    private var output: SCRecordingOutput?
    private var pendingURL: URL?
    private var startedAt: Date?
    private var countdownTask: Task<Void, Never>?
    private var clockTask: Task<Void, Never>?
    private var finalizationTask: Task<Void, Never>?
    private var stopWatchdog: Task<Void, Never>?
    private var activity: NSObjectProtocol?

    override init() {
        let defaults = UserDefaults.standard
        defaults.register(defaults: ["systemAudio": true, "countdown": true, "cursor": true, "startsAfterChoosing": true])
        systemAudioEnabled = defaults.bool(forKey: "systemAudio")
        countdownEnabled = defaults.bool(forKey: "countdown")
        showsCursor = defaults.bool(forKey: "cursor")
        startsAfterChoosing = defaults.bool(forKey: "startsAfterChoosing")
        quality = RecordingQuality(rawValue: defaults.string(forKey: "quality") ?? "") ?? .balanced
        if let path = defaults.string(forKey: "destination") {
            destination = URL(fileURLWithPath: path, isDirectory: true)
        } else {
            destination = FileManager.default.urls(for: .moviesDirectory, in: .userDomainMask)[0]
                .appendingPathComponent("Footage Record", isDirectory: true)
        }
        super.init()
        microphoneEnabled = defaults.bool(forKey: "microphone")
            && AVCaptureDevice.authorizationStatus(for: .audio) == .authorized
        SCContentSharingPicker.shared.add(self)
    }

    var canRecord: Bool { filter != nil && !phase.isBusy && phase != .choosing && !requestingMicrophone }
    var audioTitle: String { sourceName == nil ? "Screen / app audio" : isDisplay ? "Computer audio" : "App audio" }
    var audioDetail: String {
        sourceName == nil ? "Sound from what you choose to record." : isDisplay ? "Sound from apps on this screen." : "Sound from the selected window’s app."
    }

    func chooseSource(display: Bool) {
        guard !phase.isBusy else { return }
        message = nil
        isDisplay = display
        phase = .choosing
        var configuration = SCContentSharingPickerConfiguration()
        configuration.allowedPickerModes = display ? [.singleDisplay] : [.singleWindow]
        configuration.allowsChangingSelectedContent = false
        let picker = SCContentSharingPicker.shared
        picker.configuration = configuration
        picker.maximumStreamCount = 1
        picker.isActive = true
        picker.present()
    }

    func cancelSelection() {
        guard phase == .choosing else { return }
        SCContentSharingPicker.shared.isActive = false
        filter = nil
        sourceName = nil
        phase = .ready
    }

    func setMicrophone(_ enabled: Bool) {
        guard !phase.isBusy, !requestingMicrophone else { return }
        if !enabled {
            microphoneEnabled = false
            UserDefaults.standard.set(false, forKey: "microphone")
            return
        }
        requestingMicrophone = true
        Task {
            let allowed = await AVCaptureDevice.requestAccess(for: .audio)
            requestingMicrophone = false
            microphoneEnabled = allowed
            UserDefaults.standard.set(allowed, forKey: "microphone")
            if !allowed {
                message = "Microphone access is off. You can record without it, or enable Footage Record in System Settings → Privacy & Security → Microphone."
            }
        }
    }

    func chooseDestination() {
        guard !phase.isBusy else { return }
        let panel = NSOpenPanel()
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.canCreateDirectories = true
        panel.allowsMultipleSelection = false
        panel.prompt = "Save recordings here"
        panel.directoryURL = destination
        guard panel.runModal() == .OK, let url = panel.url else { return }
        destination = url
        UserDefaults.standard.set(url.path, forKey: "destination")
    }

    func start() {
        guard canRecord else { return }
        message = nil
        savedURL = nil
        unfinishedURL = nil
        elapsed = 0
        phase = .countdown(countdownEnabled ? 3 : 0)
        countdownTask = Task {
            do {
                if countdownEnabled {
                    for remaining in stride(from: 3, through: 1, by: -1) {
                        phase = .countdown(remaining)
                        try await Task.sleep(for: .seconds(1))
                    }
                }
                try Task.checkCancellation()
                await beginCapture()
            } catch is CancellationError {
                // cancelCountdown sets the state synchronously.
            } catch {
                message = error.localizedDescription
                phase = .ready
            }
        }
    }

    func cancelCountdown() {
        guard case .countdown = phase else { return }
        countdownTask?.cancel()
        countdownTask = nil
        phase = .ready
    }

    private func beginCapture() async {
        guard let filter else { phase = .ready; return }
        phase = .starting
        do {
            try FileManager.default.createDirectory(at: destination, withIntermediateDirectories: true)
            guard FileManager.default.isWritableFile(atPath: destination.path) else {
                throw RecorderError.destinationNotWritable
            }
            let space = try destination.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
            if let available = space.volumeAvailableCapacityForImportantUsage, available < 250_000_000 {
                throw RecorderError.lowDiskSpace
            }
            if microphoneEnabled && AVCaptureDevice.authorizationStatus(for: .audio) != .authorized {
                microphoneEnabled = false
                throw RecorderError.microphonePermissionChanged
            }
            guard let size = PixelSize.fitting(
                width: filter.contentRect.width * Double(filter.pointPixelScale),
                height: filter.contentRect.height * Double(filter.pointPixelScale),
                within: quality.maximumSize
            ) else { throw RecorderError.invalidSource }

            let configuration = SCStreamConfiguration()
            configuration.width = size.width
            configuration.height = size.height
            configuration.minimumFrameInterval = CMTime(value: 1, timescale: 30)
            // Keep the default queue depth: 3 starves SCRecordingOutput on display capture after ~4 frames.
            configuration.showsCursor = showsCursor
            configuration.scalesToFit = false
            configuration.preservesAspectRatio = true
            configuration.captureDynamicRange = .SDR
            configuration.capturesAudio = systemAudioEnabled
            configuration.excludesCurrentProcessAudio = true
            configuration.captureMicrophone = microphoneEnabled
            configuration.sampleRate = 48_000
            configuration.channelCount = 2
            configuration.streamName = "Footage Record"

            let temporary = destination.appendingPathComponent(".Footage-\(UUID().uuidString).incomplete.mp4")
            pendingURL = temporary
            let recordingConfiguration = SCRecordingOutputConfiguration()
            recordingConfiguration.outputURL = temporary
            recordingConfiguration.videoCodecType = size.fitsH264 ? .h264 : .hevc
            recordingConfiguration.outputFileType = .mp4
            let recordingOutput = SCRecordingOutput(configuration: recordingConfiguration, delegate: self)
            let captureStream = SCStream(filter: filter, configuration: configuration, delegate: self)
            output = recordingOutput
            stream = captureStream
            try captureStream.addRecordingOutput(recordingOutput)
            activity = ProcessInfo.processInfo.beginActivity(
                options: [.userInitiated, .idleSystemSleepDisabled], reason: "Recording your selected screen content"
            )
            hideWindow?()
            try await captureStream.startCapture()
            // The output delegate, not this method, confirms recording has actually begun.
            if phase == .starting { scheduleStartWatchdog() }
        } catch {
            await failCapture(error.localizedDescription)
        }
    }

    func stop() {
        guard phase == .recording || phase == .starting, let stream else { return }
        phase = .stopping
        clockTask?.cancel()
        scheduleStopWatchdog()
        let identity = ObjectIdentifier(stream)
        Task {
            do {
                try await stream.stopCapture()
                // Saving is completed only by recordingOutputDidFinishRecording.
            } catch {
                guard self.stream.map(ObjectIdentifier.init) == identity else { return }
                await failCapture("Couldn’t finish the recording: \(error.localizedDescription)")
            }
        }
    }

    func recordAnother() {
        guard !phase.isBusy else { return }
        savedURL = nil
        message = nil
        phase = .ready
    }

    func dismissMessage() { message = nil }

    func revealSaved() {
        if let savedURL { NSWorkspace.shared.activateFileViewerSelecting([savedURL]) }
    }

    func openSaved() {
        if let savedURL { NSWorkspace.shared.open(savedURL) }
    }

    private func scheduleStartWatchdog() {
        let identity = output.map(ObjectIdentifier.init)
        clockTask = Task {
            try? await Task.sleep(for: .seconds(15))
            guard !Task.isCancelled, phase == .starting, output.map(ObjectIdentifier.init) == identity else { return }
            message = "The selected source isn’t producing video. Choose it again."
            stop()
            showWindow?()
        }
    }

    private func didStart(outputID: ObjectIdentifier) {
        guard output.map(ObjectIdentifier.init) == outputID, phase == .starting else { return }
        clockTask?.cancel()
        phase = .recording
        startedAt = .now
        clockTask = Task {
            while !Task.isCancelled, phase == .recording {
                elapsed = Date.now.timeIntervalSince(startedAt ?? .now)
                let capacity = try? destination.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
                if let available = capacity?.volumeAvailableCapacityForImportantUsage, available < 100_000_000 {
                    message = "Recording stopped because your disk is nearly full."
                    stop()
                    return
                }
                do { try await Task.sleep(for: .seconds(1)) } catch { return }
            }
        }
    }

    private func didFinish(outputID: ObjectIdentifier) {
        guard output.map(ObjectIdentifier.init) == outputID, let pendingURL else { return }
        stopWatchdog?.cancel()
        phase = .stopping
        clockTask?.cancel()
        finalizationTask = Task {
            do {
                // Validate that a video exists before presenting a successful save.
                let asset = AVURLAsset(url: pendingURL)
                let tracks = try await asset.loadTracks(withMediaType: .video)
                let duration = try await asset.load(.duration)
                guard output.map(ObjectIdentifier.init) == outputID else { return }
                try Task.checkCancellation()
                guard !tracks.isEmpty, duration.seconds.isFinite, duration.seconds > 0 else {
                    throw RecorderError.emptyRecording
                }
                let finalURL = RecordingFiles.finalURL(in: destination)
                try FileManager.default.moveItem(at: pendingURL, to: finalURL)
                savedURL = finalURL
                savedBytes = Int64((try? finalURL.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0)
                elapsed = duration.seconds
                releaseSession()
                phase = .saved
                showWindow?()
            } catch {
                await failCapture("The recording couldn’t be saved: \(error.localizedDescription)")
            }
        }
    }

    private func failCapture(_ description: String) async {
        // Detach callbacks before awaiting cleanup to avoid treating a failed output as saved.
        let failedStream = stream
        output = nil
        stream = nil
        clockTask?.cancel()
        stopWatchdog?.cancel()
        finalizationTask?.cancel()
        phase = .stopping
        if let failedStream { try? await failedStream.stopCapture() }
        unfinishedURL = pendingURL.flatMap { FileManager.default.fileExists(atPath: $0.path) ? $0 : nil }
        releaseSession()
        message = description
        phase = .ready
        showWindow?()
    }

    private func releaseSession() {
        clockTask?.cancel()
        clockTask = nil
        stopWatchdog?.cancel()
        stopWatchdog = nil
        stream = nil
        output = nil
        pendingURL = nil
        startedAt = nil
        // A new recording uses a fresh picker selection and fresh user consent.
        filter = nil
        sourceName = nil
        SCContentSharingPicker.shared.isActive = false
        if let activity { ProcessInfo.processInfo.endActivity(activity) }
        activity = nil
    }

    private func acceptSelection(_ selection: SCContentFilter) {
        guard phase == .choosing else { return }
        filter = selection
        isDisplay = selection.style == .display
        sourceName = isDisplay ? "Selected screen" : "Selected window"
        if #available(macOS 15.2, *), !isDisplay,
           let window = selection.includedWindows.first {
            sourceName = window.title?.isEmpty == false ? window.title : window.owningApplication?.applicationName
        }
        phase = .ready
        showWindow?()
        // The countdown (Esc cancels it) still runs in the main window before capture begins.
        if startsAfterChoosing { start() }
    }

    private func scheduleStopWatchdog() {
        stopWatchdog?.cancel()
        let identity = output.map(ObjectIdentifier.init)
        stopWatchdog = Task {
            do { try await Task.sleep(for: .seconds(20)) } catch { return }
            guard phase == .stopping, output.map(ObjectIdentifier.init) == identity else { return }
            // Don't claim the temporary MP4 is valid if the OS writer never acknowledges completion.
            let stalledStream = stream
            unfinishedURL = pendingURL.flatMap { FileManager.default.fileExists(atPath: $0.path) ? $0 : nil }
            releaseSession()
            phase = .ready
            message = "macOS didn’t finish the recording. Any unfinished file has been kept; it may not be playable. Please choose your source again."
            showWindow?()
            if let stalledStream { try? await stalledStream.stopCapture() }
        }
    }
}

extension Recorder: SCContentSharingPickerObserver {
    nonisolated func contentSharingPicker(_ picker: SCContentSharingPicker, didCancelFor stream: SCStream?) {
        Task { @MainActor in
            guard phase == .choosing else { return }
            cancelSelection()
            showWindow?()
        }
    }

    nonisolated func contentSharingPicker(_ picker: SCContentSharingPicker, didUpdateWith filter: SCContentFilter, for stream: SCStream?) {
        Task { @MainActor in acceptSelection(filter) }
    }

    nonisolated func contentSharingPickerStartDidFailWithError(_ error: any Error) {
        let detail = error.localizedDescription
        Task { @MainActor in
            guard !phase.isBusy else { return }
            phase = .ready
            message = "Couldn’t open the screen picker: \(detail)"
            showWindow?()
        }
    }
}

extension Recorder: SCRecordingOutputDelegate {
    nonisolated func recordingOutputDidStartRecording(_ recordingOutput: SCRecordingOutput) {
        let identity = ObjectIdentifier(recordingOutput)
        Task { @MainActor in didStart(outputID: identity) }
    }

    nonisolated func recordingOutputDidFinishRecording(_ recordingOutput: SCRecordingOutput) {
        let identity = ObjectIdentifier(recordingOutput)
        Task { @MainActor in didFinish(outputID: identity) }
    }

    nonisolated func recordingOutput(_ recordingOutput: SCRecordingOutput, didFailWithError error: any Error) {
        let identity = ObjectIdentifier(recordingOutput)
        let detail = error.localizedDescription
        Task { @MainActor in
            guard output.map(ObjectIdentifier.init) == identity else { return }
            await failCapture("Recording failed: \(detail)")
        }
    }
}

extension Recorder: SCStreamDelegate {
    nonisolated func stream(_ stream: SCStream, didStopWithError error: any Error) {
        let identity = ObjectIdentifier(stream)
        let detail = error.localizedDescription
        Task { @MainActor in
            guard self.stream.map(ObjectIdentifier.init) == identity else { return }
            // Keep the output alive for its finalization callback.
            // Stopping from the macOS recording indicator is a normal stop, not a failure.
            if (error as? SCStreamError)?.code != .userStopped {
                message = "Capture stopped: \(detail)"
            }
            phase = .stopping
            clockTask?.cancel()
            scheduleStopWatchdog()
            showWindow?()
        }
    }
}

private enum RecorderError: LocalizedError {
    case destinationNotWritable, lowDiskSpace, microphonePermissionChanged, invalidSource, emptyRecording
    var errorDescription: String? {
        switch self {
        case .destinationNotWritable: "Choose a save folder you can write to."
        case .lowDiskSpace: "Free up at least 250 MB or choose another disk before recording."
        case .microphonePermissionChanged: "Microphone permission changed. Turn it on again, or record without it."
        case .invalidSource: "The selected source has no usable picture. Choose another window or screen."
        case .emptyRecording: "No video frames were recorded. Try selecting the source again."
        }
    }
}

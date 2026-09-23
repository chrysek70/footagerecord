import Foundation

public enum RecordingQuality: String, CaseIterable, Sendable, Identifiable {
    case small, balanced, sharp, original
    public var id: Self { self }
    public var title: String { rawValue.capitalized }
    public var detail: String {
        switch self {
        case .small: "Up to 720p · smaller files"
        case .balanced: "Up to 1080p · everyday recording"
        case .sharp: "Up to 4K · more detail"
        case .original: "Exact window or screen pixels · largest files"
        }
    }
    public var summary: String {
        switch self {
        case .small: "720p"
        case .balanced: "1080p"
        case .sharp: "Up to 4K"
        case .original: "Original size"
        }
    }
    /// `nil` records the source's own pixel size.
    public var maximumSize: PixelSize? {
        switch self {
        case .small: PixelSize(width: 1280, height: 720)
        case .balanced: PixelSize(width: 1920, height: 1080)
        case .sharp: PixelSize(width: 3840, height: 2160)
        case .original: nil
        }
    }
}

public struct PixelSize: Equatable, Sendable {
    public let width: Int
    public let height: Int
    public init(width: Int, height: Int) {
        self.width = width
        self.height = height
    }

    /// Hardware H.264 encoding fails above 4096×2304 (measured on Apple silicon); larger frames need HEVC.
    public var fitsH264: Bool { width <= 4096 && height <= 4096 && width * height <= 4096 * 2304 }

    /// Encoders require positive, even dimensions. Never scale normal inputs up.
    public static func fitting(width: Double, height: Double, within limit: PixelSize?) -> PixelSize? {
        guard width.isFinite, height.isFinite, width >= 2, height >= 2 else { return nil }
        var scale = 1.0
        if let limit {
            guard limit.width >= 2, limit.height >= 2 else { return nil }
            scale = min(1, Double(limit.width) / width, Double(limit.height) / height)
        }
        return PixelSize(
            width: max(2, Int((width * scale / 2).rounded(.down)) * 2),
            height: max(2, Int((height * scale / 2).rounded(.down)) * 2)
        )
    }
}

public enum RecordingFiles {
    public static func finalURL(in directory: URL, date: Date = .now) -> URL {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyy-MM-dd HH-mm-ss"
        let name = "Footage \(formatter.string(from: date))"
        var candidate = directory.appendingPathComponent(name).appendingPathExtension("mp4")
        var suffix = 2
        while FileManager.default.fileExists(atPath: candidate.path) {
            candidate = directory.appendingPathComponent("\(name) (\(suffix))").appendingPathExtension("mp4")
            suffix += 1
        }
        return candidate
    }

    public static func elapsed(_ seconds: TimeInterval) -> String {
        let value = seconds.isFinite ? Int(max(0, min(seconds, 359_999))) : 0
        if value >= 3600 {
            return String(format: "%d:%02d:%02d", value / 3600, (value / 60) % 60, value % 60)
        }
        return String(format: "%02d:%02d", value / 60, value % 60)
    }
}

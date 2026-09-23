import Foundation
import Testing
@testable import RecordingCore

@Test func fittingRetinaInto1080p() {
    #expect(PixelSize.fitting(width: 3024, height: 1964, within: RecordingQuality.balanced.maximumSize)
        == PixelSize(width: 1662, height: 1080))
}

@Test func dimensionsAreEvenWithoutUpscaling() {
    #expect(PixelSize.fitting(width: 801, height: 601, within: RecordingQuality.sharp.maximumSize)
        == PixelSize(width: 800, height: 600))
}

@Test func portraitFitsWithoutCropping() {
    #expect(PixelSize.fitting(width: 1080, height: 1920, within: RecordingQuality.balanced.maximumSize)
        == PixelSize(width: 606, height: 1080))
}

@Test(arguments: [0.0, -1.0, Double.nan, Double.infinity])
func invalidSourceDimensionsAreRejected(width: Double) {
    #expect(PixelSize.fitting(width: width, height: 1080, within: RecordingQuality.balanced.maximumSize) == nil)
}

@Test func filenamesNeverOverwriteExistingRecordings() throws {
    let folder = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: folder) }
    let date = Date(timeIntervalSince1970: 1_700_000_000)
    let first = RecordingFiles.finalURL(in: folder, date: date)
    try Data("keep this recording".utf8).write(to: first)
    let second = RecordingFiles.finalURL(in: folder, date: date)
    #expect(first != second)
    #expect(second.lastPathComponent.hasSuffix("(2).mp4"))
    #expect(try String(contentsOf: first, encoding: .utf8) == "keep this recording")
}

@Test func elapsedHandlesLongAndInvalidDurations() {
    #expect(RecordingFiles.elapsed(65) == "01:05")
    #expect(RecordingFiles.elapsed(3661) == "1:01:01")
    #expect(RecordingFiles.elapsed(-1) == "00:00")
    #expect(RecordingFiles.elapsed(.nan) == "00:00")
}

@Test func originalKeepsNativePixels() {
    #expect(PixelSize.fitting(width: 1240, height: 964, within: RecordingQuality.original.maximumSize)
        == PixelSize(width: 1240, height: 964))
    #expect(PixelSize.fitting(width: 7680, height: 4321, within: RecordingQuality.original.maximumSize)
        == PixelSize(width: 7680, height: 4320))
}

@Test func largeFramesNeedHEVC() {
    #expect(PixelSize(width: 3840, height: 2160).fitsH264)
    #expect(PixelSize(width: 4096, height: 2304).fitsH264)
    #expect(!PixelSize(width: 4200, height: 2400).fitsH264)
    #expect(!PixelSize(width: 7680, height: 4320).fitsH264)
}

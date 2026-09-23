// swift-tools-version: 6.2
import PackageDescription

let package = Package(
    name: "FootageRecord",
    platforms: [.macOS(.v15)],
    products: [.executable(name: "FootageRecord", targets: ["FootageRecord"])],
    targets: [
        .target(name: "RecordingCore"),
        .executableTarget(
            name: "FootageRecord",
            dependencies: ["RecordingCore"],
            swiftSettings: [
                .defaultIsolation(MainActor.self),
                .enableUpcomingFeature("NonisolatedNonsendingByDefault"),
            ]
        ),
        .testTarget(name: "RecordingCoreTests", dependencies: ["RecordingCore"]),
    ]
)

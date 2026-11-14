// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "VoiceDesk",
    platforms: [.macOS(.v14)],
    targets: [
        .executableTarget(
            name: "VoiceDesk",
            path: "Sources",
            resources: [.copy("Resources/ui")],
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
    ]
)

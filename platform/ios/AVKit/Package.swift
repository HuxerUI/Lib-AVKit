// swift-tools-version: 5.9

import PackageDescription

let package = Package(
    name: "AVKit",
    platforms: [
        .iOS(.v15),
    ],
    products: [
        .library(
            name: "AVKit",
            targets: ["AVKit"]
        ),
    ],
    targets: [
        .target(name: "AVKit"),
    ]
)

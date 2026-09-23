#!/bin/bash
set -euo pipefail
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FIXTURE_APP="$PROJECT_ROOT/build/Footage Capture Fixture.app"
mkdir -p "$FIXTURE_APP/Contents/MacOS"
xcrun swiftc -swift-version 6 -target arm64-apple-macos15.0 "$PROJECT_ROOT/tools/CaptureFixture.swift" -o "$FIXTURE_APP/Contents/MacOS/CaptureFixture"
cat > "$FIXTURE_APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>CaptureFixture</string>
<key>CFBundleIdentifier</key><string>com.footagerecord.capture-fixture</string>
<key>CFBundleName</key><string>Footage Capture Fixture</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>LSMinimumSystemVersion</key><string>15.0</string>
</dict></plist>
PLIST
codesign --force --sign - "$FIXTURE_APP"
printf 'Built %s\n' "$FIXTURE_APP"

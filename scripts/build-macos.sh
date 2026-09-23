#!/bin/bash
set -euo pipefail
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CONFIGURATION="${CONFIGURATION:-debug}"
APP_PATH="$PROJECT_ROOT/dist/Footage Record.app"
swift build --package-path "$PROJECT_ROOT/apps/macos" -c "$CONFIGURATION"
BIN_PATH="$(swift build --package-path "$PROJECT_ROOT/apps/macos" -c "$CONFIGURATION" --show-bin-path)"
mkdir -p "$APP_PATH/Contents/MacOS" "$APP_PATH/Contents/Resources"
cp "$BIN_PATH/FootageRecord" "$APP_PATH/Contents/MacOS/FootageRecord"
cp "$PROJECT_ROOT/apps/macos/Resources/Info.plist" "$APP_PATH/Contents/Info.plist"
ICONSET="$PROJECT_ROOT/build/FootageRecord.iconset"
xcrun swift "$PROJECT_ROOT/tools/MakeAppIcon.swift" "$ICONSET"
iconutil -c icns "$ICONSET" -o "$APP_PATH/Contents/Resources/FootageRecord.icns"
# Local development identity only. Public releases need Developer ID + notarization.
codesign --force --sign "${SIGNING_IDENTITY:--}" "$APP_PATH"
printf 'Built %s\n' "$APP_PATH"

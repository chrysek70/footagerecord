#!/bin/bash
# Builds apps/windows/resources/FootageRecord.ico from the same code-drawn icon as the Mac app.
# Run on a Mac after changing tools/MakeAppIcon.swift; the .ico is committed for the Windows build.
set -euo pipefail
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ICONSET="$PROJECT_ROOT/build/FootageRecord.iconset"
xcrun swift "$PROJECT_ROOT/tools/MakeAppIcon.swift" "$ICONSET"
# Resize every entry from the largest render so each has exactly its pixel size.
for size in 16 32 48 64 128 256; do
    sips -z $size $size "$ICONSET/icon_512x512@2x.png" --out "$ICONSET/win_$size.png" >/dev/null
done
mkdir -p "$PROJECT_ROOT/apps/windows/resources"
# PNG-compressed .ico entries (supported since Windows Vista): 16, 32, 48, 64, 128, 256 px.
python3 - "$ICONSET" "$PROJECT_ROOT/apps/windows/resources/FootageRecord.ico" <<'PY'
import struct, sys
folder, out = sys.argv[1], sys.argv[2]
images = [(size, open(f"{folder}/win_{size}.png", "rb").read()) for size in (16, 32, 48, 64, 128, 256)]
offset = 6 + 16 * len(images)
header = struct.pack("<HHH", 0, 1, len(images))
entries, data = b"", b""
for size, png in images:
    entries += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(png), offset + len(data))
    data += png
open(out, "wb").write(header + entries + data)
PY
printf 'Built %s\n' "$PROJECT_ROOT/apps/windows/resources/FootageRecord.ico"

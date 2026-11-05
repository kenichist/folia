#!/bin/sh
# Build a self-contained Folia.app + DMG on macOS (tested: macOS 26.6.2 arm64,
# Apple clang 21, Homebrew qtbase 6.12.0, CMake 4.4, Ninja 1.13).
#   brew install cmake ninja qtbase
#   sh packaging/macos/package.sh
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
QT=${QT:-/opt/homebrew/opt/qtbase}
cmake -S "$ROOT" -B "$ROOT/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QT"
cmake --build "$ROOT/build" --target folia
rm -rf "$ROOT/dist" && mkdir -p "$ROOT/dist"
cp -R "$ROOT/build/src/Folia.app" "$ROOT/dist/"
# Image-format plugins are not used (no images are decoded in v1).
"$QT/bin/macdeployqt" "$ROOT/dist/Folia.app" -always-overwrite
rm -rf "$ROOT/dist/Folia.app/Contents/PlugIns/imageformats"
codesign --force --deep --sign - "$ROOT/dist/Folia.app"   # ad-hoc signature (no Developer ID)
hdiutil create -volname Folia -srcfolder "$ROOT/dist/Folia.app" -ov -format UDZO "$ROOT/dist/Folia.dmg"
du -sh "$ROOT/dist/Folia.app" "$ROOT/dist/Folia.dmg"

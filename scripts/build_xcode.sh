#!/bin/zsh
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build/xcode}"
PROJECT_FILE="$BUILD_DIR/gstmax.xcodeproj"

if ! command -v xcodebuild >/dev/null 2>&1; then
    echo "xcodebuild is required." >&2
    exit 1
fi

if ! command -v xcrun >/dev/null 2>&1; then
    echo "xcrun is required." >&2
    exit 1
fi

# xcode-select が Command Line Tools を指している場合、Xcode 本体へ切り替える
DEVELOPER_DIR="$(xcode-select -p 2>/dev/null || true)"
if [[ "$DEVELOPER_DIR" == *"CommandLineTools"* || -z "$DEVELOPER_DIR" ]]; then
    XCODE_DEVELOPER_DIR="/Applications/Xcode.app/Contents/Developer"
    if [[ -d "$XCODE_DEVELOPER_DIR" ]]; then
        export DEVELOPER_DIR="$XCODE_DEVELOPER_DIR"
    else
        echo "Xcode (not just Command Line Tools) is required for Xcode project generation." >&2
        echo "Install Xcode from the App Store, then run: sudo xcode-select -s /Applications/Xcode.app/Contents/Developer" >&2
        exit 1
    fi
fi

CC="$(xcrun -find clang)" \
CXX="$(xcrun -find clang++)" \
CMAKE_GENERATOR=Xcode \
BUILD_DIR="$BUILD_DIR" \
"$ROOT_DIR/scripts/build.sh"

echo "Xcode project is available at $PROJECT_FILE"

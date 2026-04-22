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

CC="$(xcrun -find clang)" \
CXX="$(xcrun -find clang++)" \
CMAKE_GENERATOR=Xcode \
BUILD_DIR="$BUILD_DIR" \
"$ROOT_DIR/scripts/build.sh"

echo "Xcode project is available at $PROJECT_FILE"

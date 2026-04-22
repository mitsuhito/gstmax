#!/bin/zsh
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build/default}"
CONFIGURATION="${CONFIGURATION:-Release}"
GENERATOR="${CMAKE_GENERATOR:-}"
DEFAULT_SDK_DIR="$ROOT_DIR/source/max-sdk-base"
SDK_DIR="${MAX_SDK_BASE_DIR:-${MAX_SDK_ROOT:-$DEFAULT_SDK_DIR}}"

if ! command -v cmake >/dev/null 2>&1; then
    echo "cmake is required." >&2
    exit 1
fi

if ! command -v pkg-config >/dev/null 2>&1; then
    echo "pkg-config is required." >&2
    exit 1
fi

if [[ ! -d "$SDK_DIR/c74support" && -d "$SDK_DIR/source/max-sdk-base/c74support" ]]; then
    SDK_DIR="$SDK_DIR/source/max-sdk-base"
fi

if [[ ! -d "$SDK_DIR/c74support" ]]; then
    if [[ "$SDK_DIR" == "$DEFAULT_SDK_DIR" ]]; then
        "$ROOT_DIR/scripts/setup_max_sdk.sh"
    else
        echo "max-sdk-base was not found at $SDK_DIR" >&2
        exit 1
    fi
fi

pkg-config --exists gstreamer-1.0 gstreamer-app-1.0

generator_args=()
if [[ -n "$GENERATOR" ]]; then
    generator_args=(-G "$GENERATOR")
elif command -v ninja >/dev/null 2>&1; then
    generator_args=(-G Ninja)
fi

cmake_args=(
    -S "$ROOT_DIR"
    -B "$BUILD_DIR"
    -DMAX_SDK_BASE_DIR="$SDK_DIR"
    -DCMAKE_BUILD_TYPE="$CONFIGURATION"
)

if [[ -n "${CC:-}" ]]; then
    cmake_args+=(-DCMAKE_C_COMPILER="$CC")
fi

if [[ -n "${CXX:-}" ]]; then
    cmake_args+=(-DCMAKE_CXX_COMPILER="$CXX")
fi

if (( ${#generator_args[@]} )); then
    cmake_args+=("${generator_args[@]}")
fi

cmake "${cmake_args[@]}"

build_args=(
    --build "$BUILD_DIR"
    --config "$CONFIGURATION"
)

if [[ -n "${CMAKE_BUILD_PARALLEL_LEVEL:-}" ]]; then
    build_args+=(--parallel "$CMAKE_BUILD_PARALLEL_LEVEL")
fi

cmake "${build_args[@]}"

echo "Built externals into $ROOT_DIR/externals"

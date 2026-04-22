#!/bin/zsh
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DEFAULT_SDK_DIR="$ROOT_DIR/source/max-sdk-base"
SDK_DIR="${MAX_SDK_BASE_DIR:-${MAX_SDK_ROOT:-$DEFAULT_SDK_DIR}}"

if [[ ! -d "$SDK_DIR/c74support" && -d "$SDK_DIR/source/max-sdk-base/c74support" ]]; then
    SDK_DIR="$SDK_DIR/source/max-sdk-base"
fi

if [[ "$SDK_DIR" != "$DEFAULT_SDK_DIR" ]]; then
    if [[ -d "$SDK_DIR/c74support" ]]; then
        echo "Using external max-sdk-base at $SDK_DIR"
        exit 0
    fi

    echo "max-sdk-base was not found at $SDK_DIR" >&2
    echo "Set MAX_SDK_BASE_DIR to a valid checkout or use the bundled source/max-sdk-base submodule." >&2
    exit 1
fi

if [[ -d "$DEFAULT_SDK_DIR/c74support" ]]; then
    echo "max-sdk-base is ready at $DEFAULT_SDK_DIR"
    exit 0
fi

if ! command -v git >/dev/null 2>&1; then
    echo "git is required." >&2
    exit 1
fi

if [[ ! -d "$ROOT_DIR/.git" ]]; then
    echo "This script requires a git checkout so it can initialize the source/max-sdk-base submodule." >&2
    echo "Clone the repository with --recurse-submodules or set MAX_SDK_BASE_DIR to an existing checkout." >&2
    exit 1
fi

git submodule update --init --recursive source/max-sdk-base

echo "max-sdk-base is ready at $DEFAULT_SDK_DIR"

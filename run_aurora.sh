#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
MODEL_PATH="whisper.cpp/models/ggml-tiny.bin"
BINARY="build/bin/continuous_listening"

# Run the Native C++ Controller directly
# No Python. No Overhead.
"$BINARY" --model "$MODEL_PATH" --threads 8

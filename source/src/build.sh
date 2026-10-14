#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
exec taskset -c 0-11 python3 -B src/build_native.py --platform linux "$@"

#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
taskset -c 0-11 bash src/build.sh
taskset -c 0-11 python3 -B src/tests/run_u.py

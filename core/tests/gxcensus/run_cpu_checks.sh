#!/usr/bin/env bash
# Compatibility entry point; the primary CPU runner now links its optional hooks directly.
set -euo pipefail
exec bash "$(dirname "$0")/../cpu/run_tests.sh" "$@"

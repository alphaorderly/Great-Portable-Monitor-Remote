#!/bin/zsh
set -euo pipefail
exec python3 "${0:A:h}/run_tests.py" "$@"

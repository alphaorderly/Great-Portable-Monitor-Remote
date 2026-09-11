#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: bash ./flash-windows.sh

Run in Git Bash with an activated ESP-IDF 5.5.5 environment.
Alternatively, run the command above from an ESP-IDF terminal with Git Bash on PATH.
Detect serial ports and select the port, flash mode, and serial monitor in a menu.
EOF
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    usage
    exit 0
fi
if [[ $# -ne 0 ]]; then
    usage >&2
    exit 2
fi
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
if [[ -z "${IDF_PATH:-}" ]] || ! command -v python >/dev/null 2>&1; then
    echo 'Activate ESP-IDF 5.5.5, then run this script using Git Bash (not WSL).' >&2
    exit 1
fi

exec python -X utf8 "$root/firmware/scripts/flash.py"

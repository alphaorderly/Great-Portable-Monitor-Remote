#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: ./flash-macos.sh

Select the ESP32-S3 UART/COM serial port; verify the chip before flashing.
S3: use UART/COM for flashing and logs, USB/OTG for PC HID.
Uses ESP-IDF 5.5.5; the project's local SDK is activated automatically if available.
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
if [[ -z "${IDF_PATH:-}" ]]; then
    if [[ ! -f "$root/.tools/esp-idf/export.sh" ]]; then
        echo 'Activate ESP-IDF 5.5.5 with export.sh before running this script.' >&2
        exit 1
    fi
    export IDF_PATH="$root/.tools/esp-idf"
    export IDF_TOOLS_PATH="$root/.tools/espressif"
    export IDF_PYTHON_ENV_PATH="$IDF_TOOLS_PATH/python_env/idf5.5_py3.12_env"
    # ESP-IDF's activation script is not intended to run with nounset enabled.
    set +u
    source "$IDF_PATH/export.sh"
    set -u
fi
if ! command -v python >/dev/null 2>&1; then
    echo 'ESP-IDF Python was not found. Activate ESP-IDF 5.5.5 with export.sh.' >&2
    exit 1
fi

exec python -X utf8 "$root/firmware/scripts/flash.py"

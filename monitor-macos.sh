#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# Use an existing SDK Python directly; SDK activation/build/patching is unnecessary.
for candidate in \
    "${IDF_PYTHON_ENV_PATH:-}/bin/python" \
    "$root/.tools/espressif/python_env/idf5.5_py3.12_env/bin/python"; do
    if [[ -x "$candidate" ]]; then
        exec "$candidate" -X utf8 "$root/firmware/scripts/monitor.py" "$@"
    fi
done
if command -v python3 >/dev/null 2>&1; then
    exec python3 -X utf8 "$root/firmware/scripts/monitor.py" "$@"
fi
echo 'Python 3 is required. Install Python with pyserial, or use ESP-IDF Python.' >&2
exit 1

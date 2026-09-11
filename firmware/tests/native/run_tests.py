"""Run native C regressions with a POSIX cc toolchain (macOS/Linux/WSL)."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--idf-path", type=Path, default=Path(os.environ.get("IDF_PATH", root.parent / ".tools/esp-idf")))
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix="remote-native-") as tmp:
    for name in ("hid_client", "mac_hid", "input_codec", "reconnect", "keymap"):
        binary = Path(tmp) / name
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                        "-Itests/native/stubs", f"tests/native/{name}_test.c", "main/input_codec.c", "main/keymap.c",
                        "tests/native/keymap_storage_stub.c", "-o", str(binary)], cwd=root, check=True)
        command = [str(binary)]
        if name == "hid_client":
            command.append(str(root.parent / "shared/fixtures/remote_report_map.bin"))
        subprocess.run(command, cwd=root, check=True)
    actual = subprocess.check_output([str(Path(tmp) / "keymap"), "--defaults"], cwd=root).strip()
    expected = (root.parent / "shared/fixtures/keymap-default.hex").read_bytes().strip()
    if actual != expected:
        sys.exit("C keymap defaults differ from the shared fixture")
source = args.idf_path / "components/bt/host/nimble/nimble/nimble/host/src/ble_hs_resolv.c"
subprocess.run([sys.executable, str(root / "tests/native/test_nimble_identity.py"), str(source)], check=True)

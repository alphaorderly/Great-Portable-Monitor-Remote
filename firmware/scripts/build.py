"""Build the ESP32-S3 project with an activated ESP-IDF 5.5.5 environment."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

TARGET = "esp32s3"
ROOT = Path(__file__).resolve().parents[1]


def build_command(argv, idf):
    parser = argparse.ArgumentParser(description=__doc__, add_help=False, allow_abbrev=False)
    parser.add_argument("-B", "--build-dir", default="build")
    args, remaining = parser.parse_known_args(argv)
    project = ROOT.resolve()
    build = (project / args.build_dir).resolve()
    # A caller-supplied path must not reuse another target's existing cache.
    metadata = build / "project_description.json"
    if metadata.exists():
        cached = json.loads(metadata.read_text())
        if cached.get("target") != TARGET or Path(cached.get("project_path", "")).resolve() != project:
            raise ValueError(f"Build directory belongs to a different project: {build}")
    if "set-target" in remaining or any(
        value in {"-C", "--project-dir", "--target"} or value.startswith(("--target=", "--project-dir=", "-C", "-DIDF_TARGET", "-DSDKCONFIG"))
        or value.startswith(("IDF_TARGET=", "SDKCONFIG=")) for value in remaining
    ):
        raise ValueError("This project only supports ESP32-S3; target, project and SDKCONFIG overrides are not supported.")
    command = [sys.executable, str(Path(idf) / "tools/idf.py"),
               "-D", f"IDF_TARGET={TARGET}", "-B", str(build), *remaining]
    return project, command


def main(argv=None):
    idf = os.environ.get("IDF_PATH")
    if not idf:
        print("Activate ESP-IDF 5.5.5 first (export.sh / export.ps1 / ESP-IDF terminal).", file=sys.stderr)
        return 1
    try:
        project, command = build_command(sys.argv[1:] if argv is None else argv, idf)
        subprocess.run([sys.executable, str(ROOT / "scripts/apply_nimble_patch.py"), idf], check=True)
        return subprocess.call(command, cwd=project)
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        print(f"Build failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

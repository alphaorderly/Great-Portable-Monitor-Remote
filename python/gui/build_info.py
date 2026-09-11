"""Identify packaged code without depending on Git on the user's computer."""
from functools import lru_cache
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def source_build_info(root):
    root = Path(root)
    paths = sorted((root / "python").rglob("*.py"))
    paths += sorted((root / "python").glob("requirements*.txt"))
    paths += [root / "scripts/build_app.py"]
    digest = hashlib.sha256()
    for path in paths:
        digest.update(path.relative_to(root).as_posix().encode())
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    try:
        revision = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=root, stderr=subprocess.DEVNULL,
            timeout=3, text=True).strip()
    except (OSError, subprocess.SubprocessError):
        revision = "unknown"
    fingerprint = digest.hexdigest()
    return {"build_id": f"{revision[:12]}-{fingerprint[:16]}",
            "git_commit": revision, "source_sha256": fingerprint}


@lru_cache(maxsize=1)
def get_build_info():
    if getattr(sys, "frozen", False):
        try:
            return json.loads((Path(sys._MEIPASS) / "build-info.json").read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            return {"build_id": "unknown", "error": str(exc)}
    return source_build_info(Path(__file__).resolve().parents[2])

"""Build with an activated ESP-IDF 5.5.5 environment on Windows/macOS/Linux."""
import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
idf = os.environ.get("IDF_PATH")
if not idf:
    sys.exit("Activate ESP-IDF 5.5.5 first (export.sh / export.ps1 / ESP-IDF terminal).")
subprocess.run([sys.executable, str(root / "scripts/apply_nimble_patch.py"), idf], check=True)
raise SystemExit(subprocess.call([sys.executable, str(Path(idf) / "tools/idf.py"), *sys.argv[1:]], cwd=root))

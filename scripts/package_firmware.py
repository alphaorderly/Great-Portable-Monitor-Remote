"""Archive the ESP32-S3 project's exact ESP-IDF flash files and addresses."""
import argparse
import json
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]
TARGET = "esp32s3"


def package(build, release):
    target = TARGET
    metadata = json.loads((build / "flasher_args.json").read_text())
    project = json.loads((build / "project_description.json").read_text())
    if metadata["extra_esptool_args"]["chip"] != target or project["target"] != target:
        raise ValueError("Firmware target differs from build metadata; refusing to package")
    names = [*metadata["flash_files"].values(), "flash_args", "flasher_args.json"]
    for name in names:
        path = (build / name).resolve()
        if not path.is_relative_to(build.resolve()) or not path.is_file():
            raise ValueError(f"Invalid or missing flash file: {name}")
    release.mkdir(exist_ok=True, parents=True)
    output = release / f"RemoteKeyMapper-firmware-{target}.zip"
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
        for name in names:
            archive.write(build / name, name)
        archive.write(ROOT / "docs/FIRMWARE.md", "README.md")
        archive.writestr("TARGET.txt", f"{target}\nPC connection: USB HID\n")
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    print(package(ROOT / "firmware" / "build", ROOT / "release"))


if __name__ == "__main__":
    main()

"""ESP32-S3 target enforcement and distribution metadata, with temporary build artifacts."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[2]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


build = module("firmware_build", ROOT / "firmware/scripts/build.py")
package = module("firmware_package", ROOT / "scripts/package_firmware.py")


class BuildTests(unittest.TestCase):
    def test_default_uses_single_s3_project(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(build, "ROOT", Path(tmp)):
            for target in ("esp32s3",):
                args = ["-B", "build", "build"]
                project, cmd = build.build_command(args, "/sdk")
                self.assertEqual(project, Path(tmp).resolve())
                self.assertIn(f"IDF_TARGET={target}", cmd)
                self.assertEqual(cmd[cmd.index("-B") + 1], str((project / "build").resolve()))

    def test_foreign_build_cache_and_target_overrides_are_rejected(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(build, "ROOT", Path(tmp)):
            cache = Path(tmp) / "build"
            cache.mkdir(parents=True)
            (cache / "project_description.json").write_text(json.dumps({"target": "esp32s3", "project_path": str(Path(tmp) / "esp32s3")}))
            with self.assertRaises(ValueError):
                build.build_command(["build"], "/sdk")
            (cache / "project_description.json").unlink()
            for args in (["--target", "esp32"],
                         ["--target=esp32s3", "build"],
                         ["set-target", "esp32"],
                         ["-DIDF_TARGET=esp32", "build"],
                         ["-D", "SDKCONFIG=/other", "build"],
                         ["-C", "/other", "build"]):
                with self.assertRaises(ValueError):
                    build.build_command(args, "/sdk")


class PackageTests(unittest.TestCase):
    def test_packages_generated_addresses_and_rejects_wrong_target(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for target, boot_offset in (("esp32s3", "0x0"),):
                folder = root / target
                (folder / "bootloader").mkdir(parents=True)
                files = {boot_offset: "bootloader/bootloader.bin", "0x10000": "app.bin"}
                for name in files.values():
                    (folder / name).write_bytes(target.encode())
                (folder / "flasher_args.json").write_text(json.dumps({"flash_files": files, "extra_esptool_args": {"chip": target}}))
                (folder / "project_description.json").write_text(json.dumps({"target": target}))
                (folder / "flash_args").write_text(f"{boot_offset} bootloader/bootloader.bin 0x10000 app.bin")
                archive = package.package(folder, root / "release")
                with zipfile.ZipFile(archive) as z:
                    self.assertIsNone(z.testzip())
                    self.assertEqual(z.read("flash_args"), (folder / "flash_args").read_bytes())
                    self.assertIn(boot_offset, json.loads(z.read("flasher_args.json"))["flash_files"])
                    self.assertIn(target, z.read("TARGET.txt").decode())
                (folder / "project_description.json").write_text(json.dumps({"target": "esp32"}))
                with self.assertRaises(ValueError):
                    package.package(folder, root / "release")
                (folder / "project_description.json").write_text(json.dumps({"target": target}))
                (folder / "app.bin").unlink()
                with self.assertRaises(ValueError):
                    package.package(folder, root / "release")

import importlib.util
import json
import os
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path


script = Path(__file__).resolve().parents[1] / ".github/scripts/package-ci.py"
spec = importlib.util.spec_from_file_location("package_ci", script)
package_ci = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package_ci)


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.build = self.root / "build"
        self.build.mkdir()
        self.binary = self.build / "obs-shaderfilter.dll"
        self.binary.write_bytes(b"plugin")
        (self.build / "installer.iss").write_text("installer")
        self.manifest = self.build / "artifact-Release.json"
        self.manifest.write_text(json.dumps({"binary": str(self.binary)}))
        self.output = self.root / "dist"

    def test_windows_payload_and_installer_inputs(self):
        package_ci.package(self.build, "Release", "windows", "x64", self.output)
        with zipfile.ZipFile(self.output / "obs-shaderfilter-windows-x64.zip") as archive:
            self.assertEqual(archive.read("obs-plugins/64bit/obs-shaderfilter.dll"), b"plugin")
            self.assertIn("data/obs-plugins/obs-shaderfilter/locale/en-US.ini", archive.namelist())
            self.assertIn("data/obs-plugins/obs-shaderfilter/internal/gamepad.effect", archive.namelist())
        self.assertEqual((self.output / "installer.iss").read_text(), "installer")
        self.assertTrue((self.output / "media/icon.ico").is_file())

    def test_linux_install_layout(self):
        self.binary = self.binary.rename(self.build / "obs-shaderfilter.so")
        self.manifest.write_text(json.dumps({"binary": str(self.binary)}))
        package_ci.package(self.build, "Release", "linux", "x86_64", self.output)
        with tarfile.open(self.output / "obs-shaderfilter-linux-x86_64.tar.gz") as archive:
            self.assertEqual(archive.extractfile("./lib/x86_64-linux-gnu/obs-plugins/obs-shaderfilter.so").read(), b"plugin")
            self.assertIn("./share/obs/obs-plugins/obs-shaderfilter/locale/en-US.ini", archive.getnames())

    def test_macos_preserves_bundle_resources_and_permissions(self):
        bundle = self.build / "obs-shaderfilter.plugin"
        executable = bundle / "Contents/MacOS/obs-shaderfilter"
        executable.parent.mkdir(parents=True)
        executable.write_bytes(b"plugin")
        executable.chmod(0o755)
        resource = bundle / "Contents/Resources/locale/en-US.ini"
        resource.parent.mkdir(parents=True)
        resource.write_text("locale")
        self.manifest.write_text(json.dumps({"binary": str(executable), "bundle": str(bundle)}))
        package_ci.package(self.build, "Release", "macos", "arm64", self.output)
        with tarfile.open(self.output / "obs-shaderfilter-macos-arm64.tar.gz") as archive:
            self.assertIn("./obs-shaderfilter.plugin/Contents/Resources/locale/en-US.ini", archive.getnames())
            member = archive.getmember("./obs-shaderfilter.plugin/Contents/MacOS/obs-shaderfilter")
            if os.name != "nt":
                self.assertTrue(member.mode & 0o111)

    def test_missing_binary_fails_without_publishing_empty_archive(self):
        self.binary.unlink()
        with self.assertRaises(FileNotFoundError):
            package_ci.package(self.build, "Release", "windows", "x64", self.output)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()

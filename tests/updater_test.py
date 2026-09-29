from __future__ import annotations

import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "hub"))
import updater  # noqa: E402


class VersionTests(unittest.TestCase):
    def test_semver_precedence(self):
        self.assertGreater(updater.compare_versions("1.2.4", "1.2.3"), 0)
        self.assertEqual(updater.compare_versions("v1.2.3", "1.2.3"), 0)
        self.assertGreater(updater.compare_versions("1.2.3", "1.2.3-rc.9"), 0)
        self.assertGreater(updater.compare_versions("1.2.3-rc.2", "1.2.3-rc.1"), 0)
        self.assertLess(updater.compare_versions("1.2.3-1", "1.2.3-alpha"), 0)

    def test_bad_semver(self):
        with self.assertRaises(updater.UpdateError):
            updater.parse_semver("1.2")


class InstallModeTests(unittest.TestCase):
    def test_modes(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.assertEqual(updater.install_mode(root), "unmanaged")
            (root / ".monitor-hub-release.json").write_text("{}", encoding="utf-8")
            self.assertEqual(updater.install_mode(root), "release")
            (root / ".git").mkdir()
            self.assertEqual(updater.install_mode(root), "development")


class ManifestTests(unittest.TestCase):
    def test_manifest_verification_and_safe_paths(self):
        with tempfile.TemporaryDirectory() as td:
            stage = Path(td)
            payload = stage / "payload"
            payload.mkdir()
            f = payload / "VERSION"
            f.write_text("0.1.0\n", encoding="utf-8")
            digest = hashlib.sha256(f.read_bytes()).hexdigest()
            (stage / "manifest.json").write_text(json.dumps({
                "format": 1,
                "version": "0.1.0",
                "files": [{"path": "VERSION", "size": f.stat().st_size, "sha256": digest}],
            }), encoding="utf-8")
            self.assertEqual(updater.verify_manifest(stage)["version"], "0.1.0")
            bad = json.loads((stage / "manifest.json").read_text(encoding="utf-8"))
            bad["files"][0]["path"] = "../VERSION"
            (stage / "manifest.json").write_text(json.dumps(bad), encoding="utf-8")
            with self.assertRaises(updater.UpdateError):
                updater.verify_manifest(stage)

    def test_checksum_parser(self):
        value = "a" * 64
        self.assertEqual(updater.parse_checksum(f"{value}  monitor-hub-windows-x64.zip\n"), value)


if __name__ == "__main__":
    unittest.main()

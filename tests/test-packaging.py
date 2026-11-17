#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Corresponding-source delivery must preserve source and exclude private state."""
from pathlib import Path
import json
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "ci"))
from source import export_source


class SourceDelivery(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "repo"
        self.root.mkdir()
        self.git("init", "-q")
        self.git("config", "user.name", "Packaging fixture")
        self.git("config", "user.email", "fixture@example.invalid")
        for path, data in {"LICENSE": "license", "CMakeLists.txt": "build", "src/lfo.cpp": "implementation", "AGENTS.md": "private instruction", "_local/sdk": "private SDK"}.items():
            target = self.root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(data)
        self.git("add", ".")
        self.git("commit", "-qm", "Fixture")
        self.guide = Path(self.temp.name) / "guide.md"
        self.guide.write_text("reviewed guide")
        self.output = Path(self.temp.name) / "source.tar.gz"

    def git(self, *args):
        return subprocess.check_output(["git", *args], cwd=self.root, stderr=subprocess.DEVNULL, text=True).strip()

    def test_matching_source_license_guide_without_private_files(self):
        sha = export_source(self.root, self.guide, self.output)
        with tarfile.open(self.output) as archive:
            names = archive.getnames()
            self.assertIn("obs-modulator/LICENSE", names)
            self.assertIn("obs-modulator/CMakeLists.txt", names)
            self.assertEqual(archive.extractfile("obs-modulator/src/lfo.cpp").read(), b"implementation")
            self.assertEqual(archive.extractfile("obs-modulator/README.md").read(), b"reviewed guide")
            self.assertEqual(archive.extractfile("obs-modulator/SOURCE_COMMIT").read().decode().strip(), sha)
            self.assertFalse(any("AGENTS" in n or "_local" in n or "/.git/" in n for n in names))

    def test_dirty_source_is_not_packaged_with_stale_corresponding_source(self):
        (self.root / "src/lfo.cpp").write_text("changed implementation")
        with self.assertRaises(subprocess.CalledProcessError):
            export_source(self.root, self.guide, self.output)

    def test_unreviewed_source_symlink_is_rejected(self):
        (self.root / "src/link").symlink_to("/etc/passwd")
        self.git("add", "src/link")
        self.git("commit", "-qm", "Symlink fixture")
        with self.assertRaisesRegex(RuntimeError, "non-regular"):
            export_source(self.root, self.guide, self.output)


    def test_windows_zip_requires_x64_dll_and_contains_matching_source(self):
        fixture_ci = self.root / "ci"
        fixture_ci.mkdir()
        project_ci = Path(__file__).resolve().parents[1] / "ci"
        for name in ("package.py", "source.py", "dependencies.json"):
            shutil.copyfile(project_ci / name, fixture_ci / name)
        (self.root / "CMakeLists.txt").write_text("project(obs-modulator VERSION 1.2.3)")
        self.git("add", "ci", "CMakeLists.txt")
        self.git("commit", "-qm", "Packaging entrypoint fixture")
        stage = Path(self.temp.name) / "stage"
        dll = stage / "obs-modulator/bin/64bit/obs-modulator.dll"
        dll.parent.mkdir(parents=True)
        out = Path(self.temp.name) / "dist"
        command = [sys.executable, str(fixture_ci / "package.py"), "--stage", str(stage),
                   "--guide", str(self.guide), "--output", str(out), "--platform", "windows-x64"]

        # Exercise package checks with only a PE header. This fixture is never executable.
        header = bytearray(0x80 + 24 + 0xf0)
        header[:2] = b"MZ"
        struct.pack_into("<I", header, 0x3c, 0x80)
        header[0x80:0x84] = b"PE\0\0"
        for machine, flags in [(0xaa64, 0x2000), (0x8664, 0)]:
            struct.pack_into("<HHIIIHHH", header, 0x84, machine, 0, 0, 0, 0, 0xf0, flags, 0x20b)
            dll.write_bytes(header)
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Expected a Windows x64 PE32+ DLL", result.stderr)

        struct.pack_into("<HHIIIHHH", header, 0x84, 0x8664, 0, 0, 0, 0, 0xf0, 0x2000, 0x20b)
        dll.write_bytes(header)
        subprocess.run(command, check=True, capture_output=True)
        with zipfile.ZipFile(out / "obs-modulator-1.2.3-preview-windows-x64.zip") as archive:
            manifest = json.loads(archive.read("manifest.json"))
            self.assertEqual(manifest["platform"], "windows-x64")
            self.assertEqual(manifest["source_commit"], self.git("rev-parse", "HEAD"))
            self.assertEqual(archive.read("obs-modulator/bin/64bit/obs-modulator.dll"), header)
            self.assertIn("source.tar.gz", archive.namelist())


if __name__ == "__main__":
    unittest.main()

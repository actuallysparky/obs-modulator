#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Corresponding-source delivery must preserve source and exclude private state."""
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest

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


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Export corresponding source from a reviewed Git commit, without workstation files."""
import argparse
import io
from pathlib import Path
import subprocess
import tarfile

ROOTS = {".gitignore", ".github", "CMakeLists.txt", "CMakePresets.json", "LICENSE", "ci", "data", "src", "tests", "packaging"}


def export_source(root, guide, output):
    sha = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    paths = subprocess.check_output(["git", "ls-tree", "-r", "--name-only", sha], cwd=root, text=True).splitlines()
    paths = [p for p in paths if p.split("/")[0] in ROOTS]
    if "LICENSE" not in paths or "CMakeLists.txt" not in paths:
        raise RuntimeError("Reviewed license/build source is missing from the commit")
    subprocess.run(["git", "diff", "--quiet", "HEAD", "--", *sorted(ROOTS)], cwd=root, check=True)
    output.parent.mkdir(parents=True, exist_ok=True)
    # Read committed blobs, never local SDKs, private instructions or build output.
    with tarfile.open(output, "w:gz") as archive:
        for path in paths:
            entry = subprocess.check_output(["git", "ls-tree", sha, "--", path], cwd=root, text=True)
            if not entry.startswith("100644 ") and not entry.startswith("100755 "):
                raise RuntimeError("Review non-regular source entry: " + path)
            data = subprocess.check_output(["git", "show", sha + ":" + path], cwd=root)
            info = tarfile.TarInfo("obs-modulator/" + path)
            info.size, info.mode, info.mtime = len(data), (0o755 if entry.startswith("100755 ") else 0o644), 0
            archive.addfile(info, io.BytesIO(data))
        for path, data in [("README.md", guide.read_bytes()), ("SOURCE_COMMIT", (sha + "\n").encode())]:
            info = tarfile.TarInfo("obs-modulator/" + path)
            info.size, info.mtime = len(data), 0
            archive.addfile(info, io.BytesIO(data))
    return sha


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--guide", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    print(export_source(Path(__file__).resolve().parents[1], args.guide, args.output))

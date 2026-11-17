#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Package an already staged plugin with the reviewed guide and per-file hashes."""
import argparse
import hashlib
import json
import re
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
from source import export_source

p = argparse.ArgumentParser()
p.add_argument("--stage", type=Path, required=True)
p.add_argument("--guide", type=Path, required=True)
p.add_argument("--output", type=Path, required=True)
p.add_argument("--platform", choices=["macos-arm64", "macos-universal", "linux-x86_64", "windows-x64"], required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
match = re.search(r"project\(obs-modulator VERSION ([0-9]+\.[0-9]+\.[0-9]+)", (root / "CMakeLists.txt").read_text())
if not match:
    raise SystemExit("Missing project version")
version = match.group(1)
if a.platform.startswith("macos"):
    expected = "obs-modulator.plugin/Contents/MacOS/obs-modulator"
else:
    extension = ".dll" if a.platform == "windows-x64" else ".so"
    expected = "obs-modulator/bin/64bit/obs-modulator" + extension
if not (a.stage / expected).is_file():
    raise SystemExit("Staged module missing: " + expected)
if any(x.is_symlink() for x in a.stage.rglob("*")):
    raise SystemExit("Review symlinks before packaging")
if set(x.name for x in a.stage.iterdir()) != {expected.split("/")[0]}:
    raise SystemExit("Unexpected payload at stage root")
if not a.guide.is_file():
    raise SystemExit("Reviewed preview guide is required")
if a.platform.startswith("macos"):
    subprocess.run(["codesign", "--verify", "--deep", "--strict", str(a.stage / "obs-modulator.plugin")], check=True)
    architectures = subprocess.check_output(["lipo", "-archs", str(a.stage / expected)], text=True).split()
    required = {"arm64", "x86_64"} if a.platform == "macos-universal" else {"arm64"}
    if set(architectures) != required:
        raise SystemExit("Architecture mismatch")
elif a.platform == "windows-x64":
    payload = (a.stage / expected).read_bytes()
    if len(payload) < 64 or payload[:2] != b"MZ":
        raise SystemExit("Expected a Windows PE image")
    pe, = struct.unpack_from("<I", payload, 0x3c)
    if pe < 64 or pe + 26 > len(payload) or payload[pe:pe + 4] != b"PE\0\0":
        raise SystemExit("Invalid Windows PE header")
    machine, _, _, _, _, optional_size, characteristics = struct.unpack_from("<HHIIIHH", payload, pe + 4)
    if (machine != 0x8664 or not characteristics & 0x2000 or optional_size < 2
            or pe + 24 + optional_size > len(payload) or payload[pe + 24:pe + 26] != b"\x0b\x02"):
        raise SystemExit("Expected a Windows x64 PE32+ DLL")
else:
    header = subprocess.check_output(["readelf", "-h", str(a.stage / expected)], text=True)
    if "Advanced Micro Devices X86-64" not in header:
        raise SystemExit("Expected x86-64 ELF module")
sha = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
a.output.mkdir(parents=True, exist_ok=True)
name = "obs-modulator-" + version + "-preview-" + a.platform
with tempfile.TemporaryDirectory() as t:
    payload = Path(t) / name
    shutil.copytree(a.stage, payload)
    shutil.copy2(a.guide, payload / "README.md")
    shutil.copy2(root / "LICENSE", payload / "LICENSE")
    export_source(root, a.guide, payload / "source.tar.gz")
    files = {str(x.relative_to(payload)): hashlib.sha256(x.read_bytes()).hexdigest() for x in sorted(payload.rglob("*")) if x.is_file()}
    lock = json.loads((root / "ci/dependencies.json").read_text())
    manifest = {"version": version, "channel": "preview", "platform": a.platform, "obs_build_version": lock["obs_version"], "source_commit": sha, "dependencies_lock_sha256": hashlib.sha256((root / "ci/dependencies.json").read_bytes()).hexdigest(), "files": files}
    (payload / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    archive = Path(shutil.make_archive(str(a.output / name), "gztar" if a.platform == "linux-x86_64" else "zip", payload))
digest = hashlib.sha256(archive.read_bytes()).hexdigest()
(archive.parent / (archive.name + ".sha256")).write_text(digest + "  " + archive.name + "\n")
print(archive)

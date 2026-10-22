#!/usr/bin/env python3
"""Package an already staged plugin with the reviewed guide and per-file hashes."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument("--stage", type=Path, required=True)
p.add_argument("--guide", type=Path, required=True)
p.add_argument("--output", type=Path, required=True)
p.add_argument("--platform", choices=["macos-arm64", "macos-universal", "linux-x86_64"], required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
version = (root / "CMakeLists.txt").read_text().split("VERSION ", 1)[1].split()[0]
expected = "obs-modulator.plugin/Contents/MacOS/obs-modulator" if a.platform.startswith("macos") else "obs-modulator/bin/64bit/obs-modulator.so"
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
    files = {str(x.relative_to(payload)): hashlib.sha256(x.read_bytes()).hexdigest() for x in sorted(payload.rglob("*")) if x.is_file()}
    manifest = {"version": version, "channel": "preview", "platform": a.platform, "obs_build_version": "32.2.2", "source_commit": sha, "dependencies_lock_sha256": hashlib.sha256((root / "ci/dependencies.json").read_bytes()).hexdigest(), "files": files}
    (payload / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    archive = Path(shutil.make_archive(str(a.output / name), "zip" if a.platform.startswith("macos") else "gztar", payload))
digest = hashlib.sha256(archive.read_bytes()).hexdigest()
(archive.parent / (archive.name + ".sha256")).write_text(digest + "  " + archive.name + "\n")
print(archive)

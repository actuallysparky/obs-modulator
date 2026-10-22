#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
# Run from the checkout root. All runtime/SDK inputs stay inside ignored _local.
mkdir -p _local/deps _local/runtime
python3 ci/download.py obs-mac-intel _local/deps/intel.dmg
python3 ci/download.py obs-mac-arm _local/deps/arm.dmg
python3 ci/download.py obs-source _local/deps/source.tar.gz
python3 ci/download.py simde _local/deps/simde.tar.gz
python3 ci/download.py shaderfilter-mac _local/deps/shaderfilter.pkg
mkdir -p _local/deps/source _local/deps/simde
python3 - <<'EXTRACT'
from pathlib import Path
import tarfile
for name in ['source', 'simde']:
    with tarfile.open(Path('_local/deps') / (name + '.tar.gz')) as t:
        t.extractall(Path('_local/deps') / name, filter='data')
EXTRACT
mkdir -p _local/mount-intel _local/mount-arm
trap 'hdiutil detach "$PWD/_local/mount-intel" 2>/dev/null || true; hdiutil detach "$PWD/_local/mount-arm" 2>/dev/null || true' EXIT
hdiutil attach -readonly -nobrowse -mountpoint "$PWD/_local/mount-intel" _local/deps/intel.dmg
ditto _local/mount-intel/OBS.app _local/runtime/OBS.app
hdiutil detach "$PWD/_local/mount-intel"
hdiutil attach -readonly -nobrowse -mountpoint "$PWD/_local/mount-arm" _local/deps/arm.dmg
cp _local/mount-arm/OBS.app/Contents/Frameworks/libobs.framework/libobs _local/deps/libobs-arm
hdiutil detach "$PWD/_local/mount-arm"
lipo -create _local/runtime/OBS.app/Contents/Frameworks/libobs.framework/libobs _local/deps/libobs-arm -output _local/deps/libobs-universal
python3 ci/mac-sdk.py --source "$PWD/_local/deps/source/obs-studio-32.2.2-sources" --library "$PWD/_local/deps/libobs-universal" --simde "$PWD/_local/deps/simde/simde-0.8.2" --output "$PWD/_local/deps/sdk"
pkgutil --expand-full _local/deps/shaderfilter.pkg _local/deps/shaderfilter-expanded
shader_bundle=$(find "$PWD/_local/deps/shaderfilter-expanded" -type d -name obs-shaderfilter.plugin -print -quit)
test -n "$shader_bundle"
codesign --verify --deep --strict "$shader_bundle"
export MODULATOR_SDK="$PWD/_local/deps/sdk"
export MODULATOR_OBS_FRAMEWORKS="$PWD/_local/runtime/OBS.app/Contents/Frameworks"
export MODULATOR_OBS_GRAPHICS="$MODULATOR_OBS_FRAMEWORKS/libobs-opengl.dylib"
export MODULATOR_OBS_DATA="$PWD/_local/runtime/OBS.app/Contents/Resources/data/libobs"
cmake --preset macos-universal -DMODULATOR_SHADERFILTER="$shader_bundle/Contents/MacOS/obs-shaderfilter" -DMODULATOR_SHADERFILTER_DATA="$shader_bundle/Contents/Resources"
cmake --build build-macos
ctest --test-dir build-macos --output-on-failure --repeat until-fail:3
cmake --install build-macos --prefix "$PWD/stage"
python3 ci/package.py --stage stage --guide README.md --output dist --platform macos-universal

#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Run in a disposable Linux x86_64 CI account, inside dbus-run-session.
set -euo pipefail
manifest=packaging/flatpak/com.obsproject.Studio.Plugin.Modulator.json
plugin_id=com.obsproject.Studio.Plugin.Modulator
version=$(python3 - <<'PY'
from pathlib import Path
import re
match = re.search(r'project\(obs-modulator VERSION ([0-9]+\.[0-9]+\.[0-9]+)', Path('CMakeLists.txt').read_text())
assert match, 'Project version is missing'
print(match.group(1))
PY
)
bundle="dist/obs-modulator-${version}-preview-flatpak-x86_64.flatpak"
mkdir -p dist
flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak install --user --noninteractive --assumeyes flathub com.obsproject.Studio//stable org.freedesktop.Sdk//25.08
# The stable branch moves independently of this release's pinned OBS SDK.
# Select the previously qualified 32.2.2 app commit in this disposable account.
obs_commit=$(python3 -c 'import json; print(json.load(open("ci/dependencies.json"))["flatpak_obs_commit"])')
flatpak update --user --noninteractive --assumeyes --commit="$obs_commit" com.obsproject.Studio
test "$(flatpak info --user --show-commit com.obsproject.Studio)" = "$obs_commit"
flatpak info --user --show-metadata com.obsproject.Studio > dist/obs-flatpak-metadata.ini
flatpak info --user com.obsproject.Studio > dist/obs-flatpak-info.txt
flatpak info --user org.freedesktop.Sdk//25.08 > dist/sdk-flatpak-info.txt
python3 - <<'PY'
import configparser
p = configparser.ConfigParser()
p.read('dist/obs-flatpak-metadata.ini')
assert p['Application']['runtime'] == 'org.freedesktop.Platform/x86_64/25.08', 'OBS runtime changed; review SDK compatibility'
extension = p['Extension com.obsproject.Studio.Plugin']
assert extension['directory'] == 'plugins' and extension.getboolean('subdirectories')
assert 'lib/obs-plugins' in extension['merge-dirs']
PY
flatpak-builder --user --arch=x86_64 --repo=flatpak-repo build-flatpak "$manifest"
flatpak-builder --run build-flatpak "$manifest" sh -c 'ldd /app/plugins/Modulator/lib/obs-plugins/obs-modulator.so' > dist/flatpak-dependencies.txt
if grep -q 'not found' dist/flatpak-dependencies.txt; then exit 1; fi
flatpak build-bundle --runtime --arch=x86_64 flatpak-repo "$bundle" "$plugin_id" stable
flatpak install --user --noninteractive --assumeyes --bundle "$bundle"
# Only a disposable profile is visible to this smoke run; never use operator scenes.
mkdir -p "$PWD/flatpak-smoke"
set +e
xvfb-run -a -s '-screen 0 1920x1080x24' timeout --kill-after=10s --signal=INT 35s \
  flatpak run --user --unshare=network --nosocket=wayland --socket=x11 --share=ipc \
  --env=LIBGL_ALWAYS_SOFTWARE=1 --env=XDG_CONFIG_HOME="$PWD/flatpak-smoke" \
  --filesystem="$PWD/flatpak-smoke" com.obsproject.Studio \
  --multi --disable-shutdown-check --disable-updater --disable-missing-files-check > dist/flatpak-startup.log 2>&1
status=$?
set -e
if [ "$status" != 0 ] && [ "$status" != 124 ]; then cat dist/flatpak-startup.log; exit "$status"; fi
grep -F "[obs-modulator] Loaded ${version}" dist/flatpak-startup.log
if grep -E 'Failed to load module.*obs-modulator|obs-modulator.*(undefined symbol|cannot open shared)' dist/flatpak-startup.log; then exit 1; fi
flatpak info --user "$plugin_id" > dist/extension-flatpak-info.txt
python3 ci/source.py --guide README.md --output "dist/obs-modulator-${version}-source.tar.gz"
cp LICENSE README.md dist/
sha256sum dist/*.flatpak dist/*-source.tar.gz > dist/SHA256SUMS

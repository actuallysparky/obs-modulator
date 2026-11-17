# SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
# SPDX-License-Identifier: GPL-2.0-or-later
param([string]$Guide = 'README.md')

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true

Push-Location (Split-Path -Parent $PSScriptRoot)
try {
    $root = (Get-Location).Path
    $obsVersion = (Get-Content ci/dependencies.json -Raw | ConvertFrom-Json).obs_version
    $scratch = Join-Path $root '_local/windows-sdk'
    $archive = Join-Path $scratch 'obs-source.tar.gz'
    $source = Join-Path $scratch "obs-studio-${obsVersion}-sources"
    $sdk = Join-Path $scratch 'installed'
    $stage = Join-Path $root 'stage-windows'
    if (Test-Path $stage) { throw "Use a fresh staging directory: $stage" }

    python ci/download.py obs-source $archive
    cmake -E tar tf $archive | Out-Null
    Push-Location $scratch
    try {
        # This is an upstream tarball checked against the project's SHA-256 lock.
        if (!(Test-Path $source)) { cmake -E tar xf $archive }
    } finally {
        Pop-Location
    }

    Push-Location $source
    try {
        # OBS's preset hash-verifies its own Windows dependencies and Qt kit.
        cmake --preset windows-x64 "-DOBS_VERSION_OVERRIDE=$obsVersion" -DENABLE_BROWSER=OFF -DENABLE_PLUGINS=OFF -DENABLE_SCRIPTING=OFF -DENABLE_FRONTEND=OFF
        cmake --build build_x64 --config RelWithDebInfo --parallel --target libobs obs-frontend-api
        cmake --install build_x64 --config RelWithDebInfo --component Development --prefix $sdk
        $kit = (Get-Content CMakePresets.json -Raw | ConvertFrom-Json).configurePresets |
            Where-Object name -eq 'dependencies' | ForEach-Object { $_.vendor.'obsproject.com/obs-studio'.dependencies }
    } finally {
        Pop-Location
    }

    $qt = Join-Path $source ".deps/obs-deps-qt6-$($kit.qt6.version)-x64"
    $deps = Join-Path $source ".deps/obs-deps-$($kit.prebuilt.version)-x64"
    $obsBin = Join-Path $sdk 'bin/64bit'
    $env:Path = "$obsBin;$qt/bin;$deps/bin;$env:Path"
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = "$qt/plugins/platforms"
    cmake --preset windows-x64 "-DCMAKE_PREFIX_PATH=$sdk;$qt;$deps"
    cmake --build build-windows --config RelWithDebInfo --parallel
    ctest --test-dir build-windows -C RelWithDebInfo --output-on-failure
    cmake --install build-windows --config RelWithDebInfo --prefix $stage
    python ci/windows-smoke.py "$stage/obs-modulator" "$obsBin" "$qt/bin" "$deps/bin"
    python ci/package.py --stage $stage --guide $Guide --output dist --platform windows-x64
} finally {
    Pop-Location
}

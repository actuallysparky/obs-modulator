# SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
# SPDX-License-Identifier: GPL-2.0-or-later
param(
    [string]$Guide = 'README.md',
    [ValidateSet('All', 'Fetch', 'Configure', 'SDK', 'Package')]
    [string]$Step = 'All'
)

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
    if ($Step -in 'All', 'Fetch') {
        Write-Host "Downloading hash-locked OBS $obsVersion source"
        python -u ci/download.py obs-source $archive
        Push-Location $scratch
        try {
            if (!(Test-Path $source)) { cmake -E tar xf $archive }
        } finally {
            Pop-Location
        }
        if ($Step -eq 'Fetch') { return }
    }

    Push-Location $source
    try {
        if ($Step -in 'All', 'Configure') {
            # OBS's preset hash-verifies its own Windows dependencies and Qt kit.
            cmake --preset windows-x64 "-DOBS_VERSION_OVERRIDE=$obsVersion" -DENABLE_BROWSER=OFF -DENABLE_PLUGINS=OFF -DENABLE_SCRIPTING=OFF -DENABLE_FRONTEND=OFF
            if ($Step -eq 'Configure') { return }
        }
        if ($Step -in 'All', 'SDK') {
            cmake --build build_x64 --config RelWithDebInfo --parallel --target libobs obs-frontend-api
            cmake --install build_x64 --config RelWithDebInfo --component Development --prefix $sdk
            if ($Step -eq 'SDK') { return }
        }
        $kit = (Get-Content CMakePresets.json -Raw | ConvertFrom-Json).configurePresets |
            Where-Object name -eq 'dependencies' | ForEach-Object { $_.vendor.'obsproject.com/obs-studio'.dependencies }
    } finally {
        Pop-Location
    }

    if (Test-Path $stage) { throw "Use a fresh staging directory: $stage" }
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

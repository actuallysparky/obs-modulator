#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Create a local imported libobs target from explicit SDK inputs; do not install OBS."""
import argparse
from pathlib import Path
p = argparse.ArgumentParser()
p.add_argument("--source", type=Path, required=True)
p.add_argument("--library", type=Path, required=True)
p.add_argument("--simde", type=Path, required=True)
p.add_argument("--output", type=Path, required=True)
a = p.parse_args()
for f in [a.source / "libobs/obs.h", a.library, a.simde / "simde/x86/sse2.h"]:
    if not f.exists():
        raise SystemExit("Missing SDK input: " + str(f))
a.output.mkdir(parents=True, exist_ok=True)
(a.output / "obsconfig.h").write_text("#pragma once\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n")
includes = ";".join(str(x.resolve()) for x in [a.source / "libobs", a.output, a.simde])
(a.output / "libobsConfig.cmake").write_text(
    'add_library(OBS::libobs SHARED IMPORTED)\n'
    'set_target_properties(OBS::libobs PROPERTIES IMPORTED_LOCATION "' + str(a.library.resolve()) +
    '" INTERFACE_INCLUDE_DIRECTORIES "' + includes + '")\n')
print("Local libobs SDK configuration prepared")

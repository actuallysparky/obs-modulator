#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Load the staged Windows plugin in isolated libobs before packaging."""
from contextlib import ExitStack
import ctypes as c
import os
from pathlib import Path
import sys

plugin, obs_bin, *deps = (Path(arg).resolve(strict=True) for arg in sys.argv[1:])
with ExitStack() as paths:
    for directory in (obs_bin, *deps):
        paths.enter_context(os.add_dll_directory(str(directory)))
    obs = c.CDLL(str(obs_bin / "obs.dll"))
    obs.obs_startup.argtypes = [c.c_char_p, c.c_char_p, c.c_void_p]
    obs.obs_startup.restype = c.c_bool
    obs.obs_shutdown.restype = None
    task_type = c.CFUNCTYPE(None, c.c_void_p)
    handler_type = c.CFUNCTYPE(None, task_type, c.c_void_p, c.c_bool)

    @handler_type
    def run_task(task, param, wait):
        task(param)

    obs.obs_set_ui_task_handler.argtypes = [handler_type]
    obs.obs_set_ui_task_handler.restype = None
    obs.obs_open_module.argtypes = [c.POINTER(c.c_void_p), c.c_char_p, c.c_char_p]
    obs.obs_open_module.restype = c.c_int
    obs.obs_init_module.argtypes = [c.c_void_p]
    obs.obs_init_module.restype = c.c_bool

    if not obs.obs_startup(b"en-US", None, None):
        raise SystemExit("Isolated libobs startup failed")
    try:
        obs.obs_set_ui_task_handler(run_task)
        module = c.c_void_p()
        result = obs.obs_open_module(
            c.byref(module), str(plugin / "bin/64bit/obs-modulator.dll").encode(),
            str(plugin / "data").encode())
        if result != 0 or not module or not obs.obs_init_module(module):
            raise SystemExit(f"Staged obs-modulator.dll failed to load (obs_open_module={result})")
        print("Staged Windows x64 module loaded and initialized in libobs")
    finally:
        obs.obs_shutdown()

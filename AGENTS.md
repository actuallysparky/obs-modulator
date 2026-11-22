# OBS Modulator — agent instructions

- Preserve working OBS scenes, installed plugins and unrelated edits; use isolated libobs scenes for validation.
- Preserve eye-toggle activation, hold-on-stop, restart-on-activation and outside-edit eye-off behavior. Direct MIDI CC control takes priority over the LFO.
- Build with CMake and run CTest plus the actual-plugin integration harness for behavior changes. Use the checked-in presets and test definitions; keep generated builds outside tracked source.
- Stage custom bundles for trial. Installing into working OBS or changing scene collections needs explicit authorization.
- Maintain accurate feature, platform and release documentation. A passing mock test is not a live-stream or hardware test.

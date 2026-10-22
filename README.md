# OBS Modulator

I made this to bring LFO control to OBS effects. Pick a numeric source or filter setting, choose a waveform, cycle time and range, then enable the filter. As the filter's eye gazes deeply into your soul, gaze back and see the slider wiggle like salmon swimming upstream. If you move the target yourself, the LFO pauses and averts its gaze so you can take over.

## Preview builds

Version 0.1.1 preview targets OBS 32.2.2. The Mac bundle contains Apple Silicon and Intel slices. The Linux archive targets native Ubuntu/Kubuntu 24.04 x86-64 OBS; it is not a Flatpak package. Check the accompanying candidate report for the architectures and runtime tests actually completed. Windows builds and Flatpak packaging are separate work items. 

## Install or update

Quit OBS while streaming, recording and virtual camera are inactive. Keep a copy of any previous obs-modulator plugin outside the active plugin folder and back up a test scene collection. Extract the archive; keep the complete plugin directory together.

- Mac: place `obs-modulator.plugin` in `~/Library/Application Support/obs-studio/plugins/`. This preview uses an ad-hoc signature, not Developer ID notarization. If macOS blocks it, report the message; signed/notarized distribution remains a release task.
- Native Linux: place the complete `obs-modulator` directory in `~/.config/obs-studio/plugins/`, or the equivalent OBS configuration directory when `XDG_CONFIG_HOME` is customized. The layout must be `plugins/obs-modulator/bin/64bit/obs-modulator.so` plus `plugins/obs-modulator/data/locale/`.

Reopen OBS. Verify the current log contains `[obs-modulator] Loaded 0.1.1 (LFO only)` and the source's Add Effect Filter menu contains **LFO Modulator**. Start in an expendable test collection. Do not copy the native Linux archive into a Flatpak installation; ask for the matching packaging instead.

## Try it

1. Add **LFO Modulator** to a video source. New mappings start with their eye off.
2. Choose the target source, optional target filter, and a numeric property. This initializes both range endpoints to the property's current value.
3. Set different minimum/maximum values, choose Sine, Triangle, Rising saw, Falling saw or Square, and set a full-cycle time (four seconds by default).
4. Enable the eye. Modulation runs while the parent source is showing, including previews. Disabling or hiding holds the current value; reactivation starts a fresh cycle.
5. Move the target manually. The LFO should stop, preserve your value, turn its eye off and explain why. Re-enable to restart; **Restore starting value and disable** returns to the value captured on activation.

Duplicate the LFO filter for more controls. One active LFO owns each target/property. Transforms, mixer volume and application settings are outside this version.

## Limits

Some target widgets do not redraw continuously; check the rendered effect and reopen the target dialog to inspect settings. Same-value or transient outside writes cannot reliably be detected. A stale dialog can submit an old target value and pause the LFO. Preview/shared-source visibility can keep it running, and saving a collection can save the instantaneous target value. Target plugins may perform expensive work or change shader randomness on update. Short cycles can alias at the video frame rate.

## Remove or roll back

Quit idle OBS, move only `obs-modulator.plugin` (Mac) or `obs-modulator` (Linux) out of the active plugin directory, then reopen. To roll back, replace it with your retained previous version while OBS is closed. Keep scene backups and avoid saving away missing-plugin settings before restoring the plugin. Do not overwrite your whole OBS profile or unrelated plugins. Removing the LFO leaves target settings at their last value.

## Feedback

Please include the preview version and archive checksum, OS/CPU architecture, exact OBS version and installation method, target filter/version, steps to reproduce, expected result and actual result. Tell me whether it happens in a clean test collection too. Share a sanitized log only when needed; private profiles and streaming credentials are not needed.

## Build

C++17, CMake 3.24+, Ninja, Python 3 and a matching libobs SDK are required. Inputs used for these previews are hash-locked in `ci/dependencies.json`. `cmake --preset core`, `cmake --build build-core` and `ctest --test-dir build-core` run the platform-independent core checks. Linux CI installs the pinned official OBS package, configures `linux-x86_64`, and runs synthetic and real Shaderfilter rendering tests under Xvfb/Mesa. Software rendering in CI does not qualify a user's GPU/session.

For Mac, supply a universal libobs SDK through `MODULATOR_SDK`, plus `MODULATOR_OBS_FRAMEWORKS`, `MODULATOR_OBS_GRAPHICS` and `MODULATOR_OBS_DATA`; configure `macos-universal`. `ci/mac-sdk.py` creates an imported target from explicit OBS source headers, a library and SIMDe headers. `MODULATOR_SHADERFILTER` and `MODULATOR_SHADERFILTER_DATA` enable the separate Shaderfilter test. `ci/package.py` packages a CMake installation tree with this guide, file hashes and source provenance.

## Credits

Built using [OBS Studio/libobs](https://github.com/obsproject/obs-studio) and tested against [Exeldro's obs-shaderfilter](https://github.com/exeldro/obs-shaderfilter). The workflow was inspired by [Move Transition](https://github.com/exeldro/obs-move-transition). OBS Modulator's LFO and mapping implementation is separate; those plugins are not bundled. Third-party build inputs retain their upstream licenses.

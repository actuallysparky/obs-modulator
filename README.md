# OBS Modulator

I made this to bring risk and excitement to your streams in the form of LFO control to OBS effects delivered by alpha software. Pick a numeric source or filter setting, choose a waveform, cycle time and range, then enable the filter. As the filter's eye gazes deeply into your soul, gaze back and see the slider wiggle like salmon swimming upstream. If you move the target yourself, the LFO pauses and averts its gaze so you can take over.

## Preview builds

[Preview Alpha 0.2.0](https://github.com/actuallysparky/obs-modulator/releases/tag/v0.2.0-alpha.1) adds the **Modulator Monitor** dock: live values, range markers, short histories, activity states and eye toggles for your mappings, all at paw's reach mid-stream. Ease in / out and Glider are included too. Download the package for your OBS installation below; [Preview Alpha 0.1.2](https://github.com/actuallysparky/obs-modulator/releases/tag/v0.1.2-alpha.1) remains available for rollback.

These preview builds target OBS 32.2.2. The Mac bundle contains Apple Silicon and Intel slices. The Linux archive targets native Ubuntu/Kubuntu 24.04 x86-64 OBS; it is not a Flatpak package. Check the accompanying candidate report for the architectures and runtime tests actually completed. A separate x86-64 Flatpak extension recipe is included below. Windows builds remain a separate work item.

## Install or update

Quit OBS while streaming, recording and virtual camera are inactive. Keep a copy of any previous obs-modulator plugin outside the active plugin folder and back up a test scene collection. DID YOU HEAR ME ABOUT BACKING UP? Bear's aint wanna be responsible for wreck'n yer streams. Extract the archive; keep the complete plugin directory together.

- Mac: place `obs-modulator.plugin` in `~/Library/Application Support/obs-studio/plugins/`. This preview uses an ad-hoc signature, not Developer ID notarization. If macOS blocks it, report the message; signed/notarized distribution remains a release task.
- Native Linux: place the complete `obs-modulator` directory in `~/.config/obs-studio/plugins/`, or the equivalent OBS configuration directory when `XDG_CONFIG_HOME` is customized. The layout must be `plugins/obs-modulator/bin/64bit/obs-modulator.so` plus `plugins/obs-modulator/data/locale/`.

Reopen OBS. Verify the current log reports `[obs-modulator] Loaded` with the version from the archive manifest, and the source's Add Effect Filter menu contains **LFO Modulator**. Start in an expendable test collection. For Flatpak OBS, use the extension build below instead of the native Linux archive.

## Try it

1. Add **LFO Modulator** to a video source. New mappings start with their gaze averted.
2. Choose the target source, optional target filter, and a numeric property. This initializes both range endpoints to the property's current value.
3. Set different minimum/maximum values, choose Sine, Triangle, Rising saw, Falling saw, Square or Glider, and set a full-cycle time (four seconds by default).
4. Enable the eye, feel it pierce your soul. Modulation runs while the parent source is showing, including previews. Disabling or hiding holds the current value when easing is unchecked; with easing checked, it returns to the activation value over one cycle. Reactivation starts a fresh cycle.
5. Move the target manually. The LFO should stop, preserve your value, turn its eye off and explain why. Re-enable to restart; **Restore starting value and disable** returns to the value captured on activation.

**Ease in / out** (0.1.2) gently blends from the target's real current value into the moving LFO over one cycle time. On stop, it gently returns from the last output to that activation value over another cycle, even with the eye off or source hidden. It starts unchecked: start is immediate and stop immediately holds the current value. Enable it before turning the eye on to let a static effect gradually come to life. Re-enabling or returning from hidden during a fade starts fresh from the current value. Restart cycle fades in again while preserving the activation value. Manual target edits cancel either transition immediately; Restore starting value cancels the fade and returns immediately.

**Glider** (0.1.2) smoothly travels between random destinations inside the min/max range, easing into and out of each destination. Cycle time is the travel time between destinations, not a repeating loop. Each filter has its own sequence; restarting retraces it. Try longer times for slow organic movement. Integer targets still move in whole steps. During Ease In, a starting value outside the LFO range travels gradually into it rather than snapping to a range endpoint.

Duplicate the LFO filter for more controls. One active LFO owns each target/property. Transforms, mixer volume and application settings are outside this version.

## Live monitor dock

Open **Docks → Modulator Monitor** to see all mappings, current target values, range markers, short histories and activity states. Its checkbox is the existing filter eye; **Off** can still show **Easing out**, and an enabled mapping with a hidden parent can show **Waiting**. The value is the target setting read back from OBS, not shader-internal smoothing.

The dock refreshes only while visible. Closing it, hiding its tab or minimizing stops its timer, reads, history and paints; modulation itself continues with its existing rules. Reopening starts a fresh history. It starts closed: open it when you want a look, and dock or float it wherever it fits.

## Limits

Some target widgets do not redraw continuously; check the rendered effect and reopen the target dialog to inspect settings. Same-value or transient outside writes cannot reliably be detected. A stale dialog can submit an old target value and pause the LFO. Preview/shared-source visibility can keep it running, and saving a collection can save the instantaneous target value. Target plugins may perform expensive work or change shader randomness on update. Short cycles can alias at the video frame rate.

## Remove or roll back

Quit idle OBS, move only `obs-modulator.plugin` (Mac) or `obs-modulator` (Linux) out of the active plugin directory, then reopen. To roll back, replace it with your retained previous version while OBS is closed. Keep scene backups and avoid saving away missing-plugin settings before restoring the plugin. Do not overwrite your whole OBS profile or unrelated plugins. Removing the LFO leaves target settings at their last value.

## Feedback

Please include the preview version and archive checksum, OS/CPU architecture, exact OBS version and installation method, target filter/version, steps to reproduce, expected result and actual result. Tell me whether it happens in a clean test collection too. Share a sanitized log only when needed; private profiles and streaming credentials are not needed.

## Build

C++17, CMake 3.24+, Ninja, Python 3 and a matching libobs SDK are required. The monitor dock also needs a matching OBS frontend API SDK and Qt 6 Widgets; core-only builds remain Qt-free. Use `MODULATOR_DOCK=OFF` for a filter-only plugin. The Mac CI wrapper downloads the official hash-pinned OBS Qt SDK; it does not bundle Qt with the plugin. Inputs used for these previews are hash-locked in `ci/dependencies.json`. `cmake --preset core`, `cmake --build build-core` and `ctest --test-dir build-core` run the platform-independent core checks. Linux CI installs the pinned official OBS package, configures `linux-x86_64`, and runs synthetic and real Shaderfilter rendering tests under Xvfb/Mesa. Software rendering in CI does not qualify a user's GPU/session.

For Mac, supply a universal libobs SDK through `MODULATOR_SDK`, plus `MODULATOR_OBS_FRAMEWORKS`, `MODULATOR_OBS_GRAPHICS` and `MODULATOR_OBS_DATA`; configure `macos-universal`. `ci/mac-sdk.py` creates an imported target from explicit OBS source headers, a library and SIMDe headers. `MODULATOR_SHADERFILTER` and `MODULATOR_SHADERFILTER_DATA` enable the separate Shaderfilter test. `ci/package.py` packages a CMake installation tree with this guide, file hashes and source provenance.

## Flatpak (Linux x86-64)

The extension is `com.obsproject.Studio.Plugin.Modulator`, branch `stable`, for Flathub OBS (`com.obsproject.Studio`). It builds against OBS inside the sandbox using Freedesktop SDK 25.08, matching OBS 32.2.2's Flatpak runtime. This is a local preview extension, not a Flathub listing. A native Linux plugin archive is a different package.

On Linux x86-64, install `flatpak` and `flatpak-builder` using your distribution's package manager. From a source checkout, or the extracted corresponding-source archive, run:

```sh
flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak install --user flathub com.obsproject.Studio//stable org.freedesktop.Sdk//25.08
flatpak info --user --show-runtime com.obsproject.Studio
flatpak-builder --user --arch=x86_64 --repo=flatpak-repo build-flatpak packaging/flatpak/com.obsproject.Studio.Plugin.Modulator.json
flatpak build-bundle --runtime --arch=x86_64 flatpak-repo obs-modulator-flatpak-x86_64.flatpak com.obsproject.Studio.Plugin.Modulator stable
```

Check that the reported OBS runtime is `org.freedesktop.Platform/x86_64/25.08`; if it has changed, the SDK selection needs review before building. The builder runs the core tests. The recipe installs the module and locales under the OBS plugin extension paths; no host libobs SDK is needed. Downloads of OBS and its SDK can be several gigabytes. For a fresh rebuild, use a new build directory or the builder's `--force-clean` option on that disposable build directory.

Quit idle Flatpak OBS and back up its test collection before installation. Install your built bundle, or the matching CI `.flatpak` preview bundle, using:

```sh
flatpak install --user --bundle ./obs-modulator-flatpak-x86_64.flatpak
flatpak run --user com.obsproject.Studio
```

Use the actual filename when installing a CI bundle. These commands use a user installation of OBS and the extension; keep their installation scope consistent. Verify the loaded-version log and **LFO Modulator** menu as above. The GitHub Flatpak workflow records exact installed OBS/SDK commits and tests startup with a disposable profile under Xvfb; desktop, Wayland and hardware-GPU feedback are still needed. The `stable` runtime can change, so rebuild and retest when OBS changes.

To remove only this extension, quit idle OBS and run `flatpak uninstall --user com.obsproject.Studio.Plugin.Modulator`. To roll back, reinstall your retained older matching bundle. Keep OBS profiles, scene backups and other extensions intact.

## License

OBS Modulator's code is licensed under the **GNU General Public License, version 2 or any later version** (`GPL-2.0-or-later`), with no warranty. See [LICENSE](https://github.com/actuallysparky/obs-modulator/blob/main/LICENSE). Preview archives include the license and matching `source.tar.gz`; Flatpak downloads include the matching source archive alongside the bundle. The small AppStream metadata file is CC0-1.0; third-party inputs keep their own licenses.

## Credits

Built using [OBS Studio/libobs](https://github.com/obsproject/obs-studio) and tested against [Exeldro's obs-shaderfilter](https://github.com/exeldro/obs-shaderfilter). The workflow was inspired by [Move Transition](https://github.com/exeldro/obs-move-transition). OBS Modulator's LFO and mapping implementation is separate; those plugins are not bundled. Third-party build inputs retain their upstream licenses.

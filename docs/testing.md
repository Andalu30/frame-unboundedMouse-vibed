# Testing

Three layers. **Automated tests** check the driver's logic and its resource files. They need no headset and no SteamVR, and run in well under a second. **Static analysis** (compiler warnings, clang-tidy, clang-format) checks the code itself. **Headset checks** cover what only a real SteamVR session can show; they are a script plus a short manual list. [CI](../.github/workflows/ci.yml) runs the first two on every push.

## Automated tests

```sh
cmake -S . -B build -DMOUSELASER_TESTS=ON
cmake --build build --target unit_tests wheel_sim
ctest --test-dir build --output-on-failure
```

They work on the headset or on any Linux PC. On a PC, build only the targets above: the driver target needs the OpenVR header (`tools/fetch-openvr-header.sh <dir>` downloads Valve's, then pass `-DOPENVR_HEADERS=<dir>`), and on an x86_64 PC it writes its `.so` into `driver/mouselaser/bin/linux64` instead of `linuxarm64`.

The first configure downloads [doctest](https://github.com/doctest/doctest) 2.5.3 (a single header, test-only) and checks its SHA-256. If doctest is installed, or you point `-DDOCTEST_INCLUDE_DIR` at a directory containing `doctest/doctest.h`, nothing is downloaded. The driver itself has no dependencies.

| Test | File | What it covers |
|---|---|---|
| `unit_tests` | [tests/unit_tests.cpp](../tests/unit_tests.cpp) | The logic in [src/core.h](../src/core.h), with simulated time. **On/off sequence** (`LaserSession`): order of role change, grip and bumper pulses; the second grip pulse on the way out; cancelling early; a randomised run of 200 × 30 on/off switches checking that grip pulses always pair up. **Aim maths**: the ray starts along the HMD's forward vector, mouse direction, `invertY`, pitch clamp, parked ray. **Events**: toggle on release, inactive mouse ignored, deltas add up, button mapping, autorepeat, clicks shorter than a frame, no movement lost between threads, events lost to `SYN_DROPPED` (ignored, then buttons re-read). **Device choice**: ordinary mouse, Valve devices, keyboards, touchpads, name filter, a mouse with the toggle button beating one without. **Wheel** (`StickStepper`), both modes. **Settings**: the defaults in `core.h` match `default.vrsettings` and pass `Validate`; out-of-range numbers are clamped, impossible roles and buttons are replaced; the log line names every setting; `ApplyLive` changes every setting it can while running and names the rest, so none is silently ignored. |
| `resources` | [tests/check_resources.py](../tests/check_resources.py) | The files that describe the same thing agree: settings keys in the code, `default.vrsettings` and the README table (and the README's default values); left, right and stylus bindings stay in step; the input profile, the components `driver.cpp` creates and the localization labels match; manifest, profile, bindings and render model names resolve; the version in `CMakeLists.txt` is the one the README and the development log mention; the settings page sets `show_without_hmd` (or SteamVR hides it), shows only real settings, offers values of the right type including the default, and marks `requires_restart` on exactly the settings `ApplyLive` can't change (the same ones the README lists). |

`driver.cpp` (the SteamVR and evdev glue) has no automated tests. It is kept thin so the headset checks below cover it.

### Sanitizers
Build the tests with AddressSanitizer and UndefinedBehaviorSanitizer, or ThreadSanitizer, in a separate build directory:
```sh
cmake -S . -B build-asan -DMOUSELASER_TESTS=ON -DMOUSELASER_SANITIZE=address,undefined
cmake -S . -B build-tsan -DMOUSELASER_TESTS=ON -DMOUSELASER_SANITIZE=thread
```
Then build and run them as above. Sanitizers apply to the tests only, never to the driver `.so`.

### Tuning the wheel
`build/wheel_sim` prints how the wheel responds to a few notch patterns as 120 Hz timelines (a digit is the deflection in tenths). It uses core.h's default `Settings`, so change those to compare. It's a tuning aid, not a test.

### When changing things
- **A new setting:** add it to `Settings` in `core.h` (with a range in `Validate`, a line in `Describe`, and in `ApplyLive` either copied or named as needing a restart), `LoadSettings` in `driver.cpp`, `default.vrsettings` and the README table, and add a line to the "Settings defaults" and `ApplyLive` tests. To show it on SteamVR's settings page, add it to `settingsschema.vrsettings`. The tests fail until these agree.
- **A behaviour change:** bump `VERSION` in `CMakeLists.txt` and add a development log entry. The `resources` test checks that the README's expected version and the log's "Current version" follow.
- **A new input:** create the component in `driver.cpp`, add it to `mouselaser_profile.json` and give it a label in every language in `localization.json`.
- **A new binding:** add it for the left hand, the right hand and the stylus.

## Static analysis

| Tool | What | Run it |
|---|---|---|
| Compiler | `-Wall -Wextra -Wshadow -Wconversion -Wold-style-cast -Woverloaded-virtual -Wformat=2 -Wnull-dereference` on the driver and the tests. `Log()` carries a printf format attribute, so every log call's arguments are checked. Valve's header is included as a system header, so its own warnings are hidden. | Every build. `-DMOUSELASER_WERROR=ON` turns warnings into errors, as CI does. |
| [clang-tidy](https://clang.llvm.org/extra/clang-tidy/) | Bug patterns, thread safety, the C++ Core Guidelines, modern C++ and readability. [`.clang-tidy`](../.clang-tidy) turns every family on and lists each style rule it turns off, with the reason. [`tests/.clang-tidy`](../tests/.clang-tidy) relaxes a few for test code. A deliberate exception in the code is marked `// NOLINT(check): reason`. | `clang-tidy -p build src/driver.cpp tools/wheel_sim.cpp tests/unit_tests.cpp` (core.h is checked through driver.cpp) |
| [clang-format](https://clang.llvm.org/docs/ClangFormat.html) | The code style: Allman braces, 4 spaces, 120 columns ([`.clang-format`](../.clang-format)). | Check: `clang-format --dry-run --Werror src/* tests/*.cpp tools/*.cpp`. Fix: the same with `-i`. |

CI pins **clang-tidy and clang-format 22.1.8** (from PyPI). Other versions may report slightly different things. To get the same ones:
```sh
python3 -m venv ~/.venv-llvm && ~/.venv-llvm/bin/pip install clang-tidy==22.1.8 clang-format==22.1.8
```

In VS Code, the [SonarQube for IDE](https://docs.sonarsource.com/sonarqube-for-vs-code/getting-started/running-an-analysis) extension gives a SonarQube-style second opinion. Point it at `build/compile_commands.json`. It's optional, and CI doesn't use it.

## On the headset

The driver only loads when SteamVR starts, so rebuild and restart SteamVR first.

1. Check the log:
   ```sh
   tools/check-log.sh
   ```
   It reads `~/.local/share/Steam/logs/vrserver.txt` (or a path you pass), looks only at the latest driver load, and reports:
   - the build that loaded, the device and the mouse;
   - settings that were corrected, and a mouse without the toggle button;
   - how many switch-ons completed the on sequence (`phase on`). The driver can't see whether the laser actually appeared; that's the first manual check below;
   - whether grip pulses are balanced;
   - grab failures, mouse dropouts and lost events.

   It exits 1 if something is clearly wrong. Run it again after the manual checks.
2. Manual checks:
   - [ ] Toggle on and off 10 times, some quickly: the laser appears every time.
   - [ ] Aim across the Steam UI, the KDE desktop and the SteamVR dashboard; left, right and middle click.
   - [ ] Drag an overlay or the dashboard: it follows the mouse, not your head.
   - [ ] Wheel: one notch moves one item in a Steam list; spinning scrolls.
   - [ ] Back side button goes back in the Steam UI.
   - [ ] Real controllers: the left one loses its laser only while laser mode is on.
   - [ ] Unplug or switch off the mouse during laser mode, reconnect within a minute: laser mode continues.
   - [ ] SteamVR settings show a **Mouse Laser** section. Change Sensitivity: the laser's speed changes without a restart, and `tools/check-log.sh` shows the change and whether the settings event or the 1 s check noticed it.

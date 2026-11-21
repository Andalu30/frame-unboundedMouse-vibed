# Testing

Two layers. **Automated tests** check the driver's logic and its resource files. They need no headset and no SteamVR, and run in well under a second. **Headset checks** cover what only a real SteamVR session can show; they are a script plus a short manual list.

## Automated tests

```sh
cmake -S . -B build -DMOUSELASER_TESTS=ON
cmake --build build --target unit_tests wheel_sim
ctest --test-dir build --output-on-failure
```

They work on the headset or on any Linux PC. On a PC, build only the targets above: the driver target expects the OpenVR header and writes its `.so` into `driver/mouselaser/bin/linuxarm64`.

The first configure downloads [doctest](https://github.com/doctest/doctest) 2.5.3 (a single header, test-only) and checks its SHA-256. If doctest is installed, or you point `-DDOCTEST_INCLUDE_DIR` at a directory containing `doctest/doctest.h`, nothing is downloaded. The driver itself has no dependencies.

| Test | File | What it covers |
|---|---|---|
| `unit_tests` | [tests/unit_tests.cpp](../tests/unit_tests.cpp) | The logic in [src/core.h](../src/core.h), with simulated time. **On/off sequence** (`LaserSession`): order of role change, grip and bumper pulses; the second grip pulse on the way out; cancelling early; a randomised run of 200 × 30 on/off switches checking that grip pulses always pair up. **Aim maths**: the ray starts along the HMD's forward vector, mouse direction, `invertY`, pitch clamp, parked ray. **Events**: toggle on release, inactive mouse ignored, deltas add up, button mapping, autorepeat, clicks shorter than a frame, no movement lost between threads. **Device choice**: ordinary mouse, Valve devices, keyboards, touchpads, name filter. **Wheel** (`StickStepper`), both modes. **Settings**: the defaults in `core.h` match `default.vrsettings`. |
| `resources` | [tests/check_resources.py](../tests/check_resources.py) | The files that describe the same thing agree: settings keys in the code, `default.vrsettings` and the README table (and the README's default values); left, right and stylus bindings stay in step; the input profile, the components `driver.cpp` creates and the localization labels match; manifest, profile, bindings and render model names resolve. |

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
- **A new setting:** add it to `Settings` in `core.h`, `LoadSettings` in `driver.cpp`, `default.vrsettings` and the README table, and add a line to the "Settings defaults" test. The `resources` test fails until the first four agree.
- **A new input:** create the component in `driver.cpp`, add it to `mouselaser_profile.json` and give it a label in every language in `localization.json`.
- **A new binding:** add it for the left hand, the right hand and the stylus.

## On the headset

The driver only loads when SteamVR starts, so rebuild and reboot first.

1. Check the log:
   ```sh
   tools/check-log.sh
   ```
   It reads `~/.local/share/Steam/logs/vrserver.txt` (or a path you pass), looks only at the latest driver load, and reports:
   - the build that loaded, the device and the mouse;
   - how many switch-ons reached the laser;
   - whether grip pulses are balanced;
   - grab failures and mouse dropouts.

   It exits 1 if something is clearly wrong. Run it again after the manual checks.
2. Manual checks:
   - [ ] Toggle on and off 10 times, some quickly: the laser appears every time.
   - [ ] Aim across the Steam UI, the KDE desktop and the SteamVR dashboard; left, right and middle click.
   - [ ] Drag an overlay or the dashboard: it follows the mouse, not your head.
   - [ ] Wheel: one notch moves one item in a Steam list; spinning scrolls.
   - [ ] Back side button goes back in the Steam UI.
   - [ ] Real controllers: the left one loses its laser only while laser mode is on.
   - [ ] Unplug or switch off the mouse during laser mode, reconnect within a minute: laser mode continues.

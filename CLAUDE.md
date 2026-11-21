# CLAUDE.md

Context for AI coding sessions on this repo. Read it before changing anything.

## What this is
`mouselaser` is an experimental, **AI-written ("vibecoded")** SteamVR driver for the Steam Frame. It adds a virtual controller whose pose is aimed by a physical mouse, so the mouse can drive SteamVR's laser pointer across all overlays. Start with:
- [README.md](README.md): usage, settings, recovery.
- [docs/development-log.md](docs/development-log.md): one short entry per version.
- [docs/testing.md](docs/testing.md): automated tests and headset checks.
- [docs/steamvr-primer.md](docs/steamvr-primer.md): SteamVR and Steam Frame concepts for newcomers.
- [docs/how-it-works.md](docs/how-it-works.md): internals.
- [docs/steam-frame-background.md](docs/steam-frame-background.md): the Frame's display and input stack, and why this approach was chosen.

## Environment facts that bite
- The session usually runs **on the headset itself**, in a terminal inside the nested KDE desktop. That desktop sits inside gamescope, which is `PartOf=steamvr.service`. **Restarting SteamVR (or gamescope) kills the desktop and this session.** Ask the owner to reboot instead, and continue in a new session.
- Inside the nested desktop `XDG_RUNTIME_DIR=/run/user/1000/nested_plasma`. For `systemctl --user`, prefix the command with `XDG_RUNTIME_DIR=/run/user/1000`.
- `/usr` belongs to the read-only OS image. Don't modify it. Everything here is user-level: `vrpathreg` and `~/.config/openvr`.
- The driver `.so` and bindings are only loaded when SteamVR starts. Code changes need a rebuild **and** a reboot to test.
- Check the logs with `grep -a 'mouselaser:' ~/.local/share/Steam/logs/vrserver.txt | tail`.

## Ground rules from the owner
- **Never commit, stage or push.** Git is the owner's job.
- **Warn before doing anything that touches the system**: registering or unregistering the driver, editing `steamvr.vrsettings`, rebooting, or restarting services. Wait for approval. Editing and building inside this repo is fine.
- Keep the "vibecoded / experimental" warning in the README accurate. Don't oversell what has been tested.
- Record new findings and behaviour changes in `docs/`. Add an entry to `docs/development-log.md` for each version: a few lines on what changed, why, and whether it was tested on the headset. **No dates, times, personal paths, device serials or quotes.**
- The code is the reference for behaviour that was tested on the headset. If the docs disagree with it, fix the docs.

## Build
```sh
cmake -S . -B build -G Ninja && cmake --build build
```
Output: `driver/mouselaser/bin/linuxarm64/driver_mouselaser.so` (gitignored). The only warnings expected come from SteamVR's own `openvr_driver.h` (unused parameters). `build/compile_commands.json` is exported, so clangd can resolve the header.

Code layout: `src/core.h` holds the logic (no SteamVR, no system calls) and `src/driver.cpp` the I/O. Keep new logic in `core.h` so it can be tested.

Tests run without a headset (details in [docs/testing.md](docs/testing.md)). Keep them passing, and add a test with each logic change:
```sh
cmake -S . -B build -DMOUSELASER_TESTS=ON && cmake --build build --target unit_tests wheel_sim && ctest --test-dir build --output-on-failure
```
Static analysis must stay clean: build with `-DMOUSELASER_WERROR=ON`, and run clang-tidy and clang-format 22.1.8 (commands in docs/testing.md). CI (`.github/workflows/ci.yml`) runs all of it on arm64. Don't silence a finding without a `// NOLINT(check): reason`.

After a reboot on the headset, `tools/check-log.sh` summarises the driver's log.

Bump `k_version` in `src/driver.cpp` on every behaviour change, so the log (`mouselaser: version ...`) shows which build is running.

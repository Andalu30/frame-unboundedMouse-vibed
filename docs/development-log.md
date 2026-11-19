# Development log

## Origin (2026-10-01)
This project came out of one exploratory session. The owner asked an AI assistant (Claude, running as Claude Code in a Konsole inside the Frame's nested KDE desktop) to analyse the Steam Frame's environment, document it, and possibly change some things. The session started **read-only**. Write access was granted later, step by step, and every system-touching action was announced first and approved by the owner.

The broader notes from that session (hardware, display pipeline, KDE scale, ultrawide options, input devices) live in the parent exploration folder `~/Projects/SteamFrameExploration/` (`README.md` plus numbered `0X-*.md` files) on the original headset. They aren't part of this repo. The parts relevant here are condensed in [steam-frame-background.md](steam-frame-background.md).

## Timeline
1. **Exploration.** We found that KDE runs nested in gamescope's VR overlay, and that the mouse belongs to gamescope while the laser belongs to vrcompositor.
2. **Quick mouse investigation.** It turned out to be only a grip-to-activate shortcut. Its binding showed that the laser takes any pose source, and the Frame HMD already binds a head-gaze laser.
3. **Driver written** (`mouselaser`, ~400 lines of C++). Built on-device with gcc 15.1 / cmake 3.29 / ninja against SteamVR's bundled `openvr_driver.h` (API 2.1.0).
4. **Installed** with `vrpathreg adddriver`, after a backup of `openvrpaths.vrpath`. Then the owner restarted SteamVR.
5. **Verified working**, from the owner's report and from `vrserver.txt`:
   ```
   [Info] - mouselaser: mouselaser: ACTIVE (mouse drives the laser)
   [Info] - mouselaser: mouselaser: inactive (mouse back to desktop)
   [Info] - [Workshop] Successfully loaded binding file '.../vrcompositor_bindings_mouselaser.json' for app 'openvr.component.vrcompositor'.
   [Info] - mouselaser: mouselaser: mouse disappeared
   [Info] - mouselaser: mouselaser: using /dev/input/event6 (MCHOSE G3 A Mouse)
   ```
   - The toggle works, and the compositor loaded our bindings.
   - The mouse is re-found after it sleeps or reconnects. Its event node moved from event5 to event6 and was still found.
6. **Moved into this repo** as `0.1.0-experimental`. Changes made while copying:
   - Removed the doubled log prefix: SteamVR already adds `mouselaser: `.
   - Added a version line on startup.
   - `install.sh` now refuses to register if a `mouselaser` driver is already registered from another folder.

7. **Repo build tested** (2026-10-01, 20:10). The old registration was removed, `./install.sh` was run from the repo, and the owner rebooted. The owner confirmed it works. The log shows a single `mouselaser:` prefix, which confirms the repo build is the one loading, and toggling works.

8. **0.2.0: wheel works as the thumbstick** (2026-10-01). The owner asked for the mouse wheel to act like the controller joystick, to scroll lists.
   - Finding: Steam UI lists read the compositor's `/actions/dualanalog`; the Frame `steam.client` binding is haptics-only. Our bindings only had `scroll_discrete`.
   - Change: added `dualanalog` (position + touch) and `scroll_smooth` bindings. Replaced the 6-frame pulse with a time-based `StickStepper` (one notch = one flick). The tilt wheel drives stick X. Four new `wheel*` settings.
   - Verified offline with a 120 Hz simulation: one notch gives one push, fast notches give separate pushes, and the queue is capped.
   - **On-headset test: works** (owner, after a reboot). Lists scroll with the wheel, but it feels "a bit choppy". Tuning or a smoother mode is a follow-up.

9. **Manage Add-ons.** The driver appears in SteamVR's developer *Manage Add-ons* list, which is expected. From the dashboard JS (`chunk~*.js`): the list shows non-`resourceOnly` drivers, its toggle calls `setEnabled`, which writes `driver_<name>.enable` (the same key our driver reads), and **Unblock** clears `blocked_by_safe_mode`. Documented in the README as an in-VR kill switch.

10. **0.3.0: smoother wheel and tidier texts** (2026-10-01). The owner found 0.2.0's scrolling "a bit choppy" and asked for some adjustment, plus a tidy-up of the text SteamVR shows.
    - Added the `smooth` wheel mode as the default: each notch bumps the stick, which eases back, and spinning holds it (see how-it-works). The 0.2.0 behaviour is still available as `wheelMode: "step"`. New settings: `wheelMode`, `wheelSmoothMin`, `wheelSmoothImpulse`, `wheelSmoothHoldMs`, `wheelSmoothDecayMs`.
    - First tuning (hold 50 ms, decay 120 ms, impulse 0.25) dipped to about 0.45 between notches when spinning at 10/s, which risks retriggering steps. Retuned to 80 / 150 / 0.3, which ramps to a steady hold. Verified offline only.
    - Texts: added `resources/localization/localization.json` (en_US, es_ES) for SteamVR's binding screens. Model `Mouse Laser (virtual)`, manufacturer `frame-unboundedMouse-vibed`, and a clearer binding name and description.
    - The display name in Manage Add-ons can't be changed from the driver: the dashboard looks it up in its own `driverPrettyNames` table and otherwise falls back to the manifest name `mouselaser`.
    - **On-headset test: working** (owner, after a reboot).

11. **Pre-commit review, 0.3.1** (2026-10-01).
    - Logs: clean load, activation and binding load. No crash. The only vrserver coredump predates the driver (2026-09-28). `no suitable devices` and `steam.client … no configured binding` are benign.
    - A clean build from only the files that would be committed works. clang `-Wall -Wextra -Wshadow` reports nothing in our code. No personal data in the tree.
    - **Fix:** `MouseReader` now stops its thread in its destructor. Previously, if SteamVR destroyed the device without `Deactivate()`, the joinable `std::thread` would `std::terminate` vrserver. This never happened in the logs; it is defensive.
    - Added `tests/wheel_sim.cpp` (optional, `-DMOUSELASER_TESTS=ON`) and `CMAKE_EXPORT_COMPILE_COMMANDS`, so clangd finds `openvr_driver.h`.
    - 0.3.1 is built but **not yet rebooted into**.

12. **License: MIT** (2026-10-01).
    - The owner asked for an open-source license. The only Valve code involved is the OpenVR driver header. It is BSD-3-Clause in the public SDK and **not vendored**: we compile against SteamVR's local copy.
    - MIT was chosen over GPL because the driver is a plugin loaded into proprietary `vrserver`, where GPL plugin licensing is a grey area. MIT is also common for SteamVR drivers.
    - Copyright holder: `Andalu30` (the GitHub handle from the remote). The README notes third-party code and trademarks.

13. **Demo media** (2026-10-01). The owner recorded `~/Videos/mousedemo.mp4` (44.2 s, 1920×1080, 20 fps). It was trimmed (first 3 s and last 2 s removed, leaving 39.2 s) and converted to `docs/media/demo.webp`, which the README shows as the hero image.

14. **Back button, 0.4.0** (2026-10-04). The mouse's back side button (BTN_SIDE, setting `backButton`) is now bound to the compositor's `/actions/lasermouse/in/back`. That is the same action as the right Frame controller's B; right A is `home`. The owner reports Back works in the Steam UI and not on the KDE overlay. Investigating KDE separately (see `steam-frame-background.md`, input path) showed two causes: gamescope doesn't forward SteamVR Back to its windows, and KWin's X11 nested backend drops X buttons 8 and up even with laser mode off. The driver can't fix either one, and nothing was added for it.

15. **Stylus role, 0.5.0** (2026-10-04). Tested in entry 17: aiming and clicking work, dragging overlays doesn't.
    - Problems: (a) pressing the toggle sometimes grabbed the mouse but the laser flashed briefly or never showed, leaving neither the KDE cursor nor the laser; (b) the real left controller's laser conflicted with the virtual device.
    - An earlier session tried 0.4.1 (delay before pressing the grip), 0.4.2 (stay connected) and 0.4.3 (parked valid pose while off), all as the **left hand**. The owner reverted them from git. Its capture finding is kept: a device whose pose goes invalid or disconnects is dropped as the compositor's laser pointer until a new "user interaction started", so a quick off/on fails. Staying connected as the left hand caused conflict (b).
    - Change: `role` now defaults to **Stylus (5)**, whose user path is `/user/stylus` and which is never a hand. The bindings repeat every hand entry for `/user/stylus` (the HMD's own binding shows the compositor laser accepts pose sources that aren't hands). With a role other than a hand, the device stays connected with a valid pose, parked straight up while off. Hand roles keep the old disconnect-while-off behaviour. The profile's binding UI mode is `single_device`.
    - Added logging of SteamVR events about our device, plus interaction, role and dashboard events, to look for a "laser is up" signal. Grabbing the mouse only once the laser is up (or releasing the grab automatically) needs that signal; nothing is gated yet.
    - Open question: whether vrcompositor's laser actually picks a `/user/stylus` pointer, and whether "quick mouse" via `/user/stylus/input/grip` brings the laser up.

16. **SteamVR primer** (2026-10-04). Added `docs/steamvr-primer.md`, an accessible overview for developers new to SteamVR. It gathers the parent exploration notes (01–09), this repo's docs and new checks on the headset. New facts recorded there: `frame_hmd` and `frame_controller` are resource-only, and the `cv` driver adds the headset and both controllers. Also the full list of compositor action sets, the controller roles and their user paths, and the `vrcmd` options.

17. **0.5.0 result: stylus can't drag overlays** (2026-10-04, owner test). With role Stylus the laser aims and clicks, but dragging the dashboard or a floating overlay makes it follow the **head** instead of the mouse. With a hand role, dragging works. Cause, from `strings vrcompositor`: the only device paths the compositor knows are `/user/hand/left`, `/user/hand/right` and `/user/head`. Overlay dragging (`CGrabTransform`) attaches to one of those devices, so a `/user/stylus` pointer falls back to the head. The driver can't change this.

18. **Hybrid role, 0.5.1** (2026-10-04). The owner chose to try this over reverting. The device registers as a stylus (`role: 5`) and, while laser mode is on, sets its `Prop_ControllerRoleHint_Int32` to `activeRole` (default 1, left hand) so it can drag overlays; it goes back to stylus when switched off. The log shows `role N` on each change, and SteamVR should emit `event 108` (role changed). Things to check: whether the role change takes effect live, whether the laser comes up straight away after it, whether dragging follows the mouse, and what happens to the real left controller's laser while laser mode is on. If the role switch fails, revert to a plain hand role and document the trade-offs.
    - **On-headset test: working** (owner, after a reboot, 22:26 on 2026-10-04). The log shows `role 1` on every switch-on and `role 5` on every switch-off. SteamVR follows each with `event 108` (role changed), so it applies `Prop_ControllerRoleHint_Int32` changes live. A quick off/on (22:30:06, 0.5 s apart) kept the laser.
    - Observation: SteamVR also sends `event 100`/`101` (TrackedDeviceActivated/Deactivated) for our device on every toggle, even though it stays connected. This was already the case with 0.5.0. We don't know what it means, and it causes no visible problem.
    - The owner confirmed the real **left controller loses its laser while laser mode is on**, as expected: the device holds the left-hand slot. It is fine with laser mode off. `activeRole: 2` moves this to the right hand.

19. **Review on SteamOS 0.5.9 and upgrade plan** (2026-11-19). The owner asked for a review of the old assumptions on the current image, a plan to fix "mode switching sometimes needs several side-button presses", and a survey of every way to hook into Steam. The session was read-only on the system. In the repo it added docs, a local log-scoring script and a local build. Nothing was registered.
    - **Platform moved:** SteamOS 0.5.9 (build 20261113.6134720), kernel 7.2, KWin 6.6.2 (still the X11 windowed backend), gamescope 3.16.30. The mouse is on Bluetooth now. The toolchain is on the image, and `/dev/uinput` is usable by the `input` group. These notes are kept locally.
    - **The headset ran an old checkout:** SteamVR loaded an older local checkout (0.5.1, built 2026-10-09). This repo builds cleanly against the current header and `wheel_sim` passes.
    - **Root cause of "needs several presses":** the Frame's grip click (`ActivateQuickMouse`) **toggles** Laser Mouse ↔ Gamepad mode. Steam's own tour text says so. Our driver holds its virtual grip while active, so every activation flips the mode and none flips it back. Activations therefore alternate between working and failing.
      - The proof uses a "laser shown" proxy: the compositor logs loading `laser_pointer.obj` ~40 ms after a working ON.
      - The outcome flipped between consecutive ONs 38/42 times, and all 19 no-laser ONs were retried.
      - A first analysis in this session blamed "cold" activations (≥ 10 s off). That was a confound, because successful ONs are followed by long sessions. It was corrected the same day.
    - **Defects confirmed in the code:**
      - D0: the grip is held instead of pulsed (the root cause above).
      - D1: the inverted `present` test, so the stylus disconnects while off. That contradicts the docs and explains `event 100/101` on every toggle.
      - D2: the toggle press leaks to gamescope without its release.
      - D3: a Bluetooth mouse nap cancels laser mode.
      - D4: the first mouse found wins; it once latched onto a Steam Controller's emulated mouse.
      - D5: the grip goes down in the same frame as the role switch.
    - **Outside evidence:** Frametop (MIT) needs a ~300–400 ms settle after a connect or role change, plus a pulse on a button bound only to `SwitchLaserHand`, to claim the laser reliably.
    - **Hooks found:**
      - "Debug Input Focus" in SteamVR's developer settings shows the laser/gamepad mode.
      - The mailbox message `lasermouse ← force_activate_laser_mouse` *forces* laser mode.
      - Events 525/307/406 might let the driver observe the laser.
      - gamescope's `vr_click_focus` convar and libei socket.
    - The plan and the hooks survey are kept locally for now. Step numbers in the entries below (0.3, 1.1–1.8) refer to that plan; each entry says what the step does. Next step: owner decisions, starting with moving the registration to this repo.


21. **Registration switch verified** (2026-11-19, 19:54). The headset wasn't rebooted, but SteamVR was restarted.
    - The log shows `Loaded server driver mouselaser … from ~/Projects/frame-unboundedMouse-vibed/…` (0.5.1, unchanged code), and the bindings load from this repo. No driver errors.
    - Scoring the new session from the logs (laser render-model loads after each activation): the outcome flipped between consecutive activations **14/14** times. That confirms the grip-toggle model (entry 19) on fresh data. The first activation after the restart worked.
    - The mouse dropped out three times in 10 minutes. Once it was 30 s into a laser session and back after 4 s, and each dropout ended laser mode (D3). That makes the mouse-nap fix (step 1.7) more urgent.
22. **0.6.0-experimental: Phase 1 fix, built but NOT yet tested on the headset** (2026-11-19). The owner approved Phase 1 and asked to keep it simple, so there's no helper (1.8).
    - **1.1 (D0, D5):** a phase machine in `RunFrame`. On: role → hand, wait `claimDelayMs` (200), pulse the grip for `pulseMs` (60), then pulse the bumper. Off: pulse the grip again while still a hand, wait one more `pulseMs`, then go back to the stylus role. The log shows each step (`phase …`).
    - **1.2:** new `/input/bumper/click`, bound only to `lasermouse_secondary/SwitchLaserHand`.
    - **1.4:** mouse buttons are latched, so a click shorter than a frame still registers.
    - **1.6 (D2):** the toggle fires on release. Switching on, gamescope sees one whole forward-button click (which may go "forward" in the focused app). Switching off, it sees nothing.
    - **1.7, simplified (D3, D4):** if the mouse vanishes while active, laser mode is kept for `reconnectGraceS` (60 s), and the mouse is re-grabbed when it returns. Devices with vendor `28de` or "Steam" in the name are skipped unless `deviceNameFilter` is set. Not done: inotify, reading several mice.
    - **0.3:** every SteamVR event is logged (first 20 per type, then every 100th), to see whether event 525/307/406 reaches drivers. That decides 1.3.
    - **Not done:** 1.3 (waits for 0.3's data). 1.5: D1's behaviour (stylus disconnects while off) is kept because it's the tested one; the comment now says so. 1.8 dropped.
    - Expected: flip rate near 0, and "NO LASER" rare. The `.so` in the repo was rebuilt while SteamVR had the old one loaded, so **reboot before testing**.

23. **0.6.0 tested on the headset, 0.6.1 built** (2026-11-19, 20:20).
    - **The fix works.** Every activation that reached the grip pulse showed the laser. The three "NO LASER" lines were activations switched off within ~180 ms, before the pulse: they were correctly cancelled and kept the mode's parity. So the report's flip rate (4/10) is not a failure measure any more; look at which ONs reached `phase grip pulse`.
    - **Too slow.** The laser appeared ~240 ms after the toggle, against ~40 ms in 0.5.1. Almost all of it is `claimDelayMs = 200`; the laser shows ~30 ms after the grip pulse. 0.5.1 pressed the grip in the same frame as the role change and still got the laser, so **0.6.1 lowers the default to 50 ms**. Toggling on release also adds however long the button is held.
    - **Event survey (0.3):** event 525 (PrimaryDashboardDeviceChanged) **never reaches the driver**. 307 (OverlayFocusChanged) does, but it doesn't say who owns the laser. So 1.3 isn't possible inside the driver; it's dropped along with 1.8.

24. **0.6.1 tested on the headset: the toggle bug is fixed** (2026-11-19, 20:26). 9 activations, all with the laser, about 100 ms after the button is released. The outcome flipped between consecutive activations 0/8 times (0.5.1: 14/14). The owner says it's "totally snappy".

## State at hand-off
- **Registered from this repo since 2026-11-19** (owner-approved). The old checkout was unregistered but kept. Verified loading on 2026-11-19 19:54 (entry 21). `vrpathreg adddriver` made KDE show "could not read file vrmonitor://driverinstalled": `vrpathreg` runs `xdg-open` on that URL to notify vrmonitor, and nothing on the Frame handles it. Harmless; vrserver logged a clean connect and disconnect, and the path was saved.
- Current version: **0.6.1-experimental, tested on the headset** (entry 24). It fixes the toggle bug analysed in entry 19.
- Nothing is vendored. The build depends on SteamVR's bundled header.
- Licensed MIT (see `LICENSE`). SPDX tags are on the sources.
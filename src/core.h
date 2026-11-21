// SPDX-License-Identifier: MIT
// core.h: the driver's decision-making, kept free of SteamVR and of system calls so it can be
// unit tested on any Linux machine (tests/unit_tests.cpp). driver.cpp does the I/O: it reads the
// settings and the mouse, talks to SteamVR, and calls into the pieces below.
//
//   Settings      every tunable, with its default; Validate() and Describe() for the log
//   StickStepper  wheel notches -> thumbstick deflection over time
//   LatchedButton a click shorter than one frame still shows up for one frame
//   MouseState    what the mouse did since the last frame (shared between threads)
//   HandleEvent   one evdev event -> MouseState; EventStream adds the kernel's lost-events case
//   RateMouse     which /dev/input device to use
//   LaserSession  the on/off hand-over sequence with SteamVR's laser
//   Aim           ray direction maths
//
// Everything is header-only: the driver is a single translation unit (driver.cpp includes this).

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <sstream>
#include <string>
#include <vector>

#include <linux/input.h>

namespace mouselaser
{

using Clock = std::chrono::steady_clock; // monotonic: unaffected by changes to the system clock
using TimePoint = Clock::time_point;
using Ms = std::chrono::milliseconds;

constexpr float k_pi = 3.14159265358979f;

// Controller roles, with the same values as OpenVR's ETrackedControllerRole. Repeated here so
// this header doesn't need openvr_driver.h; driver.cpp static_asserts that they match.
constexpr int k_roleLeftHand = 1;
constexpr int k_roleRightHand = 2;
constexpr int k_roleStylus = 5;

struct Settings
{
    float sensitivityDeg = 0.05f; // degrees of ray rotation per mouse count
    int toggleButton = BTN_EXTRA;
    int backButton = BTN_SIDE; // sends the compositor's laser "back"; -1 = none
    // Stylus (5) gets its own /user/stylus path and reports itself disconnected while off. A hand
    // role (1/2) shares the slot with the real controller of that hand and stays connected while
    // off, so that controller loses its laser all the time.
    int role = k_roleStylus;
    // Role while laser mode is on (0 = keep `role`). The compositor attaches dragged overlays
    // to /user/hand/{left,right} or /user/head only, so a stylus drag follows the head.
    int activeRole = k_roleLeftHand;
    std::string nameFilter;       // substring of the evdev name; empty = first mouse found
    float originOffsetY = -0.08f; // metres, world space, relative to the HMD
    bool invertY = false;
    // How wheel notches drive the virtual thumbstick:
    //  smooth: each notch bumps the stick, which eases back to centre; spinning holds it.
    //  step:   each notch is one flick, pushed for wheelPressMs then centred for wheelReleaseMs.
    bool wheelSmooth = true;
    float wheelSmoothMin = 0.7f;     // deflection right after a single notch
    float wheelSmoothImpulse = 0.3f; // extra deflection per further notch
    int wheelSmoothHoldMs = 80;      // no decay this long after a notch
    int wheelSmoothDecayMs = 150;    // exponential decay time constant
    int wheelPressMs = 90;
    int wheelReleaseMs = 60;
    float wheelDeflection = 1.0f; // maximum deflection (both modes)
    int wheelMaxQueued = 10;      // notches buffered while flicks are still playing out
    // Laser mode handshake (see LaserSession).
    int claimDelayMs = 50;        // wait after the role change before pulsing (bindings settle)
    int pulseMs = 60;             // length of each grip / bumper pulse
    float reconnectGraceS = 60.f; // keep laser mode this long while the mouse is gone (sleep)
};

inline bool IsRole(int role) { return role == k_roleLeftHand || role == k_roleRightHand || role == k_roleStylus; }

namespace detail // helpers for Validate below
{
// Puts `value` back inside [lo, hi] if it is outside, and says so in `problems`. A template, so
// the same code serves int and float settings (T is the setting's type).
template <typename T> void Clamp(std::vector<std::string> &problems, const char *key, T &value, T lo, T hi)
{
    if (value >= lo && value <= hi)
        return;
    const T fixed = value > hi ? hi : lo;
    std::ostringstream m;
    m << key << " " << value << " is outside " << lo << " to " << hi << ", using " << fixed;
    problems.push_back(m.str());
    value = fixed;
}

// Puts `value` back to `fallback`, for a value that can't work; `why` says why, for the log.
inline void Replace(std::vector<std::string> &problems, const char *key, int &value, int fallback, const char *why)
{
    std::ostringstream m;
    m << key << " " << value << " " << why << ", using " << fallback;
    problems.push_back(m.str());
    value = fallback;
}
} // namespace detail

// The settings come from a file edited by hand, so any value can be out of range. Validate()
// brings each one back into a range the driver works with, and returns one message per change
// for the log. Numbers are clamped to their range; a value that has to be one of a few choices
// (a role, a button) goes back to its default.
inline std::vector<std::string> Validate(Settings &s)
{
    using detail::Clamp;
    using detail::Replace;
    std::vector<std::string> problems;
    const Settings defaults;

    Clamp(problems, "sensitivity", s.sensitivityDeg, 0.001f, 1.f);
    Clamp(problems, "originOffsetY", s.originOffsetY, -1.f, 1.f);

    // Buttons are evdev key codes, 1 to KEY_MAX. The left button is the laser's click, so it
    // can't be the toggle. Back can't share a button that already does something: HandleEvent
    // would never get to it.
    if (s.toggleButton < 1 || s.toggleButton > KEY_MAX || s.toggleButton == BTN_LEFT)
        Replace(problems, "toggleButton", s.toggleButton, defaults.toggleButton,
                "is not a key code other than 272 (left)");
    if (s.backButton != -1 && (s.backButton < 1 || s.backButton > KEY_MAX))
        Replace(problems, "backButton", s.backButton, defaults.backButton, "is not -1 or a key code");
    if (s.backButton == s.toggleButton || s.backButton == BTN_LEFT || s.backButton == BTN_RIGHT ||
        s.backButton == BTN_MIDDLE)
        Replace(problems, "backButton", s.backButton, -1, "is already the toggle or a click button");

    if (!IsRole(s.role))
        Replace(problems, "role", s.role, defaults.role, "is not 1, 2 or 5");
    if (s.activeRole != 0 && !IsRole(s.activeRole))
        Replace(problems, "activeRole", s.activeRole, defaults.activeRole, "is not 0, 1, 2 or 5");

    Clamp(problems, "claimDelayMs", s.claimDelayMs, 0, 2000);
    Clamp(problems, "pulseMs", s.pulseMs, 10, 1000);
    Clamp(problems, "reconnectGraceS", s.reconnectGraceS, 0.f, 3600.f);

    // The stick's range is -1 to 1.
    Clamp(problems, "wheelDeflection", s.wheelDeflection, 0.f, 1.f);
    Clamp(problems, "wheelSmoothMin", s.wheelSmoothMin, 0.f, s.wheelDeflection);
    Clamp(problems, "wheelSmoothImpulse", s.wheelSmoothImpulse, 0.f, 1.f);
    Clamp(problems, "wheelSmoothHoldMs", s.wheelSmoothHoldMs, 0, 2000);
    Clamp(problems, "wheelSmoothDecayMs", s.wheelSmoothDecayMs, 1, 5000);
    Clamp(problems, "wheelPressMs", s.wheelPressMs, 1, 1000);
    Clamp(problems, "wheelReleaseMs", s.wheelReleaseMs, 0, 1000);
    Clamp(problems, "wheelMaxQueued", s.wheelMaxQueued, 1, 100); // 0 would silence step mode
    return problems;
}

// The settings in use, as one line for the log, named as in steamvr.vrsettings.
inline std::string Describe(const Settings &s)
{
    std::ostringstream o;
    o << std::boolalpha; // bools as true/false
    o << "sensitivity " << s.sensitivityDeg;
    o << ", toggleButton " << s.toggleButton;
    o << ", backButton " << s.backButton;
    o << ", role " << s.role;
    o << ", activeRole " << s.activeRole;
    o << ", deviceNameFilter \"" << s.nameFilter << '"';
    o << ", originOffsetY " << s.originOffsetY;
    o << ", invertY " << s.invertY;
    o << ", claimDelayMs " << s.claimDelayMs;
    o << ", pulseMs " << s.pulseMs;
    o << ", reconnectGraceS " << s.reconnectGraceS;
    o << ", wheelMode " << (s.wheelSmooth ? "smooth" : "step");
    o << ", wheelDeflection " << s.wheelDeflection;
    o << ", wheelSmoothMin " << s.wheelSmoothMin;
    o << ", wheelSmoothImpulse " << s.wheelSmoothImpulse;
    o << ", wheelSmoothHoldMs " << s.wheelSmoothHoldMs;
    o << ", wheelSmoothDecayMs " << s.wheelSmoothDecayMs;
    o << ", wheelPressMs " << s.wheelPressMs;
    o << ", wheelReleaseMs " << s.wheelReleaseMs;
    o << ", wheelMaxQueued " << s.wheelMaxQueued;
    return o.str();
}

// Turns wheel notches on one axis into thumbstick deflection, in "smooth" or "step" mode
// (see Settings). A change of direction drops whatever was still pending the other way.
// `now` is passed in rather than read from the clock, so tests can simulate time.
class StickStepper
{
public:
    float Update(int ticks, const Settings &s, TimePoint now)
    {
        return s.wheelSmooth ? UpdateSmooth(ticks, s, now) : UpdateStep(ticks, s, now);
    }

    void Reset()
    {
        m_queued = 0;
        m_pressed = false;
        m_mag = 0;
    }

private:
    float UpdateSmooth(int ticks, const Settings &s, TimePoint now)
    {
        const float dt = std::chrono::duration<float>(now - m_last).count();
        m_last = now;
        if (ticks != 0)
        {
            const float d = ticks > 0 ? 1.f : -1.f;
            if (d != m_dir)
            {
                m_dir = d;
                m_mag = 0;
            }
            const auto n = static_cast<float>(std::abs(ticks));
            const float bumped =
                m_mag > 0 ? m_mag + s.wheelSmoothImpulse * n : s.wheelSmoothMin + s.wheelSmoothImpulse * (n - 1);
            m_mag = std::min(s.wheelDeflection, std::max(bumped, s.wheelSmoothMin));
            m_lastNotch = now;
        }
        else if (m_mag > 0 && now - m_lastNotch > Ms(s.wheelSmoothHoldMs))
        {
            m_mag *= std::exp(-dt * 1000.f / static_cast<float>(std::max(1, s.wheelSmoothDecayMs)));
            if (m_mag < 0.1f)
                m_mag = 0;
        }
        return m_dir * m_mag;
    }

    float UpdateStep(int ticks, const Settings &s, TimePoint now)
    {
        if (ticks != 0)
        {
            const float d = ticks > 0 ? 1.f : -1.f;
            if (d != m_dir)
            {
                m_dir = d;
                m_queued = 0;
                m_pressed = false;
                m_until = now;
            }
            m_queued = std::min(m_queued + std::abs(ticks), s.wheelMaxQueued);
        }
        if (now >= m_until)
        {
            if (m_pressed)
            {
                m_pressed = false;
                m_until = now + Ms(s.wheelReleaseMs);
            }
            else if (m_queued > 0)
            {
                m_queued--;
                m_pressed = true;
                m_until = now + Ms(s.wheelPressMs);
            }
        }
        return m_pressed ? m_dir * s.wheelDeflection : 0.f;
    }

    float m_dir = 0; // +1 or -1: direction of the last notch
    // step mode
    int m_queued = 0;
    bool m_pressed = false;
    TimePoint m_until;
    // smooth mode
    float m_mag = 0;
    TimePoint m_last, m_lastNotch;
};

// A button that also remembers a press, so a click that starts and ends between two
// RunFrames is still reported down for one frame.
struct LatchedButton
{
    std::atomic<bool> down{false}, pressed{false};
    void Set(bool v)
    {
        down = v;
        if (v)
            pressed = true;
    }
    // exchange(false) reads the latch and clears it in one atomic step.
    bool Read() { return pressed.exchange(false) || down; }
    void Reset() { down = pressed = false; }
};

// What the mouse did since vrserver's thread last looked. The mouse thread writes it and
// RunFrame consumes it with exchange(0) (read and reset in one step, so no movement is lost
// between the two threads). Every field is atomic, so no lock is needed.
struct MouseState
{
    std::atomic<bool> active{false}; // laser mode wanted (toggled by the toggle button)
    std::atomic<int> dx{0}, dy{0};
    std::atomic<int> wheel{0}, hwheel{0};
    LatchedButton left, right, middle, back;

    void Clear()
    {
        left.Reset();
        right.Reset();
        middle.Reset();
        back.Reset();
        dx = 0;
        dy = 0;
        wheel = 0;
        hwheel = 0;
    }
};

enum class EventAction
{
    None,
    Toggle, // the toggle button was released: the caller grabs or releases the mouse
    Resync, // the kernel lost events: the caller reads the buttons' state and calls SyncButtons
};

// Applies one evdev event (struct input_event from <linux/input.h>) to `m`.
inline EventAction HandleEvent(MouseState &m, const Settings &s, const input_event &ev)
{
    if (ev.type == EV_KEY && ev.code == s.toggleButton)
    {
        // Toggle on release: gamescope then sees a whole click (switching on) or nothing
        // (switching off, the press is already grabbed), never half of one.
        return ev.value == 0 ? EventAction::Toggle : EventAction::None;
    }
    if (!m.active)
        return EventAction::None;
    if (ev.type == EV_REL)
    {
        if (ev.code == REL_X)
            m.dx += ev.value;
        else if (ev.code == REL_Y)
            m.dy += ev.value;
        else if (ev.code == REL_WHEEL)
            m.wheel += ev.value;
        else if (ev.code == REL_HWHEEL)
            m.hwheel += ev.value;
    }
    else if (ev.type == EV_KEY)
    {
        if (ev.value == 2) // key autorepeat
            return EventAction::None;
        const bool down = ev.value != 0;
        if (ev.code == BTN_LEFT)
            m.left.Set(down);
        else if (ev.code == BTN_RIGHT)
            m.right.Set(down);
        else if (ev.code == BTN_MIDDLE)
            m.middle.Set(down);
        else if (ev.code == s.backButton)
            m.back.Set(down);
    }
    return EventAction::None;
}

// The events of one open device. Adds one case to HandleEvent: if the reader falls behind, the
// kernel throws events away and sends SYN_DROPPED. What follows, up to the next SYN_REPORT, is
// incomplete and has to be ignored, and the buttons read afresh: a lost release would otherwise
// leave a button held down. (A lost toggle-button click is just lost; press it again.)
class EventStream
{
public:
    EventAction Handle(MouseState &m, const Settings &s, const input_event &ev)
    {
        if (ev.type == EV_SYN && ev.code == SYN_DROPPED)
        {
            m_dropping = true;
            return EventAction::None;
        }
        if (m_dropping)
        {
            if (ev.type != EV_SYN || ev.code != SYN_REPORT)
                return EventAction::None;
            m_dropping = false;
            return EventAction::Resync;
        }
        return HandleEvent(m, s, ev);
    }

private:
    bool m_dropping = false;
};

// The kernel describes an input device's capabilities as bitmaps: bit N of `key` is set if the
// device has key/button code N, and so on. Sized as the kernel's EVIOCGBIT ioctl expects.
constexpr std::size_t BitmapLongs(int maxCode)
{
    return static_cast<std::size_t>(maxCode) / (8 * sizeof(unsigned long)) + 1;
}

inline bool TestBit(const unsigned long *bits, int bit)
{
    const auto b = static_cast<unsigned>(bit);
    const unsigned bitsPerLong = sizeof(unsigned long) * 8;
    return ((bits[b / bitsPerLong] >> (b % bitsPerLong)) & 1UL) != 0;
}

// One /dev/input/event* node, as reported by EVIOCGNAME, EVIOCGID and EVIOCGBIT.
struct InputDeviceInfo
{
    std::string name;
    unsigned vendor = 0;
    unsigned long ev[BitmapLongs(EV_MAX)] = {};   // event types (EV_REL, EV_KEY, ...)
    unsigned long rel[BitmapLongs(REL_MAX)] = {}; // relative axes (REL_X, REL_WHEEL, ...)
    unsigned long key[BitmapLongs(KEY_MAX)] = {}; // keys and buttons (BTN_LEFT, ...)
};

constexpr unsigned k_valveVendorId = 0x28de;

// After EventAction::Resync: sets the buttons from `keys`, the device's key bitmap as it is now
// (EVIOCGKEY). Only while active, like HandleEvent.
inline void SyncButtons(MouseState &m, const Settings &s, const unsigned long *keys)
{
    if (!m.active)
        return;
    m.left.Set(TestBit(keys, BTN_LEFT));
    m.right.Set(TestBit(keys, BTN_RIGHT));
    m.middle.Set(TestBit(keys, BTN_MIDDLE));
    if (s.backButton >= 1 && s.backButton <= KEY_MAX)
        m.back.Set(TestBit(keys, s.backButton));
}

// How well an input device fits as the mouse to use, from worst to best. The driver looks at
// every /dev/input/event* node and uses the best one (on a tie, the first it finds).
enum class MouseFit
{
    NotAMouse,      // no relative X/Y motion or no left button: keyboards, touchpads, ...
    ValveDevice,    // Valve's own emulated mice ("Steam Ctrl (BT) ... Mouse"), unless deviceNameFilter is set
    FilteredOut,    // a mouse whose name doesn't contain deviceNameFilter
    NoToggleButton, // a mouse without the toggle button: usable, but laser mode can't be switched on
    Good,
};

inline MouseFit RateMouse(const InputDeviceInfo &d, const Settings &s)
{
    if (!TestBit(d.ev, EV_REL) || !TestBit(d.ev, EV_KEY) || !TestBit(d.rel, REL_X) || !TestBit(d.rel, REL_Y) ||
        !TestBit(d.key, BTN_LEFT))
        return MouseFit::NotAMouse;
    if (s.nameFilter.empty() && (d.vendor == k_valveVendorId || d.name.find("Steam") != std::string::npos))
        return MouseFit::ValveDevice;
    if (!s.nameFilter.empty() && d.name.find(s.nameFilter) == std::string::npos)
        return MouseFit::FilteredOut;
    const bool hasToggle = s.toggleButton >= 1 && s.toggleButton <= KEY_MAX && TestBit(d.key, s.toggleButton);
    return hasToggle ? MouseFit::Good : MouseFit::NoToggleButton;
}

// Switching laser mode on and off, one RunFrame at a time.
//
// On the Frame, a click of the grip TOGGLES SteamVR's laser between "Laser Mouse" and "Gamepad"
// mode. So the grip is pulsed exactly once on the way in and once on the way out, which always
// puts the mode back where it started (0.5.1 held it instead: the "needs several presses" bug).
//
//   ON:  hand role -> wait claimDelayMs -> grip pulse (gamepad -> laser) -> bumper pulse
//        (bound only to SwitchLaserHand: moves the laser to us) -> On.
//   OFF: grip pulse while still a hand (laser -> gamepad, the stock default) -> let the
//        release land -> back to the parked role.
class LaserSession
{
public:
    enum class Phase
    {
        Off,
        Settle,
        Grip,
        Claim,
        On,
        Restore,
        Release
    };
    static constexpr const char *k_phaseNames[] = {"off", "settle",        "grip pulse", "claim pulse",
                                                   "on",  "restore pulse", "release"};
    static_assert(sizeof(k_phaseNames) / sizeof(k_phaseNames[0]) == static_cast<std::size_t>(Phase::Release) + 1,
                  "one name per phase");

    // Advances at most one phase. `want` is whether laser mode is wanted (MouseState::active).
    // Returns true if the phase changed.
    bool Step(bool want, const Settings &s, TimePoint now)
    {
        const auto elapsed = now - m_phaseAt;
        const Ms pulse(s.pulseMs);
        switch (m_phase)
        {
        case Phase::Off:
            if (want)
                return Enter(Phase::Settle, now);
            break;
        case Phase::Settle:
            if (!want)
                return Enter(Phase::Off, now); // nothing pulsed yet, so nothing to undo
            if (elapsed >= Ms(s.claimDelayMs))
                return Enter(Phase::Grip, now);
            break;
        case Phase::Grip:
            if (elapsed >= pulse)
                return Enter(Phase::Claim, now);
            break;
        case Phase::Claim:
            if (elapsed >= pulse)
                return Enter(Phase::On, now);
            break;
        case Phase::On:
            if (!want)
                return Enter(Phase::Restore, now);
            break;
        case Phase::Restore:
            if (elapsed >= pulse)
                return Enter(Phase::Release, now);
            break;
        case Phase::Release:
            if (elapsed >= pulse)
                return Enter(Phase::Off, now);
            break;
        }
        return false;
    }

    Phase GetPhase() const { return m_phase; }
    const char *PhaseName() const { return k_phaseNames[static_cast<int>(m_phase)]; }

    // The controller role the device should have right now: `role` while parked, the hand
    // role from the moment switching on starts until switching off has finished.
    int Role(const Settings &s) const
    {
        if (m_phase == Phase::Off)
            return s.role;
        return s.activeRole > 0 ? s.activeRole : s.role;
    }

    bool Grip() const { return m_phase == Phase::Grip || m_phase == Phase::Restore; }
    bool Bumper() const { return m_phase == Phase::Claim; }
    // The ray follows the mouse (otherwise it is parked pointing at the sky).
    bool Aiming() const
    {
        return m_phase == Phase::Settle || m_phase == Phase::Grip || m_phase == Phase::Claim || m_phase == Phase::On;
    }
    // Mouse buttons and wheel reach SteamVR.
    bool Live(bool want) const { return m_phase == Phase::On && want; }

private:
    bool Enter(Phase p, TimePoint now)
    {
        m_phase = p;
        m_phaseAt = now;
        return true;
    }

    Phase m_phase = Phase::Off;
    TimePoint m_phaseAt;
};

// Ray direction as yaw (about world up, Y) and pitch (about X), in radians. World-locked:
// turning the head moves the ray's origin but not its direction.
struct Aim
{
    float yaw = 0, pitch = 0;
};

// While laser mode is off the ray points straight up, where it hits nothing.
constexpr Aim k_parkedAim{0.f, k_pi / 2};

// The aim that points the ray along `forward`, a unit vector (the HMD's -Z axis in world space).
inline Aim AimAlong(float fx, float fy, float fz)
{
    return {std::atan2(-fx, -fz), std::asin(std::fmax(-1.f, std::fmin(1.f, fy)))};
}

// Mouse counts -> rotation. Right turns right, down turns down (unless invertY). Pitch is
// clamped short of straight up or down, where yaw would stop meaning anything.
inline void ApplyMouse(Aim &a, int dx, int dy, const Settings &s)
{
    const float k = s.sensitivityDeg * k_pi / 180.f;
    a.yaw -= static_cast<float>(dx) * k;
    a.pitch -= static_cast<float>(dy) * k * (s.invertY ? -1.f : 1.f);
    const float limit = 89.f * k_pi / 180.f;
    a.pitch = std::fmax(-limit, std::fmin(limit, a.pitch));
}

struct Quat
{
    float w = 1, x = 0, y = 0, z = 0;
};

// Orientation for the device: q = yaw(Y) * pitch(X), built from half-angle sines and cosines.
// OpenVR controllers point along their -Z axis, so this aims the ray.
inline Quat RayRotation(const Aim &a)
{
    const float cy = std::cos(a.yaw / 2), sy = std::sin(a.yaw / 2);
    const float cp = std::cos(a.pitch / 2), sp = std::sin(a.pitch / 2);
    return {cy * cp, cy * sp, sy * cp, -sy * sp};
}

} // namespace mouselaser

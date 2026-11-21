// SPDX-License-Identifier: MIT
// core.h: the driver's decision-making, kept free of SteamVR and of system calls so it can be
// unit tested on any Linux machine (tests/unit_tests.cpp). driver.cpp does the I/O: it reads the
// settings and the mouse, talks to SteamVR, and calls into the pieces below.
//
//   Settings      every tunable, with its default
//   StickStepper  wheel notches -> thumbstick deflection over time
//   LatchedButton a click shorter than one frame still shows up for one frame
//   MouseState    what the mouse did since the last frame (shared between threads)
//   HandleEvent   one evdev event -> MouseState
//   IsWantedMouse which /dev/input device to use
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
#include <string>

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
    // Stylus (5) gets its own /user/stylus path. A hand role (1/2) shares the slot with the
    // real controller of that hand, so the device disconnects while off (pre-0.5.0 behaviour).
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

// True if `d` looks like the mouse to use: relative X/Y motion and a left button. Valve's own
// emulated mice (e.g. "Steam Ctrl (BT) ... Mouse") are skipped unless nameFilter asks for them.
inline bool IsWantedMouse(const InputDeviceInfo &d, const std::string &nameFilter)
{
    if (nameFilter.empty() && (d.vendor == k_valveVendorId || d.name.find("Steam") != std::string::npos))
        return false;
    if (!TestBit(d.ev, EV_REL) || !TestBit(d.ev, EV_KEY))
        return false;
    if (!TestBit(d.rel, REL_X) || !TestBit(d.rel, REL_Y) || !TestBit(d.key, BTN_LEFT))
        return false;
    return nameFilter.empty() || d.name.find(nameFilter) != std::string::npos;
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

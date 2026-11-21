// SPDX-License-Identifier: MIT
// Unit tests for src/core.h, the driver's logic, without SteamVR or a mouse.
// Build with -DMOUSELASER_TESTS=ON and run `ctest --test-dir build`. See docs/testing.md.
//
// Time is simulated (tests move a TimePoint forward by hand), so everything runs instantly
// and gives the same result on every machine.

#include <doctest/doctest.h> // main() is in doctest_main.cpp

#include "core.h"

#include <cmath>
#include <fstream>
#include <map>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace mouselaser;
using std::chrono::microseconds;
using Phase = LaserSession::Phase;

namespace
{

constexpr microseconds k_frame{8333}; // one frame at 120 Hz
const TimePoint k_start{std::chrono::seconds(1000)};

// ---------------------------------------------------------------------------------------------
// LaserSession: the on/off hand-over. This is where the "needs several presses" bug lived.

// Steps a LaserSession frame by frame and records what SteamVR would have seen.
struct SessionSim
{
    Settings s;
    LaserSession session;
    TimePoint now = k_start;
    std::vector<Phase> entered; // every phase entered, in order
    int gripPulses = 0, bumperPulses = 0, onEntries = 0;
    bool gripWhileNotHand = false; // a grip pulse while the device didn't hold the hand role
    bool lastGrip = false, lastBumper = false;

    void Frame(bool want, microseconds dt = k_frame)
    {
        now += dt;
        if (session.Step(want, s, now))
        {
            entered.push_back(session.GetPhase());
            if (session.GetPhase() == Phase::On)
                onEntries++;
        }
        if (session.Grip() && !lastGrip)
            gripPulses++;
        if (session.Bumper() && !lastBumper)
            bumperPulses++;
        if (session.Grip() && session.Role(s) != s.activeRole)
            gripWhileNotHand = true;
        lastGrip = session.Grip();
        lastBumper = session.Bumper();
    }

    void Run(bool want, Ms duration)
    {
        const TimePoint end = now + duration;
        while (now < end)
            Frame(want);
    }
};

TEST_CASE("switching on: hand role, wait, one grip pulse, one bumper pulse, on")
{
    SessionSim sim;
    CHECK(sim.session.Role(sim.s) == k_roleStylus);
    sim.Frame(true);
    CHECK(sim.session.GetPhase() == Phase::Settle);
    CHECK(sim.session.Role(sim.s) == k_roleLeftHand); // the hand role comes first...
    CHECK_FALSE(sim.session.Grip());                  // ...and the grip waits for claimDelayMs

    sim.Run(true, Ms(500));
    CHECK(sim.entered == std::vector<Phase>{Phase::Settle, Phase::Grip, Phase::Claim, Phase::On});
    CHECK(sim.gripPulses == 1);
    CHECK(sim.bumperPulses == 1);
    CHECK(sim.session.Role(sim.s) == k_roleLeftHand);
    CHECK(sim.session.Aiming());
    CHECK_FALSE(sim.session.Grip());
}

TEST_CASE("the grip waits claimDelayMs and each pulse lasts pulseMs")
{
    SessionSim sim;
    sim.s.claimDelayMs = 100;
    sim.s.pulseMs = 60;
    sim.Frame(true);
    const TimePoint settleAt = sim.now;
    TimePoint gripAt{}, claimAt{};
    while (sim.session.GetPhase() != Phase::On)
    {
        sim.Frame(true);
        if (sim.session.GetPhase() == Phase::Grip && gripAt == TimePoint{})
            gripAt = sim.now;
        if (sim.session.GetPhase() == Phase::Claim && claimAt == TimePoint{})
            claimAt = sim.now;
    }
    CHECK(gripAt - settleAt >= Ms(100));
    CHECK(gripAt - settleAt < Ms(100) + k_frame);
    CHECK(claimAt - gripAt >= Ms(60));
    CHECK(claimAt - gripAt < Ms(60) + k_frame);
}

TEST_CASE("switching off: a second grip pulse while still a hand, then the parked role")
{
    SessionSim sim;
    sim.Run(true, Ms(500));
    sim.entered.clear();
    sim.Run(false, Ms(500));
    CHECK(sim.entered == std::vector<Phase>{Phase::Restore, Phase::Release, Phase::Off});
    CHECK(sim.gripPulses == 2); // one on, one off: the Laser Mouse/Gamepad mode is back where it was
    CHECK_FALSE(sim.gripWhileNotHand);
    CHECK(sim.session.Role(sim.s) == k_roleStylus);
    CHECK_FALSE(sim.session.Aiming());
}

TEST_CASE("switching off before the grip pulse has nothing to undo")
{
    SessionSim sim;
    sim.Frame(true);
    sim.Frame(false);
    CHECK(sim.entered == std::vector<Phase>{Phase::Settle, Phase::Off});
    CHECK(sim.gripPulses == 0);
    CHECK(sim.session.Role(sim.s) == k_roleStylus);
}

TEST_CASE("switching off after the grip pulse finishes the sequence, so the pulses pair up")
{
    SessionSim sim;
    while (sim.session.GetPhase() != Phase::Grip)
        sim.Frame(true);
    sim.Run(false, Ms(500));
    CHECK(sim.entered == std::vector<Phase>{Phase::Settle, Phase::Grip, Phase::Claim, Phase::On, Phase::Restore,
                                            Phase::Release, Phase::Off});
    CHECK(sim.gripPulses == 2);
}

TEST_CASE("any on/off sequence leaves an even number of grip pulses whenever it is back to off")
{
    std::mt19937 rng(12345); // fixed seed: the same "random" sequences on every run
    std::uniform_int_distribution<int> holdMs(0, 400), frameUs(4000, 25000), delayMs(0, 300), pulseMs(1, 150);
    for (int run = 0; run < 200; run++)
    {
        SessionSim sim;
        sim.s.claimDelayMs = delayMs(rng);
        sim.s.pulseMs = pulseMs(rng);
        bool want = false;
        for (int toggle = 0; toggle < 30; toggle++)
        {
            want = !want;
            const TimePoint until = sim.now + Ms(holdMs(rng));
            while (sim.now < until)
            {
                sim.Frame(want, microseconds(frameUs(rng)));
                if (sim.session.GetPhase() == Phase::Off)
                {
                    INFO("run " << run << ", claimDelayMs " << sim.s.claimDelayMs << ", pulseMs " << sim.s.pulseMs);
                    REQUIRE(sim.gripPulses == 2 * sim.onEntries);
                    REQUIRE(sim.bumperPulses == sim.onEntries);
                }
            }
        }
        CHECK_FALSE(sim.gripWhileNotHand);
    }
}

TEST_CASE("buttons and wheel reach SteamVR only once fully on")
{
    SessionSim sim;
    sim.Frame(true);
    while (sim.session.GetPhase() != Phase::On)
    {
        CHECK_FALSE(sim.session.Live(true));
        sim.Frame(true);
    }
    CHECK(sim.session.Live(true));
    CHECK_FALSE(sim.session.Live(false));
}

TEST_CASE("activeRole 0 keeps the parked role while on")
{
    SessionSim sim;
    sim.s.activeRole = 0;
    sim.Run(true, Ms(500));
    CHECK(sim.session.GetPhase() == Phase::On);
    CHECK(sim.session.Role(sim.s) == k_roleStylus);
}

// ---------------------------------------------------------------------------------------------
// Aim: ray direction maths.

struct Vec
{
    float x, y, z;
};

// Rotates v by the unit quaternion q.
Vec Rotate(const Quat &q, const Vec &v)
{
    // v' = v + 2w (u x v) + 2 u x (u x v), with u = (x, y, z)
    const Vec u{q.x, q.y, q.z};
    const Vec t{2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x)};
    return {v.x + q.w * t.x + (u.y * t.z - u.z * t.y), v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
            v.z + q.w * t.z + (u.x * t.y - u.y * t.x)};
}

// Where the device's ray points: OpenVR controllers point along -Z.
Vec RayDirection(const Aim &a) { return Rotate(RayRotation(a), {0, 0, -1}); }

TEST_CASE("the ray starts where the user is looking")
{
    const std::vector<Vec> forwards = {
        {0, 0, -1},          {1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0.3f, 0.4f, -0.866f}, {-0.5f, -0.5f, 0.707f},
        {0.1f, 0.95f, 0.2f},
    };
    for (Vec f : forwards)
    {
        const float len = std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
        f = {f.x / len, f.y / len, f.z / len};
        const Vec d = RayDirection(AimAlong(f.x, f.y, f.z));
        INFO("forward " << f.x << ", " << f.y << ", " << f.z);
        CHECK(d.x == doctest::Approx(f.x).epsilon(1e-4));
        CHECK(d.y == doctest::Approx(f.y).epsilon(1e-4));
        CHECK(d.z == doctest::Approx(f.z).epsilon(1e-4));
    }
}

TEST_CASE("the parked ray points straight up")
{
    const Vec d = RayDirection(k_parkedAim);
    CHECK(d.x == doctest::Approx(0).epsilon(1e-6));
    CHECK(d.y == doctest::Approx(1));
    CHECK(d.z == doctest::Approx(0).epsilon(1e-6));
}

TEST_CASE("mouse right turns the ray right, mouse down turns it down")
{
    Settings s; // 0.05 degrees per count
    Aim a;
    ApplyMouse(a, 100, 0, s);
    CHECK(a.yaw == doctest::Approx(-5 * k_pi / 180)); // 100 counts = 5 degrees
    CHECK(RayDirection(a).x > 0);                     // OpenVR: +X is right

    a = Aim{};
    ApplyMouse(a, 0, 100, s); // evdev: +Y is down
    CHECK(RayDirection(a).y < 0);

    s.invertY = true;
    a = Aim{};
    ApplyMouse(a, 0, 100, s);
    CHECK(RayDirection(a).y > 0);
}

TEST_CASE("pitch stops short of straight up and straight down")
{
    Settings s;
    Aim a;
    ApplyMouse(a, 0, -1000000, s);
    CHECK(a.pitch == doctest::Approx(89 * k_pi / 180));
    ApplyMouse(a, 0, 2000000, s);
    CHECK(a.pitch == doctest::Approx(-89 * k_pi / 180));
}

// ---------------------------------------------------------------------------------------------
// HandleEvent and LatchedButton: evdev events -> MouseState.

input_event Ev(int type, int code, int value)
{
    input_event e{};
    e.type = static_cast<decltype(e.type)>(type);
    e.code = static_cast<decltype(e.code)>(code);
    e.value = value;
    return e;
}

TEST_CASE("the toggle fires on release, not on press")
{
    MouseState m;
    const Settings s;
    CHECK(HandleEvent(m, s, Ev(EV_KEY, BTN_EXTRA, 1)) == EventAction::None);
    CHECK(HandleEvent(m, s, Ev(EV_KEY, BTN_EXTRA, 0)) == EventAction::Toggle);
    CHECK_FALSE(m.active); // the caller grabs the mouse and flips `active`

    m.active = true; // the toggle works in both states
    CHECK(HandleEvent(m, s, Ev(EV_KEY, BTN_EXTRA, 1)) == EventAction::None);
    CHECK(HandleEvent(m, s, Ev(EV_KEY, BTN_EXTRA, 0)) == EventAction::Toggle);
}

TEST_CASE("while inactive, movement and buttons are ignored")
{
    MouseState m;
    const Settings s;
    HandleEvent(m, s, Ev(EV_REL, REL_X, 5));
    HandleEvent(m, s, Ev(EV_REL, REL_WHEEL, 1));
    HandleEvent(m, s, Ev(EV_KEY, BTN_LEFT, 1));
    CHECK(m.dx == 0);
    CHECK(m.wheel == 0);
    CHECK_FALSE(m.left.Read());
}

TEST_CASE("while active, movement and wheel add up until read")
{
    MouseState m;
    const Settings s;
    m.active = true;
    HandleEvent(m, s, Ev(EV_REL, REL_X, 3));
    HandleEvent(m, s, Ev(EV_REL, REL_X, 4));
    HandleEvent(m, s, Ev(EV_REL, REL_Y, -2));
    HandleEvent(m, s, Ev(EV_REL, REL_WHEEL, 1));
    HandleEvent(m, s, Ev(EV_REL, REL_WHEEL, 1));
    HandleEvent(m, s, Ev(EV_REL, REL_HWHEEL, -1));
    CHECK(m.dx.exchange(0) == 7);
    CHECK(m.dx == 0);
    CHECK(m.dy == -2);
    CHECK(m.wheel == 2);
    CHECK(m.hwheel == -1);
}

TEST_CASE("buttons map to left, right, middle and back")
{
    MouseState m;
    Settings s;
    m.active = true;
    HandleEvent(m, s, Ev(EV_KEY, BTN_LEFT, 1));
    HandleEvent(m, s, Ev(EV_KEY, BTN_RIGHT, 1));
    HandleEvent(m, s, Ev(EV_KEY, BTN_MIDDLE, 1));
    HandleEvent(m, s, Ev(EV_KEY, BTN_SIDE, 1));
    CHECK(m.left.Read());
    CHECK(m.right.Read());
    CHECK(m.middle.Read());
    CHECK(m.back.Read());

    SUBCASE("backButton -1 disables Back")
    {
        s.backButton = -1;
        MouseState m2;
        m2.active = true;
        HandleEvent(m2, s, Ev(EV_KEY, BTN_SIDE, 1));
        CHECK_FALSE(m2.back.Read());
    }
}

TEST_CASE("key autorepeat is ignored")
{
    MouseState m;
    const Settings s;
    m.active = true;
    HandleEvent(m, s, Ev(EV_KEY, BTN_LEFT, 2));
    CHECK_FALSE(m.left.Read());
}

TEST_CASE("a click shorter than a frame is still seen, exactly once")
{
    LatchedButton b;
    b.Set(true);
    b.Set(false); // pressed and released before the frame read it
    CHECK(b.Read());
    CHECK_FALSE(b.Read());

    b.Set(true); // held: down on every read
    CHECK(b.Read());
    CHECK(b.Read());
    b.Set(false);
    CHECK_FALSE(b.Read());
}

TEST_CASE("Clear drops pending movement and clicks")
{
    MouseState m;
    const Settings s;
    m.active = true;
    HandleEvent(m, s, Ev(EV_REL, REL_X, 3));
    HandleEvent(m, s, Ev(EV_KEY, BTN_LEFT, 1));
    m.Clear();
    CHECK(m.dx == 0);
    CHECK_FALSE(m.left.Read());
}

// SYN_DROPPED: the kernel lost events because the reader fell behind.
TEST_CASE("after SYN_DROPPED, events are ignored up to the next SYN_REPORT, then a resync is asked for")
{
    MouseState m;
    m.active = true;
    const Settings s;
    EventStream events;
    CHECK(events.Handle(m, s, Ev(EV_REL, REL_X, 5)) == EventAction::None);
    CHECK(events.Handle(m, s, Ev(EV_SYN, SYN_DROPPED, 0)) == EventAction::None);
    CHECK(events.Handle(m, s, Ev(EV_REL, REL_X, 100)) == EventAction::None);   // incomplete: ignored
    CHECK(events.Handle(m, s, Ev(EV_KEY, BTN_EXTRA, 0)) == EventAction::None); // even the toggle
    CHECK(events.Handle(m, s, Ev(EV_SYN, SYN_REPORT, 0)) == EventAction::Resync);
    CHECK(m.dx.exchange(0) == 5);
    // back to normal afterwards
    CHECK(events.Handle(m, s, Ev(EV_REL, REL_X, 7)) == EventAction::None);
    CHECK(m.dx.exchange(0) == 7);
    CHECK(events.Handle(m, s, Ev(EV_KEY, BTN_EXTRA, 0)) == EventAction::Toggle);
}

TEST_CASE("a button release lost to SYN_DROPPED doesn't leave the button held")
{
    MouseState m;
    m.active = true;
    Settings s;
    EventStream events;
    events.Handle(m, s, Ev(EV_KEY, BTN_LEFT, 1));
    events.Handle(m, s, Ev(EV_KEY, BTN_SIDE, 1)); // back
    CHECK(m.left.Read());                         // both presses are seen (and their latches read)
    CHECK(m.back.Read());
    // The releases are among the lost events; the kernel's key state says both are up now and
    // the right button is down.
    events.Handle(m, s, Ev(EV_SYN, SYN_DROPPED, 0));
    events.Handle(m, s, Ev(EV_KEY, BTN_LEFT, 0));
    REQUIRE(events.Handle(m, s, Ev(EV_SYN, SYN_REPORT, 0)) == EventAction::Resync);
    unsigned long keys[BitmapLongs(KEY_MAX)] = {};
    keys[BTN_RIGHT / (8 * sizeof(unsigned long))] |= 1UL << (BTN_RIGHT % (8 * sizeof(unsigned long)));
    SyncButtons(m, s, keys);
    CHECK_FALSE(m.left.Read());
    CHECK_FALSE(m.back.Read());
    CHECK(m.right.Read());

    SUBCASE("nothing changes while inactive")
    {
        m.Clear();
        m.active = false;
        SyncButtons(m, s, keys);
        CHECK_FALSE(m.right.Read());
    }
    SUBCASE("backButton -1 is left alone")
    {
        s.backButton = -1;
        m.back.Set(true);
        SyncButtons(m, s, keys);
        CHECK(m.back.Read());
    }
}

TEST_CASE("no mouse movement is lost between the reader thread and the frame thread")
{
    // Also the test to run under ThreadSanitizer (-DMOUSELASER_SANITIZE=thread).
    MouseState m;
    const Settings s;
    m.active = true;
    constexpr int k_events = 200000;
    std::atomic<bool> done{false};
    std::thread reader([&] {
        for (int i = 0; i < k_events; i++)
            HandleEvent(m, s, Ev(EV_REL, REL_X, 1));
        done = true;
    });
    long total = 0;
    while (!done)
        total += m.dx.exchange(0);
    reader.join();
    total += m.dx.exchange(0);
    CHECK(total == k_events);
}

// ---------------------------------------------------------------------------------------------
// RateMouse: which /dev/input device to use.

void SetBit(unsigned long *bits, int bit)
{
    const auto b = static_cast<unsigned>(bit);
    bits[b / (8 * sizeof(unsigned long))] |= 1UL << (b % (8 * sizeof(unsigned long)));
}

InputDeviceInfo Mouse(const std::string &name, unsigned vendor = 0x1234)
{
    InputDeviceInfo d;
    d.name = name;
    d.vendor = vendor;
    for (int t : {EV_SYN, EV_KEY, EV_REL, EV_MSC})
        SetBit(d.ev, t);
    for (int r : {REL_X, REL_Y, REL_WHEEL, REL_HWHEEL})
        SetBit(d.rel, r);
    for (int k : {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA})
        SetBit(d.key, k);
    return d;
}

MouseFit Rate(const InputDeviceInfo &d, const std::string &nameFilter = "")
{
    Settings s;
    s.nameFilter = nameFilter;
    return RateMouse(d, s);
}

void ClearBit(unsigned long *bits, int bit)
{
    const auto b = static_cast<unsigned>(bit);
    bits[b / (8 * sizeof(unsigned long))] &= ~(1UL << (b % (8 * sizeof(unsigned long))));
}

TEST_CASE("an ordinary mouse is picked") { CHECK(Rate(Mouse("MCHOSE G3 A Mouse")) == MouseFit::Good); }

TEST_CASE("Valve's emulated mice are skipped unless the name filter asks for them")
{
    CHECK(Rate(Mouse("Valve Software Steam Controller", k_valveVendorId)) == MouseFit::ValveDevice);
    CHECK(Rate(Mouse("Steam Ctrl (BT) Mouse")) == MouseFit::ValveDevice);
    CHECK(Rate(Mouse("Steam Ctrl (BT) Mouse"), "Steam Ctrl") == MouseFit::Good);
}

TEST_CASE("a mouse with the toggle button beats one without")
{
    InputDeviceInfo threeButtons = Mouse("Basic Mouse");
    ClearBit(threeButtons.key, BTN_SIDE);
    ClearBit(threeButtons.key, BTN_EXTRA);
    CHECK(Rate(threeButtons) == MouseFit::NoToggleButton);
    CHECK(Rate(Mouse("MCHOSE G3 A Mouse")) > Rate(threeButtons));
    // but it is still a usable mouse, better than anything skipped
    CHECK(Rate(threeButtons) > MouseFit::FilteredOut);
    CHECK(Rate(threeButtons) > MouseFit::ValveDevice);

    Settings s; // a different toggleButton is looked for instead
    s.toggleButton = BTN_MIDDLE;
    CHECK(RateMouse(threeButtons, s) == MouseFit::Good);
}

TEST_CASE("devices that aren't mice are skipped")
{
    SUBCASE("a keyboard's consumer-control node: a wheel but no X/Y")
    {
        InputDeviceInfo d;
        d.name = "Keyboard Consumer Control";
        SetBit(d.ev, EV_KEY);
        SetBit(d.ev, EV_REL);
        SetBit(d.rel, REL_HWHEEL);
        SetBit(d.key, KEY_VOLUMEUP);
        CHECK(Rate(d) == MouseFit::NotAMouse);
    }
    SUBCASE("a touchpad: absolute axes, no relative ones")
    {
        InputDeviceInfo d;
        d.name = "Touchpad";
        SetBit(d.ev, EV_KEY);
        SetBit(d.ev, EV_ABS);
        SetBit(d.key, BTN_LEFT);
        CHECK(Rate(d) == MouseFit::NotAMouse);
    }
    SUBCASE("relative motion but no left button")
    {
        InputDeviceInfo d = Mouse("Odd device");
        ClearBit(d.key, BTN_LEFT);
        CHECK(Rate(d) == MouseFit::NotAMouse);
    }
}

TEST_CASE("deviceNameFilter picks by substring")
{
    CHECK(Rate(Mouse("Logitech MX Master 3"), "MX Master") == MouseFit::Good);
    CHECK(Rate(Mouse("Logitech MX Master 3"), "G502") == MouseFit::FilteredOut);
}

// ---------------------------------------------------------------------------------------------
// StickStepper: wheel notches -> thumbstick, at 120 Hz.

// Feeds `notches` (frame -> ticks) for `frames` frames; returns the stick value of every frame.
std::vector<float> Wheel(const Settings &s, const std::map<int, int> &notches, int frames)
{
    StickStepper st;
    std::vector<float> out;
    TimePoint now = k_start;
    for (int f = 0; f < frames; f++)
    {
        const auto it = notches.find(f);
        out.push_back(st.Update(it == notches.end() ? 0 : it->second, s, now));
        now += k_frame;
    }
    return out;
}

int FramesAtLeast(const std::vector<float> &v, float threshold)
{
    int n = 0;
    for (float x : v)
        n += std::fabs(x) >= threshold ? 1 : 0;
    return n;
}

int Flicks(const std::vector<float> &v) // number of separate pushes away from centre
{
    int n = 0;
    float prev = 0;
    for (float x : v)
    {
        if (x != 0 && prev == 0)
            n++;
        prev = x;
    }
    return n;
}

std::map<int, int> Spin(int everyFrames, int until)
{
    std::map<int, int> m;
    for (int f = 2; f < until; f += everyFrames)
        m[f] = 1;
    return m;
}

TEST_CASE("smooth: a single notch is one clear push that eases back to exactly zero")
{
    const Settings s; // smooth is the default
    const auto v = Wheel(s, {{2, 1}}, 60);
    CHECK(FramesAtLeast(v, 0.5f) >= 5); // long enough for the UI to register one step
    CHECK(Flicks(v) == 1);
    CHECK(v.back() == 0.f);
}

TEST_CASE("smooth: spinning at 10 notches/s holds the stick")
{
    const Settings s;
    const auto v = Wheel(s, Spin(12, 110), 130);
    CHECK(FramesAtLeast(v, 0.5f) >= 100);
}

TEST_CASE("smooth: never beyond wheelDeflection")
{
    Settings s;
    s.wheelDeflection = 0.6f;
    const auto v = Wheel(s, {{2, 30}, {3, 30}, {4, 30}}, 20);
    for (float x : v)
        CHECK(std::fabs(x) <= 0.6f);
}

TEST_CASE("smooth: reversing direction flips the stick at once")
{
    const Settings s;
    const auto v = Wheel(s, {{2, 2}, {8, -1}}, 50);
    CHECK(v[7] > 0);
    CHECK(v[8] < 0);
}

TEST_CASE("step: a single notch is one push of wheelPressMs")
{
    Settings s;
    s.wheelSmooth = false;
    const auto v = Wheel(s, {{2, 1}}, 40);
    CHECK(Flicks(v) == 1);
    const int pushed = FramesAtLeast(v, 0.5f);
    CHECK(pushed >= 10); // 90 ms at 120 Hz is 10.8 frames
    CHECK(pushed <= 11);
}

TEST_CASE("step: notches queue up, capped at wheelMaxQueued")
{
    Settings s;
    s.wheelSmooth = false;
    const auto v = Wheel(s, {{2, 50}}, 600);
    CHECK(Flicks(v) == s.wheelMaxQueued);
}

TEST_CASE("step: reversing direction drops the queue")
{
    Settings s;
    s.wheelSmooth = false;
    const auto v = Wheel(s, {{2, 5}, {8, -1}}, 200);
    int positive = 0, negative = 0;
    float prev = 0;
    for (float x : v)
    {
        if (x > 0 && prev <= 0)
            positive++;
        if (x < 0 && prev >= 0)
            negative++;
        prev = x;
    }
    CHECK(positive == 1);
    CHECK(negative == 1);
}

TEST_CASE("Reset drops queued notches")
{
    Settings s;
    s.wheelSmooth = false;
    StickStepper st;
    TimePoint now = k_start;
    st.Update(5, s, now);
    st.Reset();
    for (int f = 0; f < 200; f++)
    {
        now += k_frame;
        CHECK(st.Update(0, s, now) == 0.f);
    }
}

// ---------------------------------------------------------------------------------------------
// Settings: the defaults in core.h must match the ones SteamVR loads from default.vrsettings, and
// Validate() keeps bad values from the file harmless.

// Reads the flat "key": value pairs of default.vrsettings.
std::map<std::string, std::string> ReadDefaultSettings()
{
    std::ifstream file(MOUSELASER_SOURCE_DIR "/driver/mouselaser/resources/settings/default.vrsettings");
    REQUIRE(file.good());
    std::stringstream text;
    text << file.rdbuf();
    const std::string str = text.str();
    std::map<std::string, std::string> values;
    const std::regex pair(R"re("(\w+)"\s*:\s*([^,\n}]+))re");
    for (auto it = std::sregex_iterator(str.begin(), str.end(), pair); it != std::sregex_iterator(); ++it)
        values[(*it)[1]] = std::regex_replace((*it)[2].str(), std::regex(R"re(^\s+|\s+$|")re"), "");
    return values;
}

TEST_CASE("Settings defaults match default.vrsettings")
{
    const auto v = ReadDefaultSettings();
    const Settings d;
    auto get = [&](const char *key) {
        INFO("key " << key);
        REQUIRE(v.count(key) == 1);
        return v.at(key);
    };
    auto num = [&](const char *key) { return std::stod(get(key)); };

    CHECK(num("sensitivity") == doctest::Approx(d.sensitivityDeg));
    CHECK(num("toggleButton") == d.toggleButton);
    CHECK(num("backButton") == d.backButton);
    CHECK(num("role") == d.role);
    CHECK(num("activeRole") == d.activeRole);
    CHECK(get("deviceNameFilter") == d.nameFilter);
    CHECK(num("claimDelayMs") == d.claimDelayMs);
    CHECK(num("pulseMs") == d.pulseMs);
    CHECK(num("reconnectGraceS") == doctest::Approx(d.reconnectGraceS));
    CHECK(num("originOffsetY") == doctest::Approx(d.originOffsetY));
    CHECK((get("invertY") == "true") == d.invertY);
    CHECK((get("wheelMode") != "step") == d.wheelSmooth);
    CHECK(num("wheelDeflection") == doctest::Approx(d.wheelDeflection));
    CHECK(num("wheelSmoothMin") == doctest::Approx(d.wheelSmoothMin));
    CHECK(num("wheelSmoothImpulse") == doctest::Approx(d.wheelSmoothImpulse));
    CHECK(num("wheelSmoothHoldMs") == d.wheelSmoothHoldMs);
    CHECK(num("wheelSmoothDecayMs") == d.wheelSmoothDecayMs);
    CHECK(num("wheelPressMs") == d.wheelPressMs);
    CHECK(num("wheelReleaseMs") == d.wheelReleaseMs);
    CHECK(num("wheelMaxQueued") == d.wheelMaxQueued);
}

TEST_CASE("Describe names every setting in default.vrsettings")
{
    const std::string line = Describe(Settings{});
    for (const auto &kv : ReadDefaultSettings())
    {
        // the section's name, and `enable` (read before the others, logged on its own when false)
        if (kv.first == "driver_mouselaser" || kv.first == "enable")
            continue;
        INFO("key " << kv.first);
        CHECK(line.find(kv.first + " ") != std::string::npos);
    }
    CHECK(line.find("sensitivity 0.05,") != std::string::npos);
    CHECK(line.find("invertY false,") != std::string::npos);
    CHECK(line.find("wheelMode smooth,") != std::string::npos);
}

TEST_CASE("the defaults pass Validate unchanged")
{
    Settings s;
    CHECK(Validate(s).empty());
}

TEST_CASE("Validate clamps numbers to their range and says so")
{
    Settings s;
    s.sensitivityDeg = 5;
    s.pulseMs = -20;
    s.wheelMaxQueued = 0;
    s.wheelDeflection = 0.8f;
    s.wheelSmoothMin = 0.9f; // above the maximum deflection
    const auto problems = Validate(s);
    CHECK(problems.size() == 4);
    CHECK(s.sensitivityDeg == 1.f);
    CHECK(s.pulseMs == 10);
    CHECK(s.wheelMaxQueued == 1);
    CHECK(s.wheelSmoothMin == 0.8f);
    REQUIRE_FALSE(problems.empty());
    CHECK(problems[0] == "sensitivity 5 is outside 0.001 to 1, using 1");
    CHECK(Validate(s).empty()); // once fixed, nothing more to say
}

TEST_CASE("Validate puts back the default for a role that doesn't exist")
{
    Settings s;
    s.role = 0;       // Invalid
    s.activeRole = 3; // OptOut
    CHECK(Validate(s).size() == 2);
    CHECK(s.role == k_roleStylus);
    CHECK(s.activeRole == k_roleLeftHand);

    s.role = k_roleRightHand; // every real choice is kept
    s.activeRole = 0;
    CHECK(Validate(s).empty());
}

TEST_CASE("Validate keeps the buttons usable")
{
    Settings s;
    SUBCASE("the left button can't be the toggle")
    {
        s.toggleButton = BTN_LEFT;
        CHECK(Validate(s).size() == 1);
        CHECK(s.toggleButton == BTN_EXTRA);
    }
    SUBCASE("a code that isn't a key")
    {
        s.toggleButton = 99999;
        s.backButton = -5;
        CHECK(Validate(s).size() == 2);
        CHECK(s.toggleButton == BTN_EXTRA);
        CHECK(s.backButton == BTN_SIDE);
    }
    SUBCASE("toggle moved to the back button: Back is switched off rather than unreachable")
    {
        s.toggleButton = BTN_SIDE; // backButton is still its default, BTN_SIDE
        const auto problems = Validate(s);
        REQUIRE(problems.size() == 1);
        CHECK(problems[0] == "backButton 275 is already the toggle or a click button, using -1");
        CHECK(s.backButton == -1);
    }
    SUBCASE("Back on a button that already clicks")
    {
        s.backButton = BTN_RIGHT;
        CHECK(Validate(s).size() == 1);
        CHECK(s.backButton == -1);
    }
    SUBCASE("-1 and a free button are fine")
    {
        s.backButton = -1;
        CHECK(Validate(s).empty());
        s.backButton = BTN_FORWARD;
        CHECK(Validate(s).empty());
    }
}

// Describe()'s "key value, key value, ..." as a map.
std::map<std::string, std::string> Described(const Settings &s)
{
    std::map<std::string, std::string> values;
    std::stringstream line(Describe(s));
    std::string pair;
    while (std::getline(line, pair, ','))
    {
        pair.erase(0, pair.find_first_not_of(' '));
        const auto space = pair.find(' ');
        values[pair.substr(0, space)] = pair.substr(space + 1);
    }
    return values;
}

TEST_CASE("ApplyLive changes every setting it can and names the ones that need a restart")
{
    Settings current; // the defaults
    Settings fresh;   // every setting different
    fresh.sensitivityDeg = 0.08f;
    fresh.toggleButton = BTN_SIDE;
    fresh.backButton = -1;
    fresh.role = k_roleLeftHand;
    fresh.activeRole = k_roleRightHand;
    fresh.nameFilter = "MX";
    fresh.originOffsetY = -0.1f;
    fresh.invertY = true;
    fresh.claimDelayMs = 100;
    fresh.pulseMs = 80;
    fresh.reconnectGraceS = 30;
    fresh.wheelSmooth = false;
    fresh.wheelDeflection = 0.9f;
    fresh.wheelSmoothMin = 0.6f;
    fresh.wheelSmoothImpulse = 0.2f;
    fresh.wheelSmoothHoldMs = 50;
    fresh.wheelSmoothDecayMs = 100;
    fresh.wheelPressMs = 70;
    fresh.wheelReleaseMs = 40;
    fresh.wheelMaxQueued = 5;
    const auto before = Described(current);
    const auto wanted = Described(fresh);
    for (const auto &kv : wanted) // a new setting must be changed above too
    {
        INFO("setting " << kv.first);
        CHECK(kv.second != before.at(kv.first));
    }

    const std::vector<std::string> needRestart = ApplyLive(current, fresh);
    CHECK(needRestart == std::vector<std::string>{"toggleButton", "backButton", "role", "activeRole",
                                                  "deviceNameFilter", "reconnectGraceS"});
    // Every setting either changed now or is named as needing a restart; none is silently ignored.
    std::set<std::string> unchanged;
    for (const auto &kv : Described(current))
    {
        if (kv.second != wanted.at(kv.first))
            unchanged.insert(kv.first);
    }
    CHECK(unchanged == std::set<std::string>(needRestart.begin(), needRestart.end()));
}

TEST_CASE("ApplyLive with the same settings names nothing")
{
    Settings current;
    CHECK(ApplyLive(current, Settings{}).empty());
}

} // namespace

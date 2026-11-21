// SPDX-License-Identifier: MIT
// mouselaser: a virtual SteamVR controller whose pose is aimed by a physical mouse,
// so the SteamVR laser pointer can be driven by the mouse across every overlay.
//
// Toggle with the mouse's forward side button (BTN_EXTRA by default, on release). While
// active the mouse is grabbed (EVIOCGRAB) so gamescope stops seeing it. Switching on and
// off pulses the grip once each (SteamVR's Laser Mouse <-> Gamepad toggle), so the mode
// always ends up where it started. While inactive the ray is parked at the sky and, as a
// stylus, the device reports itself disconnected.
// It is a stylus while off, so it never takes a real controller's hand slot, and switches to
// a hand role while on, because the compositor only drags overlays with hands (or the head).
//
// Layout:
//   core.h              the logic (settings, wheel, laser hand-over, aim maths); unit tested
//   this file           the I/O around it:
//     LoadSettings        reads the driver_mouselaser section of steamvr.vrsettings
//     MouseReader         finds, grabs and reads the mouse, on its own thread
//     MouseLaserDevice    the virtual controller SteamVR sees; RunFrame updates it every frame
//     MouseLaserProvider  the object vrserver loads: creates the device, forwards RunFrame
//     HmdDriverFactory    the only exported function; vrserver looks it up by name
//
// Threads: vrserver's thread calls everything in the provider and the device (RunFrame,
// GetPose, ...). MouseReader runs one more thread, which only writes into a MouseState made
// of atomics. Nothing else is shared between them.
//
// EXPERIMENTAL and AI-generated ("vibecoded"); see README.md before relying on it.

#include <openvr_driver.h>

#include "core.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

using namespace vr;
using namespace mouselaser;

static_assert(k_roleLeftHand == TrackedControllerRole_LeftHand && k_roleRightHand == TrackedControllerRole_RightHand &&
                  k_roleStylus == TrackedControllerRole_Stylus,
              "core.h role numbers must match OpenVR's ETrackedControllerRole");

// The version comes from project() in CMakeLists.txt, which passes it in as MOUSELASER_VERSION.
#ifndef MOUSELASER_VERSION
#error "MOUSELASER_VERSION is not set: build with CMake (see the README)"
#endif

// `static` at file scope: visible only inside this file, not exported from the .so.
static constexpr const char *k_section = "driver_mouselaser";
static constexpr const char *k_version = MOUSELASER_VERSION;

// printf-style logging into vrserver.txt (SteamVR prefixes each line with "mouselaser: ").
// The format attribute makes the compiler check every call's arguments against `fmt`.
__attribute__((format(printf, 1, 2))) static void Log(const char *fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    static_cast<void>(vsnprintf(buf, sizeof(buf), fmt, args)); // a longer message is cut short, which is fine
    va_end(args);
    if (VRDriverLog())
        VRDriverLog()->Log(buf);
}

// The text for an errno value. strerror() may share one buffer between threads; strerror_r
// (the GNU version, which returns the message) doesn't, and the mouse thread logs errors too.
static std::string ErrorText(int err)
{
    char buf[128] = {};
    return strerror_r(err, buf, sizeof(buf));
}

// Reads the driver_mouselaser section of steamvr.vrsettings. ReadSetting(key, out) overwrites `out`
// with the value of `key`, if SteamVR has one: set by the user (in the file or on SteamVR's settings
// page), or else taken from driver/mouselaser/resources/settings/default.vrsettings. Otherwise
// `out` keeps its default from core.h's Settings. One overload per value type.
// Every key is in default.vrsettings, so an error means the user's value couldn't be read (a wrong
// type, for example). That goes into `problems` rather than straight into the log: the settings
// are read again every second, and a problem should be logged when it appears, not every second.
class SettingsReader
{
public:
    std::vector<std::string> problems;

    void ReadSetting(const char *key, float &out)
    {
        EVRSettingsError err = VRSettingsError_None;
        const float value = VRSettings()->GetFloat(k_section, key, &err);
        if (Ok(key, err))
            out = value;
    }

    void ReadSetting(const char *key, int &out)
    {
        EVRSettingsError err = VRSettingsError_None;
        const int32_t value = VRSettings()->GetInt32(k_section, key, &err);
        if (Ok(key, err))
            out = value;
    }

    void ReadSetting(const char *key, bool &out)
    {
        EVRSettingsError err = VRSettingsError_None;
        const bool value = VRSettings()->GetBool(k_section, key, &err);
        if (Ok(key, err))
            out = value;
    }

    void ReadSetting(const char *key, std::string &out)
    {
        EVRSettingsError err = VRSettingsError_None;
        char value[256] = {};
        VRSettings()->GetString(k_section, key, value, sizeof(value), &err);
        if (Ok(key, err))
            out = value;
    }

private:
    bool Ok(const char *key, EVRSettingsError err)
    {
        if (err == VRSettingsError_None)
            return true;
        problems.push_back(std::string(key) + " can't be read (" + VRSettings()->GetSettingsErrorNameFromEnum(err) +
                           "), using the default");
        return false;
    }
};

// Reads and validates every setting. `problems` gets one line per value that couldn't be read or
// was corrected.
static Settings LoadSettings(std::vector<std::string> &problems)
{
    SettingsReader r;
    Settings s;
    r.ReadSetting("sensitivity", s.sensitivityDeg);
    r.ReadSetting("toggleButton", s.toggleButton);
    r.ReadSetting("backButton", s.backButton);
    r.ReadSetting("role", s.role);
    r.ReadSetting("activeRole", s.activeRole);
    r.ReadSetting("deviceNameFilter", s.nameFilter);
    r.ReadSetting("claimDelayMs", s.claimDelayMs);
    r.ReadSetting("pulseMs", s.pulseMs);
    r.ReadSetting("reconnectGraceS", s.reconnectGraceS);
    r.ReadSetting("originOffsetY", s.originOffsetY);
    r.ReadSetting("invertY", s.invertY);
    std::string wheelMode = "smooth";
    r.ReadSetting("wheelMode", wheelMode);
    s.wheelSmooth = wheelMode != "step";
    if (wheelMode != "smooth" && wheelMode != "step")
        r.problems.push_back("wheelMode " + wheelMode + " is not smooth or step, using smooth");
    r.ReadSetting("wheelDeflection", s.wheelDeflection);
    r.ReadSetting("wheelSmoothMin", s.wheelSmoothMin);
    r.ReadSetting("wheelSmoothImpulse", s.wheelSmoothImpulse);
    r.ReadSetting("wheelSmoothHoldMs", s.wheelSmoothHoldMs);
    r.ReadSetting("wheelSmoothDecayMs", s.wheelSmoothDecayMs);
    r.ReadSetting("wheelPressMs", s.wheelPressMs);
    r.ReadSetting("wheelReleaseMs", s.wheelReleaseMs);
    r.ReadSetting("wheelMaxQueued", s.wheelMaxQueued);
    problems = r.problems;
    for (const std::string &problem : Validate(s))
        problems.push_back(problem);
    return s;
}

static void LogProblems(const std::vector<std::string> &problems)
{
    for (const std::string &problem : problems)
        Log("setting %s\n", problem.c_str());
}

// Finds the mouse, grabs it while laser mode is on, and feeds its events into a MouseState.
// It runs on its own thread, so a slow or vanished device never stalls vrserver.
class MouseReader
{
public:
    explicit MouseReader(MouseState &out) : m_out(out) {}

    // Destroying a joinable std::thread calls std::terminate (taking vrserver down),
    // so make sure the thread is stopped even if Deactivate() was never called.
    ~MouseReader() { Stop(); }
    MouseReader(const MouseReader &) = delete; // owns a thread and a file descriptor: no copies
    MouseReader &operator=(const MouseReader &) = delete;

    void Start(const Settings &s)
    {
        m_settings = s;
        m_run = true;
        m_thread = std::thread(&MouseReader::Loop, this);
    }

    void Stop()
    {
        m_run = false;
        if (m_thread.joinable())
            m_thread.join();
        Close();
        m_out.active = false;
    }

private:
    MouseState &m_out;
    Settings m_settings;
    std::atomic<bool> m_run{false};
    std::thread m_thread;
    int m_fd = -1;
    EventStream m_events; // the open device's events
    bool m_lost = false;  // mouse vanished while active; laser mode kept for reconnectGraceS
    TimePoint m_lostAt;
    std::set<std::string> m_reported; // skipped mice already logged, as "path name"

    // Asks the kernel for a device's name, vendor and capability bitmaps.
    static bool ReadDeviceInfo(int fd, InputDeviceInfo &d)
    {
        char n[256] = {};
        if (ioctl(fd, EVIOCGNAME(sizeof(n)), n) < 0)
            return false;
        d.name = n;
        input_id id = {};
        ioctl(fd, EVIOCGID, &id);
        d.vendor = id.vendor;
        if (ioctl(fd, EVIOCGBIT(0, sizeof(d.ev)), d.ev) < 0)
            return false;
        ioctl(fd, EVIOCGBIT(EV_REL, sizeof(d.rel)), d.rel);
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(d.key)), d.key);
        return true;
    }

    // Logs a mouse that was skipped on purpose, once per device, so that a deviceNameFilter that
    // doesn't match shows up in the log.
    void ReportSkipped(const std::string &path, const std::string &name, MouseFit fit)
    {
        if (fit != MouseFit::ValveDevice && fit != MouseFit::FilteredOut)
            return;
        if (!m_reported.insert(path + " " + name).second) // already in the set
            return;
        Log("skipped %s (%s): %s\n", path.c_str(), name.c_str(),
            fit == MouseFit::ValveDevice ? "Valve device" : "name doesn't contain deviceNameFilter");
    }

    // Looks at every /dev/input/event* node and opens the one RateMouse likes best: a mouse with
    // the toggle button over one without (on a tie, the first found). False if there is no mouse.
    bool Open()
    {
        DIR *dir = opendir("/dev/input");
        if (!dir)
            return false;
        int bestFd = -1;
        MouseFit bestFit = MouseFit::NotAMouse;
        std::string bestPath, bestName;
        while (dirent *e = readdir(dir)) // NOLINT(concurrency-mt-unsafe): this DIR is used by one thread only
        {
            if (strncmp(e->d_name, "event", 5) != 0)
                continue;
            std::string path = std::string("/dev/input/") + e->d_name;
            const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0)
                continue;
            InputDeviceInfo info;
            const MouseFit fit = ReadDeviceInfo(fd, info) ? RateMouse(info, m_settings) : MouseFit::NotAMouse;
            ReportSkipped(path, info.name, fit);
            if (fit >= MouseFit::NoToggleButton && fit > bestFit)
            {
                if (bestFd >= 0)
                    close(bestFd);
                bestFd = fd;
                bestFit = fit;
                bestPath = path;
                bestName = info.name;
            }
            else
            {
                close(fd);
            }
        }
        closedir(dir);
        if (bestFd < 0)
            return false;

        m_fd = bestFd;
        m_events = EventStream(); // a fresh device has nothing pending
        Log("using %s (%s)\n", bestPath.c_str(), bestName.c_str());
        if (bestFit == MouseFit::NoToggleButton)
            Log("WARNING: it has no button %d (toggleButton), so laser mode can't be switched on. "
                "Set toggleButton, or deviceNameFilter to use another device\n",
                m_settings.toggleButton);
        if (m_out.active && ioctl(m_fd, EVIOCGRAB, 1) < 0)
        {
            Log("EVIOCGRAB failed after reconnect (%s)\n", ErrorText(errno).c_str());
            SetActive(false);
        }
        else if (m_lost)
        {
            Log("mouse back, laser mode kept\n");
        }
        m_lost = false;
        return true;
    }

    void Close()
    {
        if (m_fd >= 0)
        {
            ioctl(m_fd, EVIOCGRAB, 0);
            close(m_fd);
            m_fd = -1;
        }
        m_out.Clear();
    }

    // The mouse vanished (unplugged, or a Bluetooth mouse napping). Keep laser mode for a
    // while, so a short nap doesn't throw the user out of it.
    void Lost()
    {
        Close();
        if (!m_out.active)
            return;
        m_lost = true;
        m_lostAt = Clock::now();
        Log("mouse gone, keeping laser mode for %.0f s\n", static_cast<double>(m_settings.reconnectGraceS));
    }

    // EVIOCGRAB gives this process exclusive use of the mouse: gamescope stops receiving it.
    void SetActive(bool on)
    {
        if (m_out.active == on)
            return;
        if (m_fd >= 0 && ioctl(m_fd, EVIOCGRAB, on ? 1 : 0) < 0 && on)
        {
            Log("EVIOCGRAB failed (%s), staying inactive\n", ErrorText(errno).c_str());
            return;
        }
        m_out.Clear();
        m_out.active = on;
        m_lost = false;
        Log("%s\n", on ? "ACTIVE (mouse drives the laser)" : "inactive (mouse back to desktop)");
    }

    // The kernel dropped events because this thread fell behind (EventStream). Read the
    // buttons as they are now, so a lost release doesn't leave one held down.
    void Resync()
    {
        unsigned long keys[BitmapLongs(KEY_MAX)] = {};
        if (ioctl(m_fd, EVIOCGKEY(sizeof(keys)), keys) >= 0)
            SyncButtons(m_out, m_settings, keys);
        Log("events lost (SYN_DROPPED), buttons re-read\n");
    }

    void Loop()
    {
        while (m_run)
        {
            if (m_fd < 0 && m_lost &&
                Clock::now() - m_lostAt > std::chrono::duration<float>(m_settings.reconnectGraceS))
            {
                Log("mouse still gone, leaving laser mode\n");
                SetActive(false);
            }
            if (m_fd < 0 && !Open())
            {
                // No mouse: look again in 2 s, waking every 100 ms so Stop() isn't kept waiting.
                for (int i = 0; i < 20 && m_run; i++)
                    usleep(100 * 1000);
                continue;
            }
            pollfd pfd = {m_fd, POLLIN, 0};
            int r = poll(&pfd, 1, 200);
            if (r <= 0)
                continue;
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            {
                Log("mouse disappeared\n");
                Lost();
                continue;
            }
            input_event ev{};
            ssize_t n = 0;
            while ((n = read(m_fd, &ev, sizeof(ev))) == sizeof(ev))
            {
                switch (m_events.Handle(m_out, m_settings, ev))
                {
                case EventAction::Toggle:
                    SetActive(!m_out.active);
                    break;
                case EventAction::Resync:
                    Resync();
                    break;
                case EventAction::None:
                    break;
                }
            }
            if (n < 0 && errno == ENODEV)
            {
                Log("mouse disappeared\n");
                Lost();
            }
        }
    }
};

// The virtual controller. SteamVR calls Activate once, then RunFrame (through the provider)
// every frame. `final`: nothing derives from it. `override`: the compiler checks that each
// method really matches one in OpenVR's ITrackedDeviceServerDriver interface.
class MouseLaserDevice final : public ITrackedDeviceServerDriver
{
public:
    explicit MouseLaserDevice(Settings s) : m_settings(std::move(s)) {}

    EVRInitError Activate(uint32_t id) override
    {
        m_id = id;
        PropertyContainerHandle_t c = VRProperties()->TrackedDeviceToPropertyContainer(id);
        m_container = c;
        VRProperties()->SetStringProperty(c, Prop_ModelNumber_String, "Mouse Laser (virtual)");
        VRProperties()->SetStringProperty(c, Prop_ManufacturerName_String, "frame-unboundedMouse-vibed");
        VRProperties()->SetStringProperty(c, Prop_ControllerType_String, "mouselaser");
        VRProperties()->SetStringProperty(c, Prop_InputProfilePath_String,
                                          "{mouselaser}/input/mouselaser_profile.json");
        VRProperties()->SetStringProperty(c, Prop_RenderModelName_String, "{mouselaser}mouselaser_none");
        VRProperties()->SetInt32Property(c, Prop_ControllerRoleHint_Int32, m_settings.role);
        m_currentRole = m_settings.role;
        VRProperties()->SetBoolProperty(c, Prop_DeviceProvidesBatteryStatus_Bool, false);

        // The inputs SteamVR's bindings can see. Paths must match input/mouselaser_profile.json.
        VRDriverInput()->CreateBooleanComponent(c, "/input/trigger/click", &m_trigger);
        VRDriverInput()->CreateScalarComponent(c, "/input/trigger/value", &m_triggerValue, VRScalarType_Absolute,
                                               VRScalarUnits_NormalizedOneSided);
        VRDriverInput()->CreateBooleanComponent(c, "/input/a/click", &m_a);
        VRDriverInput()->CreateBooleanComponent(c, "/input/b/click", &m_b);
        VRDriverInput()->CreateBooleanComponent(c, "/input/back/click", &m_back);
        VRDriverInput()->CreateBooleanComponent(c, "/input/grip/click", &m_grip);
        VRDriverInput()->CreateBooleanComponent(c, "/input/bumper/click", &m_bumper);
        VRDriverInput()->CreateBooleanComponent(c, "/input/thumbstick/touch", &m_stickTouch);
        VRDriverInput()->CreateScalarComponent(c, "/input/thumbstick/x", &m_stickX, VRScalarType_Absolute,
                                               VRScalarUnits_NormalizedTwoSided);
        VRDriverInput()->CreateScalarComponent(c, "/input/thumbstick/y", &m_stickY, VRScalarType_Absolute,
                                               VRScalarUnits_NormalizedTwoSided);

        m_mouse.Start(m_settings);
        Log("activated as device %u, role %d\n", id, m_settings.role);
        return VRInitError_None;
    }

    void Deactivate() override
    {
        m_mouse.Stop();
        m_id = k_unTrackedDeviceIndexInvalid;
    }

    void EnterStandby() override {}
    void *GetComponent(const char * /*componentNameAndVersion*/) override { return nullptr; }
    void DebugRequest(const char * /*request*/, char *resp, uint32_t size) override
    {
        if (size)
            resp[0] = 0;
    }

    DriverPose_t GetPose() override
    {
        const std::scoped_lock lock(m_poseMutex); // unlocks when `lock` goes out of scope
        return m_pose;
    }

    // Settings changed while running: applies the ones that can change now (see ApplyLive in
    // core.h) and returns the names of those that need a SteamVR restart.
    std::vector<std::string> ApplySettings(const Settings &fresh)
    {
        std::vector<std::string> needRestart = ApplyLive(m_settings, fresh);
        m_stepX.Reset(); // the wheel may have changed mode or timing in the middle of a scroll
        m_stepY.Reset();
        return needRestart;
    }

    const Settings &CurrentSettings() const { return m_settings; }

    void RunFrame()
    {
        if (m_id == k_unTrackedDeviceIndexInvalid)
            return;

        TrackedDevicePose_t hmd = {};
        VRServerDriverHost()->GetRawTrackedDevicePoses(0, &hmd, 1); // device 0 is the HMD
        const HmdMatrix34_t &m = hmd.mDeviceToAbsoluteTracking;     // 3x4: rotation | position

        const TimePoint now = Clock::now();
        const bool want = m_input.active;
        if (m_session.Step(want, m_settings, now))
        {
            SetRole(m_session.Role(m_settings));
            if (m_session.GetPhase() == LaserSession::Phase::Settle && hmd.bPoseIsValid)
            {
                // Switching on: start the ray where the user is looking (the HMD's -Z axis).
                m_aim = AimAlong(-m.m[0][2], -m.m[1][2], -m.m[2][2]);
            }
            Log("phase %s\n", m_session.PhaseName());
        }
        const bool live = m_session.Live(want);

        // Connected whenever laser mode is (or is being switched) on. While parked, a stylus
        // DISCONNECTS (IsRoleAllowedAsHand(Stylus) is false) and a hand role stays connected.
        // This is the tested behaviour, so it is kept as is.
        const bool parked = m_session.GetPhase() == LaserSession::Phase::Off;
        const bool present = !parked || IsRoleAllowedAsHand(ETrackedControllerRole(m_settings.role));

        ApplyMouse(m_aim, m_input.dx.exchange(0), m_input.dy.exchange(0), m_settings);

        DriverPose_t pose = {};
        pose.qWorldFromDriverRotation.w = 1;
        pose.qDriverFromHeadRotation.w = 1;
        pose.deviceIsConnected = present;
        pose.poseIsValid = present && hmd.bPoseIsValid;
        pose.result = pose.poseIsValid ? TrackingResult_Running_OK : TrackingResult_Running_OutOfRange;
        // Position: the HMD's, lowered by originOffsetY.
        pose.vecPosition[0] = m.m[0][3];
        pose.vecPosition[1] = m.m[1][3] + m_settings.originOffsetY;
        pose.vecPosition[2] = m.m[2][3];
        const Quat q = RayRotation(m_session.Aiming() ? m_aim : k_parkedAim);
        pose.qRotation.w = q.w;
        pose.qRotation.x = q.x;
        pose.qRotation.y = q.y;
        pose.qRotation.z = q.z;
        {
            const std::scoped_lock lock(m_poseMutex);
            m_pose = pose;
        }
        VRServerDriverHost()->TrackedDevicePoseUpdated(m_id, pose, sizeof(DriverPose_t));

        // Wheel notches become thumbstick flicks: vertical wheel -> Y, tilt wheel -> X.
        if (!live)
        {
            m_stepY.Reset();
            m_stepX.Reset();
        }
        const float stickY = m_stepY.Update(m_input.wheel.exchange(0), m_settings, now);
        const float stickX = m_stepX.Update(m_input.hwheel.exchange(0), m_settings, now);

        // Read() every frame so stale latched presses are dropped while not live.
        const bool l = m_input.left.Read() && live;
        VRDriverInput()->UpdateBooleanComponent(m_trigger, l, 0);
        VRDriverInput()->UpdateScalarComponent(m_triggerValue, l ? 1.f : 0.f, 0);
        VRDriverInput()->UpdateBooleanComponent(m_a, m_input.right.Read() && live, 0);
        VRDriverInput()->UpdateBooleanComponent(m_b, m_input.middle.Read() && live, 0);
        VRDriverInput()->UpdateBooleanComponent(m_back, m_input.back.Read() && live, 0);
        VRDriverInput()->UpdateBooleanComponent(m_grip, m_session.Grip(), 0);
        VRDriverInput()->UpdateBooleanComponent(m_bumper, m_session.Bumper(), 0);
        VRDriverInput()->UpdateBooleanComponent(m_stickTouch, stickX != 0.f || stickY != 0.f, 0);
        VRDriverInput()->UpdateScalarComponent(m_stickX, stickX, 0);
        VRDriverInput()->UpdateScalarComponent(m_stickY, stickY, 0);
    }

private:
    void SetRole(int role)
    {
        if (role == m_currentRole)
            return;
        m_currentRole = role;
        VRProperties()->SetInt32Property(m_container, Prop_ControllerRoleHint_Int32, role);
        Log("role %d\n", role);
    }

    Settings m_settings;
    // m_input is declared before m_mouse on purpose: members are destroyed in reverse order, so
    // the reader thread (inside m_mouse) is stopped before the state it writes to goes away.
    MouseState m_input;
    MouseReader m_mouse{m_input};
    LaserSession m_session;
    Aim m_aim;
    StickStepper m_stepX, m_stepY;
    uint32_t m_id = k_unTrackedDeviceIndexInvalid;
    PropertyContainerHandle_t m_container = k_ulInvalidPropertyContainer;
    int m_currentRole = 0;
    std::mutex m_poseMutex;
    DriverPose_t m_pose = {};
    VRInputComponentHandle_t m_trigger = 0, m_triggerValue = 0, m_a = 0, m_b = 0, m_back = 0, m_grip = 0, m_bumper = 0;
    VRInputComponentHandle_t m_stickTouch = 0, m_stickX = 0, m_stickY = 0;
};

// What vrserver loads. It calls Init once at startup, RunFrame every frame, Cleanup at exit.
class MouseLaserProvider : public IServerTrackedDeviceProvider
{
public:
    EVRInitError Init(IVRDriverContext *ctx) override
    {
        // OpenVR macro: sets up VRSettings(), VRDriverLog(), VRServerDriverHost() and the
        // other accessors used throughout this file.
        VR_INIT_SERVER_DRIVER_CONTEXT(ctx);
        Log("version %s\n", k_version);
        SettingsReader r;
        bool enable = true;
        r.ReadSetting("enable", enable);
        LogProblems(r.problems);
        if (!enable)
        {
            Log("disabled by setting\n");
            return VRInitError_None;
        }
        std::vector<std::string> problems;
        const Settings s = LoadSettings(problems);
        LogProblems(problems);
        Log("settings: %s\n", Describe(s).c_str());
        m_loadedSettings = Fingerprint(s, problems);
        // The provider owns the device; SteamVR only borrows the pointer until Cleanup().
        m_device = std::make_unique<MouseLaserDevice>(s);
        if (!VRServerDriverHost()->TrackedDeviceAdded("mouselaser-0", TrackedDeviceClass_Controller, m_device.get()))
            Log("TrackedDeviceAdded failed\n");
        return VRInitError_None;
    }

    void Cleanup() override
    {
        m_device.reset();
        VR_CLEANUP_SERVER_DRIVER_CONTEXT();
    }

    const char *const *GetInterfaceVersions() override { return k_InterfaceVersions; }

    void RunFrame() override
    {
        if (m_device)
            m_device->RunFrame();
        // Empty SteamVR's event queue. Only one event matters: a driver's setting changed, for
        // example on SteamVR's settings page. (Logging them all, 0.5.0 to 0.6.2, showed that none
        // says who owns the laser.)
        bool settingsEvent = false;
        VREvent_t ev{};
        while (VRServerDriverHost()->PollNextEvent(&ev, sizeof(ev)))
        {
            if (ev.eventType == VREvent_AnyDriverSettingsChanged)
                settingsEvent = true;
        }
        // The settings are also checked every second, in case that event doesn't reach drivers
        // (not yet verified on the Frame). Reading them is a lookup inside vrserver: cheap.
        const TimePoint now = Clock::now();
        if (m_device && (settingsEvent || now >= m_nextSettingsCheck))
        {
            m_nextSettingsCheck = now + std::chrono::seconds(1);
            ReloadSettings(settingsEvent ? "settings event" : "1 s check");
        }
    }

    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override {}
    void LeaveStandby() override {}

private:
    // What was loaded, as text, to tell whether anything changed since: the settings plus any
    // problems found reading them.
    static std::string Fingerprint(const Settings &s, const std::vector<std::string> &problems)
    {
        std::string text = Describe(s);
        for (const std::string &problem : problems)
            text += "\n" + problem;
        return text;
    }

    // Reads the settings again and, if anything changed, applies it and logs it. `trigger` says
    // what noticed the change, for the log.
    void ReloadSettings(const char *trigger)
    {
        std::vector<std::string> problems;
        const Settings fresh = LoadSettings(problems);
        std::string loaded = Fingerprint(fresh, problems);
        if (loaded == m_loadedSettings)
            return;
        m_loadedSettings = std::move(loaded);
        LogProblems(problems);
        const std::vector<std::string> needRestart = m_device->ApplySettings(fresh);
        Log("settings changed (%s): %s\n", trigger, Describe(m_device->CurrentSettings()).c_str());
        if (!needRestart.empty())
        {
            std::string names;
            for (const std::string &name : needRestart)
                names += (names.empty() ? "" : ", ") + name;
            Log("settings changed that apply after a SteamVR restart: %s\n", names.c_str());
        }
    }

    std::unique_ptr<MouseLaserDevice> m_device;
    std::string m_loadedSettings;
    TimePoint m_nextSettingsCheck = TimePoint::min(); // min(), unlike TimePoint{}, can't throw (g_provider is global)
};

// One provider for the lifetime of the loaded .so. It has to be a mutable global: vrserver gets a
// pointer to it from HmdDriverFactory and calls into it.
static MouseLaserProvider g_provider; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

// The driver's entry point. vrserver finds it by name with dlsym, so it is exported with C
// linkage (`extern "C"`: no C++ name mangling) and default visibility (everything else in the
// .so is hidden by -fvisibility=hidden in CMakeLists.txt).
extern "C" __attribute__((visibility("default"))) void *HmdDriverFactory(const char *iface, int *ret)
{
    if (strcmp(iface, IServerTrackedDeviceProvider_Version) == 0)
        return &g_provider;
    if (ret)
        *ret = VRInitError_Init_InterfaceNotFound;
    return nullptr;
}

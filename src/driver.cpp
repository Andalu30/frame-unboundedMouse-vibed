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
#include <map>
#include <mutex>
#include <string>
#include <thread>

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

// `static` at file scope: visible only inside this file, not exported from the .so.
static const char *k_section = "driver_mouselaser";
static const char *k_version = "0.6.2-experimental";

// printf-style logging into vrserver.txt (SteamVR prefixes each line with "mouselaser: ").
static void Log(const char *fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (VRDriverLog())
        VRDriverLog()->Log(buf);
}

// Reads the driver_mouselaser section. SteamVR fills in keys the user didn't set from
// driver/mouselaser/resources/settings/default.vrsettings; a key missing from both keeps the
// default from core.h's Settings.
static void LoadSettings(Settings &out)
{
    EVRSettingsError err;
    IVRSettings *s = VRSettings();
    float f = s->GetFloat(k_section, "sensitivity", &err);
    if (err == VRSettingsError_None) out.sensitivityDeg = f;
    int32_t i = s->GetInt32(k_section, "toggleButton", &err);
    if (err == VRSettingsError_None) out.toggleButton = i;
    i = s->GetInt32(k_section, "backButton", &err);
    if (err == VRSettingsError_None) out.backButton = i;
    i = s->GetInt32(k_section, "role", &err);
    if (err == VRSettingsError_None) out.role = i;
    i = s->GetInt32(k_section, "activeRole", &err);
    if (err == VRSettingsError_None) out.activeRole = i;
    char buf[256] = {};
    s->GetString(k_section, "deviceNameFilter", buf, sizeof(buf), &err);
    if (err == VRSettingsError_None) out.nameFilter = buf;
    f = s->GetFloat(k_section, "originOffsetY", &err);
    if (err == VRSettingsError_None) out.originOffsetY = f;
    bool b = s->GetBool(k_section, "invertY", &err);
    if (err == VRSettingsError_None) out.invertY = b;
    i = s->GetInt32(k_section, "wheelPressMs", &err);
    if (err == VRSettingsError_None) out.wheelPressMs = i;
    i = s->GetInt32(k_section, "wheelReleaseMs", &err);
    if (err == VRSettingsError_None) out.wheelReleaseMs = i;
    f = s->GetFloat(k_section, "wheelDeflection", &err);
    if (err == VRSettingsError_None) out.wheelDeflection = f;
    i = s->GetInt32(k_section, "wheelMaxQueued", &err);
    if (err == VRSettingsError_None) out.wheelMaxQueued = i;
    s->GetString(k_section, "wheelMode", buf, sizeof(buf), &err);
    if (err == VRSettingsError_None) out.wheelSmooth = strcmp(buf, "step") != 0;
    f = s->GetFloat(k_section, "wheelSmoothMin", &err);
    if (err == VRSettingsError_None) out.wheelSmoothMin = f;
    f = s->GetFloat(k_section, "wheelSmoothImpulse", &err);
    if (err == VRSettingsError_None) out.wheelSmoothImpulse = f;
    i = s->GetInt32(k_section, "wheelSmoothHoldMs", &err);
    if (err == VRSettingsError_None) out.wheelSmoothHoldMs = i;
    i = s->GetInt32(k_section, "wheelSmoothDecayMs", &err);
    if (err == VRSettingsError_None) out.wheelSmoothDecayMs = i;
    i = s->GetInt32(k_section, "claimDelayMs", &err);
    if (err == VRSettingsError_None) out.claimDelayMs = i;
    i = s->GetInt32(k_section, "pulseMs", &err);
    if (err == VRSettingsError_None) out.pulseMs = i;
    f = s->GetFloat(k_section, "reconnectGraceS", &err);
    if (err == VRSettingsError_None) out.reconnectGraceS = f;
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
    bool m_lost = false; // mouse vanished while active; laser mode kept for reconnectGraceS
    TimePoint m_lostAt;

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

    bool Open()
    {
        DIR *dir = opendir("/dev/input");
        if (!dir)
            return false;
        while (dirent *e = readdir(dir))
        {
            if (strncmp(e->d_name, "event", 5) != 0)
                continue;
            std::string path = std::string("/dev/input/") + e->d_name;
            int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0)
                continue;
            InputDeviceInfo info;
            if (ReadDeviceInfo(fd, info) && IsWantedMouse(info, m_settings.nameFilter))
            {
                m_fd = fd;
                Log("using %s (%s)\n", path.c_str(), info.name.c_str());
                if (m_out.active && ioctl(m_fd, EVIOCGRAB, 1) < 0)
                {
                    Log("EVIOCGRAB failed after reconnect (%s)\n", strerror(errno));
                    SetActive(false);
                }
                else if (m_lost)
                    Log("mouse back, laser mode kept\n");
                m_lost = false;
                closedir(dir);
                return true;
            }
            close(fd);
        }
        closedir(dir);
        return false;
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
            Log("EVIOCGRAB failed (%s), staying inactive\n", strerror(errno));
            return;
        }
        m_out.Clear();
        m_out.active = on;
        m_lost = false;
        Log("%s\n", on ? "ACTIVE (mouse drives the laser)" : "inactive (mouse back to desktop)");
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
            input_event ev;
            ssize_t n;
            while ((n = read(m_fd, &ev, sizeof(ev))) == sizeof(ev))
            {
                if (HandleEvent(m_out, m_settings, ev) == EventAction::Toggle)
                    SetActive(!m_out.active);
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
    explicit MouseLaserDevice(const Settings &s) : m_settings(s) {}

    EVRInitError Activate(uint32_t id) override
    {
        m_id = id;
        PropertyContainerHandle_t c = VRProperties()->TrackedDeviceToPropertyContainer(id);
        m_container = c;
        VRProperties()->SetStringProperty(c, Prop_ModelNumber_String, "Mouse Laser (virtual)");
        VRProperties()->SetStringProperty(c, Prop_ManufacturerName_String, "frame-unboundedMouse-vibed");
        VRProperties()->SetStringProperty(c, Prop_ControllerType_String, "mouselaser");
        VRProperties()->SetStringProperty(c, Prop_InputProfilePath_String, "{mouselaser}/input/mouselaser_profile.json");
        VRProperties()->SetStringProperty(c, Prop_RenderModelName_String, "{mouselaser}mouselaser_none");
        VRProperties()->SetInt32Property(c, Prop_ControllerRoleHint_Int32, m_settings.role);
        m_currentRole = m_settings.role;
        VRProperties()->SetBoolProperty(c, Prop_DeviceProvidesBatteryStatus_Bool, false);

        // The inputs SteamVR's bindings can see. Paths must match input/mouselaser_profile.json.
        VRDriverInput()->CreateBooleanComponent(c, "/input/trigger/click", &m_trigger);
        VRDriverInput()->CreateScalarComponent(c, "/input/trigger/value", &m_triggerValue,
                                               VRScalarType_Absolute, VRScalarUnits_NormalizedOneSided);
        VRDriverInput()->CreateBooleanComponent(c, "/input/a/click", &m_a);
        VRDriverInput()->CreateBooleanComponent(c, "/input/b/click", &m_b);
        VRDriverInput()->CreateBooleanComponent(c, "/input/back/click", &m_back);
        VRDriverInput()->CreateBooleanComponent(c, "/input/grip/click", &m_grip);
        VRDriverInput()->CreateBooleanComponent(c, "/input/bumper/click", &m_bumper);
        VRDriverInput()->CreateBooleanComponent(c, "/input/thumbstick/touch", &m_stickTouch);
        VRDriverInput()->CreateScalarComponent(c, "/input/thumbstick/x", &m_stickX,
                                               VRScalarType_Absolute, VRScalarUnits_NormalizedTwoSided);
        VRDriverInput()->CreateScalarComponent(c, "/input/thumbstick/y", &m_stickY,
                                               VRScalarType_Absolute, VRScalarUnits_NormalizedTwoSided);

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
    void *GetComponent(const char *) override { return nullptr; }
    void DebugRequest(const char *, char *resp, uint32_t size) override
    {
        if (size)
            resp[0] = 0;
    }

    DriverPose_t GetPose() override
    {
        std::lock_guard<std::mutex> lock(m_poseMutex); // unlocks when `lock` goes out of scope
        return m_pose;
    }

    uint32_t Id() const { return m_id; }
    bool Active() const { return m_session.GetPhase() != LaserSession::Phase::Off; }

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
            std::lock_guard<std::mutex> lock(m_poseMutex);
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
        Settings s;
        LoadSettings(s);
        EVRSettingsError err;
        bool enable = VRSettings()->GetBool(k_section, "enable", &err);
        if (err == VRSettingsError_None && !enable)
        {
            Log("disabled by setting\n");
            return VRInitError_None;
        }
        m_device = new MouseLaserDevice(s);
        if (!VRServerDriverHost()->TrackedDeviceAdded("mouselaser-0", TrackedDeviceClass_Controller, m_device))
            Log("TrackedDeviceAdded failed\n");
        return VRInitError_None;
    }

    void Cleanup() override
    {
        delete m_device;
        m_device = nullptr;
        VR_CLEANUP_SERVER_DRIVER_CONTEXT();
    }

    const char *const *GetInterfaceVersions() override { return k_InterfaceVersions; }

    void RunFrame() override
    {
        if (m_device)
            m_device->RunFrame();
        // Diagnostics: log every event (the first 20 of each type, then every 100th), to find
        // out whether a driver sees PrimaryDashboardDeviceChanged (525) or focus events.
        VREvent_t ev;
        while (VRServerDriverHost()->PollNextEvent(&ev, sizeof(ev)))
        {
            unsigned n = ++m_eventCounts[ev.eventType];
            if (n <= 20 || n % 100 == 0)
                Log("event %u device %u (%s) #%u\n", ev.eventType, ev.trackedDeviceIndex,
                    m_device && m_device->Active() ? "active" : "off", n);
        }
    }

    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override {}
    void LeaveStandby() override {}

private:
    MouseLaserDevice *m_device = nullptr;
    std::map<uint32_t, unsigned> m_eventCounts;
};

// One provider for the lifetime of the loaded .so.
static MouseLaserProvider g_provider;

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

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
// EXPERIMENTAL and AI-generated ("vibecoded"); see README.md before relying on it.

#include <openvr_driver.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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

static const char *k_section = "driver_mouselaser";
static const char *k_version = "0.6.1-experimental";

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

static bool TestBit(const unsigned long *bits, int bit)
{
    const int bpl = sizeof(unsigned long) * 8;
    return (bits[bit / bpl] >> (bit % bpl)) & 1;
}

struct Settings
{
    float sensitivityDeg = 0.05f; // degrees of ray rotation per mouse count
    int toggleButton = BTN_EXTRA;
    int backButton = BTN_SIDE;    // sends the compositor's laser "back"; -1 = none
    // Stylus (5) gets its own /user/stylus path. A hand role (1/2) shares the slot with the
    // real controller of that hand, so the device disconnects while off (pre-0.5.0 behaviour).
    int role = TrackedControllerRole_Stylus;
    // Role while laser mode is on (0 = keep `role`). The compositor attaches dragged overlays
    // to /user/hand/{left,right} or /user/head only, so a stylus drag follows the head.
    int activeRole = TrackedControllerRole_LeftHand;
    std::string nameFilter;      // substring of the evdev name; empty = first mouse found
    float originOffsetY = -0.08f; // metres, world space, relative to the HMD
    bool invertY = false;
    // How wheel notches drive the virtual thumbstick:
    //  smooth: each notch bumps the stick, which eases back to centre; spinning holds it.
    //  step:   each notch is one flick, pushed for wheelPressMs then centred for wheelReleaseMs.
    bool wheelSmooth = true;
    float wheelSmoothMin = 0.7f;      // deflection right after a single notch
    float wheelSmoothImpulse = 0.3f;  // extra deflection per further notch
    int wheelSmoothHoldMs = 80;       // no decay this long after a notch
    int wheelSmoothDecayMs = 150;     // exponential decay time constant
    int wheelPressMs = 90;
    int wheelReleaseMs = 60;
    float wheelDeflection = 1.0f;     // maximum deflection (both modes)
    int wheelMaxQueued = 10; // notches buffered while flicks are still playing out
    // Laser mode handshake. The grip click is SteamVR's Laser Mouse <-> Gamepad TOGGLE, so it
    // is pulsed once on activation and once more on deactivation, which puts the mode back.
    int claimDelayMs = 50;        // wait after the role change before pulsing (bindings settle)
    int pulseMs = 60;             // length of each grip / bumper pulse
    float reconnectGraceS = 60.f; // keep laser mode this long while the mouse is gone (sleep)

    void Load()
    {
        EVRSettingsError err;
        IVRSettings *s = VRSettings();
        float f = s->GetFloat(k_section, "sensitivity", &err);
        if (err == VRSettingsError_None) sensitivityDeg = f;
        int32_t i = s->GetInt32(k_section, "toggleButton", &err);
        if (err == VRSettingsError_None) toggleButton = i;
        i = s->GetInt32(k_section, "backButton", &err);
        if (err == VRSettingsError_None) backButton = i;
        i = s->GetInt32(k_section, "role", &err);
        if (err == VRSettingsError_None) role = i;
        i = s->GetInt32(k_section, "activeRole", &err);
        if (err == VRSettingsError_None) activeRole = i;
        char buf[256] = {};
        s->GetString(k_section, "deviceNameFilter", buf, sizeof(buf), &err);
        if (err == VRSettingsError_None) nameFilter = buf;
        f = s->GetFloat(k_section, "originOffsetY", &err);
        if (err == VRSettingsError_None) originOffsetY = f;
        bool b = s->GetBool(k_section, "invertY", &err);
        if (err == VRSettingsError_None) invertY = b;
        i = s->GetInt32(k_section, "wheelPressMs", &err);
        if (err == VRSettingsError_None) wheelPressMs = i;
        i = s->GetInt32(k_section, "wheelReleaseMs", &err);
        if (err == VRSettingsError_None) wheelReleaseMs = i;
        f = s->GetFloat(k_section, "wheelDeflection", &err);
        if (err == VRSettingsError_None) wheelDeflection = f;
        i = s->GetInt32(k_section, "wheelMaxQueued", &err);
        if (err == VRSettingsError_None) wheelMaxQueued = i;
        s->GetString(k_section, "wheelMode", buf, sizeof(buf), &err);
        if (err == VRSettingsError_None) wheelSmooth = strcmp(buf, "step") != 0;
        f = s->GetFloat(k_section, "wheelSmoothMin", &err);
        if (err == VRSettingsError_None) wheelSmoothMin = f;
        f = s->GetFloat(k_section, "wheelSmoothImpulse", &err);
        if (err == VRSettingsError_None) wheelSmoothImpulse = f;
        i = s->GetInt32(k_section, "wheelSmoothHoldMs", &err);
        if (err == VRSettingsError_None) wheelSmoothHoldMs = i;
        i = s->GetInt32(k_section, "wheelSmoothDecayMs", &err);
        if (err == VRSettingsError_None) wheelSmoothDecayMs = i;
        i = s->GetInt32(k_section, "claimDelayMs", &err);
        if (err == VRSettingsError_None) claimDelayMs = i;
        i = s->GetInt32(k_section, "pulseMs", &err);
        if (err == VRSettingsError_None) pulseMs = i;
        f = s->GetFloat(k_section, "reconnectGraceS", &err);
        if (err == VRSettingsError_None) reconnectGraceS = f;
    }
};

// Turns wheel notches on one axis into thumbstick deflection, in "smooth" or "step" mode
// (see Settings). A change of direction drops whatever was still pending the other way.
class StickStepper
{
public:
    float Update(int ticks, const Settings &s)
    {
        using clock = std::chrono::steady_clock;
        clock::time_point now = clock::now();
        return s.wheelSmooth ? UpdateSmooth(ticks, s, now) : UpdateStep(ticks, s, now);
    }

    void Reset() { m_queued = 0, m_pressed = false, m_mag = 0; }

private:
    float UpdateSmooth(int ticks, const Settings &s, std::chrono::steady_clock::time_point now)
    {
        float dt = std::chrono::duration<float>(now - m_last).count();
        m_last = now;
        if (ticks != 0)
        {
            float d = ticks > 0 ? 1.f : -1.f;
            if (d != m_dir) m_dir = d, m_mag = 0;
            float bumped = m_mag > 0 ? m_mag + s.wheelSmoothImpulse * std::abs(ticks)
                                     : s.wheelSmoothMin + s.wheelSmoothImpulse * (std::abs(ticks) - 1);
            m_mag = std::min(s.wheelDeflection, std::max(bumped, s.wheelSmoothMin));
            m_lastNotch = now;
        }
        else if (m_mag > 0 && now - m_lastNotch > std::chrono::milliseconds(s.wheelSmoothHoldMs))
        {
            m_mag *= expf(-dt * 1000.f / float(std::max(1, s.wheelSmoothDecayMs)));
            if (m_mag < 0.1f) m_mag = 0;
        }
        return m_dir * m_mag;
    }

    float UpdateStep(int ticks, const Settings &s, std::chrono::steady_clock::time_point now)
    {
        if (ticks != 0)
        {
            float d = ticks > 0 ? 1.f : -1.f;
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
                m_until = now + std::chrono::milliseconds(s.wheelReleaseMs);
            }
            else if (m_queued > 0)
            {
                m_queued--;
                m_pressed = true;
                m_until = now + std::chrono::milliseconds(s.wheelPressMs);
            }
        }
        return m_pressed ? m_dir * s.wheelDeflection : 0.f;
    }

    // step mode
    int m_queued = 0;
    bool m_pressed = false;
    float m_dir = 0;
    std::chrono::steady_clock::time_point m_until;
    // smooth mode
    float m_mag = 0;
    std::chrono::steady_clock::time_point m_last, m_lastNotch;
};

// A button that also remembers a press, so a click that starts and ends between two
// RunFrames is still reported down for one frame.
struct LatchedButton
{
    std::atomic<bool> down{false}, pressed{false};
    void Set(bool v)
    {
        down = v;
        if (v) pressed = true;
    }
    bool Read() { return pressed.exchange(false) || down; }
    void Reset() { down = pressed = false; }
};

// Reads the physical mouse on its own thread. All state shared with RunFrame is atomic.
class MouseReader
{
public:
    std::atomic<bool> active{false};
    std::atomic<int> dx{0}, dy{0};
    std::atomic<int> wheel{0}, hwheel{0};
    LatchedButton left, right, middle, back;

    // Destroying a joinable std::thread calls std::terminate (taking vrserver down),
    // so make sure the thread is stopped even if Deactivate() was never called.
    ~MouseReader() { Stop(); }

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
        active = false;
    }

private:
    Settings m_settings;
    std::atomic<bool> m_run{false};
    std::thread m_thread;
    int m_fd = -1;
    bool m_lost = false; // mouse vanished while active; laser mode kept for reconnectGraceS
    std::chrono::steady_clock::time_point m_lostAt;

    bool IsMouse(int fd, std::string &name)
    {
        unsigned long evbits[(EV_MAX + 1) / (8 * sizeof(long)) + 1] = {};
        unsigned long relbits[(REL_MAX + 1) / (8 * sizeof(long)) + 1] = {};
        unsigned long keybits[(KEY_MAX + 1) / (8 * sizeof(long)) + 1] = {};
        char n[256] = {};
        if (ioctl(fd, EVIOCGNAME(sizeof(n)), n) < 0) return false;
        name = n;
        // Skip Valve's own emulated mice (e.g. "Steam Ctrl (BT) ... Mouse") unless asked for.
        input_id id = {};
        ioctl(fd, EVIOCGID, &id);
        if (m_settings.nameFilter.empty() && (id.vendor == 0x28de || name.find("Steam") != std::string::npos))
            return false;
        if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0) return false;
        if (!TestBit(evbits, EV_REL) || !TestBit(evbits, EV_KEY)) return false;
        ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbits)), relbits);
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
        if (!TestBit(relbits, REL_X) || !TestBit(relbits, REL_Y) || !TestBit(keybits, BTN_LEFT))
            return false;
        return m_settings.nameFilter.empty() || name.find(m_settings.nameFilter) != std::string::npos;
    }

    bool Open()
    {
        DIR *dir = opendir("/dev/input");
        if (!dir) return false;
        while (dirent *e = readdir(dir))
        {
            if (strncmp(e->d_name, "event", 5) != 0) continue;
            std::string path = std::string("/dev/input/") + e->d_name;
            int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;
            std::string name;
            if (IsMouse(fd, name))
            {
                m_fd = fd;
                Log("using %s (%s)\n", path.c_str(), name.c_str());
                if (active && ioctl(m_fd, EVIOCGRAB, 1) < 0)
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
        left.Reset(), right.Reset(), middle.Reset(), back.Reset();
        dx = dy = wheel = hwheel = 0;
    }

    // The mouse vanished (unplugged, or a Bluetooth mouse napping). Keep laser mode for a
    // while, so a short nap doesn't throw the user out of it.
    void Lost()
    {
        Close();
        if (!active) return;
        m_lost = true;
        m_lostAt = std::chrono::steady_clock::now();
        Log("mouse gone, keeping laser mode for %.0f s\n", m_settings.reconnectGraceS);
    }

    void SetActive(bool on)
    {
        if (active == on) return;
        if (m_fd >= 0 && ioctl(m_fd, EVIOCGRAB, on ? 1 : 0) < 0 && on)
        {
            Log("EVIOCGRAB failed (%s), staying inactive\n", strerror(errno));
            return;
        }
        left.Reset(), right.Reset(), middle.Reset(), back.Reset();
        dx = dy = wheel = hwheel = 0;
        active = on;
        m_lost = false;
        Log("%s\n", on ? "ACTIVE (mouse drives the laser)" : "inactive (mouse back to desktop)");
    }

    void Loop()
    {
        while (m_run)
        {
            if (m_fd < 0 && m_lost &&
                std::chrono::steady_clock::now() - m_lostAt > std::chrono::duration<float>(m_settings.reconnectGraceS))
            {
                Log("mouse still gone, leaving laser mode\n");
                SetActive(false);
            }
            if (m_fd < 0 && !Open())
            {
                for (int i = 0; i < 20 && m_run; i++) usleep(100 * 1000);
                continue;
            }
            pollfd pfd = {m_fd, POLLIN, 0};
            int r = poll(&pfd, 1, 200);
            if (r <= 0) continue;
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            {
                Log("mouse disappeared\n");
                Lost();
                continue;
            }
            input_event ev;
            ssize_t n;
            while ((n = read(m_fd, &ev, sizeof(ev))) == sizeof(ev))
                Handle(ev);
            if (n < 0 && errno == ENODEV)
            {
                Log("mouse disappeared\n");
                Lost();
            }
        }
    }

    void Handle(const input_event &ev)
    {
        if (ev.type == EV_KEY && ev.code == m_settings.toggleButton)
        {
            // Toggle on release: gamescope then sees a whole click (switching on) or nothing
            // (switching off, the press is already grabbed), never half of one.
            if (ev.value == 0) SetActive(!active);
            return;
        }
        if (!active) return;
        if (ev.type == EV_REL)
        {
            if (ev.code == REL_X) dx += ev.value;
            else if (ev.code == REL_Y) dy += ev.value;
            else if (ev.code == REL_WHEEL) wheel += ev.value;
            else if (ev.code == REL_HWHEEL) hwheel += ev.value;
        }
        else if (ev.type == EV_KEY)
        {
            if (ev.value == 2) return; // autorepeat
            if (ev.code == BTN_LEFT) left.Set(ev.value != 0);
            else if (ev.code == BTN_RIGHT) right.Set(ev.value != 0);
            else if (ev.code == BTN_MIDDLE) middle.Set(ev.value != 0);
            else if (ev.code == m_settings.backButton) back.Set(ev.value != 0);
        }
    }
};

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
        if (size) resp[0] = 0;
    }

    DriverPose_t GetPose() override
    {
        std::lock_guard<std::mutex> lock(m_poseMutex);
        return m_pose;
    }

    uint32_t Id() const { return m_id; }
    bool Active() const { return m_phase != Phase::Off; }

    void RunFrame()
    {
        if (m_id == k_unTrackedDeviceIndexInvalid) return;

        TrackedDevicePose_t hmd = {};
        VRServerDriverHost()->GetRawTrackedDevicePoses(0, &hmd, 1);
        const HmdMatrix34_t &m = hmd.mDeviceToAbsoluteTracking;

        using clock = std::chrono::steady_clock;
        const clock::time_point now = clock::now();
        const auto elapsed = now - m_phaseAt;
        const auto pulse = std::chrono::milliseconds(m_settings.pulseMs);
        const bool want = m_mouse.active;

        // ON:  hand role -> wait claimDelayMs -> grip pulse (gamepad -> laser) -> bumper pulse
        //      (SwitchLaserHand only: moves the laser to us) -> On.
        // OFF: grip pulse while still a hand (laser -> gamepad, the stock default) -> let the
        //      release land -> back to the parked role.
        switch (m_phase)
        {
        case Phase::Off:
            if (want)
            {
                SetRole(m_settings.activeRole > 0 ? m_settings.activeRole : m_settings.role);
                if (hmd.bPoseIsValid)
                {
                    // Start the ray where the user is looking.
                    float fx = -m.m[0][2], fy = -m.m[1][2], fz = -m.m[2][2];
                    m_yaw = atan2f(-fx, -fz);
                    m_pitch = asinf(fmaxf(-1.f, fminf(1.f, fy)));
                }
                Enter(Phase::Settle, now);
            }
            break;
        case Phase::Settle:
            if (!want) SetRole(m_settings.role), Enter(Phase::Off, now); // nothing pulsed yet
            else if (elapsed >= std::chrono::milliseconds(m_settings.claimDelayMs)) Enter(Phase::Grip, now);
            break;
        case Phase::Grip:
            if (elapsed >= pulse) Enter(Phase::Claim, now);
            break;
        case Phase::Claim:
            if (elapsed >= pulse) Enter(Phase::On, now);
            break;
        case Phase::On:
            if (!want) Enter(Phase::Restore, now);
            break;
        case Phase::Restore:
            if (elapsed >= pulse) Enter(Phase::Release, now);
            break;
        case Phase::Release:
            if (elapsed >= pulse) SetRole(m_settings.role), Enter(Phase::Off, now);
            break;
        }
        const bool aiming = m_phase == Phase::Settle || m_phase == Phase::Grip ||
                            m_phase == Phase::Claim || m_phase == Phase::On;
        const bool live = m_phase == Phase::On && want;

        // Connected whenever laser mode is (or is being switched) on. While parked, a stylus
        // DISCONNECTS (IsRoleAllowedAsHand(Stylus) is false); a hand role stays connected.
        // This is the behaviour that was tested; staying connected is plan item 1.5.
        bool present = m_phase != Phase::Off || IsRoleAllowedAsHand(ETrackedControllerRole(m_settings.role));

        const float k = m_settings.sensitivityDeg * float(M_PI) / 180.f;
        m_yaw -= m_mouse.dx.exchange(0) * k;
        m_pitch -= m_mouse.dy.exchange(0) * k * (m_settings.invertY ? -1.f : 1.f);
        const float limit = 89.f * float(M_PI) / 180.f;
        m_pitch = fmaxf(-limit, fminf(limit, m_pitch));

        DriverPose_t pose = {};
        pose.qWorldFromDriverRotation.w = 1;
        pose.qDriverFromHeadRotation.w = 1;
        pose.deviceIsConnected = present;
        pose.poseIsValid = present && hmd.bPoseIsValid;
        pose.result = pose.poseIsValid ? TrackingResult_Running_OK : TrackingResult_Running_OutOfRange;
        pose.vecPosition[0] = m.m[0][3];
        pose.vecPosition[1] = m.m[1][3] + m_settings.originOffsetY;
        pose.vecPosition[2] = m.m[2][3];
        // q = yaw(Y) * pitch(X). While off the ray is parked straight up, where it hits nothing.
        float yaw = aiming ? m_yaw : 0.f, pitch = aiming ? m_pitch : float(M_PI) / 2;
        float cy = cosf(yaw / 2), sy = sinf(yaw / 2);
        float cp = cosf(pitch / 2), sp = sinf(pitch / 2);
        pose.qRotation.w = cy * cp;
        pose.qRotation.x = cy * sp;
        pose.qRotation.y = sy * cp;
        pose.qRotation.z = -sy * sp;
        {
            std::lock_guard<std::mutex> lock(m_poseMutex);
            m_pose = pose;
        }
        VRServerDriverHost()->TrackedDevicePoseUpdated(m_id, pose, sizeof(DriverPose_t));

        // Wheel notches become thumbstick flicks: vertical wheel -> Y, tilt wheel -> X.
        if (!live) m_stepY.Reset(), m_stepX.Reset();
        float stickY = m_stepY.Update(m_mouse.wheel.exchange(0), m_settings);
        float stickX = m_stepX.Update(m_mouse.hwheel.exchange(0), m_settings);

        // Read() every frame so stale latched presses are dropped while not live.
        bool l = m_mouse.left.Read() && live;
        VRDriverInput()->UpdateBooleanComponent(m_trigger, l, 0);
        VRDriverInput()->UpdateScalarComponent(m_triggerValue, l ? 1.f : 0.f, 0);
        VRDriverInput()->UpdateBooleanComponent(m_a, m_mouse.right.Read() && live, 0);
        VRDriverInput()->UpdateBooleanComponent(m_b, m_mouse.middle.Read() && live, 0);
        VRDriverInput()->UpdateBooleanComponent(m_back, m_mouse.back.Read() && live, 0);
        VRDriverInput()->UpdateBooleanComponent(m_grip, m_phase == Phase::Grip || m_phase == Phase::Restore, 0);
        VRDriverInput()->UpdateBooleanComponent(m_bumper, m_phase == Phase::Claim, 0);
        VRDriverInput()->UpdateBooleanComponent(m_stickTouch, stickX != 0.f || stickY != 0.f, 0);
        VRDriverInput()->UpdateScalarComponent(m_stickX, stickX, 0);
        VRDriverInput()->UpdateScalarComponent(m_stickY, stickY, 0);
    }

private:
    enum class Phase { Off, Settle, Grip, Claim, On, Restore, Release };

    void Enter(Phase p, std::chrono::steady_clock::time_point now)
    {
        static const char *names[] = {"off", "settle", "grip pulse", "claim pulse", "on", "restore pulse", "release"};
        m_phase = p;
        m_phaseAt = now;
        Log("phase %s\n", names[int(p)]);
    }

    void SetRole(int role)
    {
        if (role == m_currentRole) return;
        m_currentRole = role;
        VRProperties()->SetInt32Property(m_container, Prop_ControllerRoleHint_Int32, role);
        Log("role %d\n", role);
    }

    Settings m_settings;
    MouseReader m_mouse;
    Phase m_phase = Phase::Off;
    std::chrono::steady_clock::time_point m_phaseAt;
    uint32_t m_id = k_unTrackedDeviceIndexInvalid;
    PropertyContainerHandle_t m_container = k_ulInvalidPropertyContainer;
    int m_currentRole = 0;
    std::mutex m_poseMutex;
    DriverPose_t m_pose = {};
    float m_yaw = 0, m_pitch = 0;
    StickStepper m_stepX, m_stepY;
    VRInputComponentHandle_t m_trigger = 0, m_triggerValue = 0, m_a = 0, m_b = 0, m_back = 0, m_grip = 0, m_bumper = 0;
    VRInputComponentHandle_t m_stickTouch = 0, m_stickX = 0, m_stickY = 0;
};

class MouseLaserProvider : public IServerTrackedDeviceProvider
{
public:
    EVRInitError Init(IVRDriverContext *ctx) override
    {
        VR_INIT_SERVER_DRIVER_CONTEXT(ctx);
        Log("version %s\n", k_version);
        Settings s;
        s.Load();
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
        if (m_device) m_device->RunFrame();
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

static MouseLaserProvider g_provider;

extern "C" __attribute__((visibility("default"))) void *HmdDriverFactory(const char *iface, int *ret)
{
    if (strcmp(iface, IServerTrackedDeviceProvider_Version) == 0)
        return &g_provider;
    if (ret) *ret = VRInitError_Init_InterfaceNotFound;
    return nullptr;
}

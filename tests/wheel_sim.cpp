// SPDX-License-Identifier: MIT
// Offline simulation of the wheel -> thumbstick logic (StickStepper), no SteamVR needed.
// Build with -DMOUSELASER_TESTS=ON and run build/wheel_sim.
//
// Each line is a 120 Hz timeline: a digit is |deflection| in tenths, a letter (a-j) is a
// negative deflection, '.' is centred. Use it to sanity-check wheel* setting changes.

#include "../src/driver.cpp"

#include <cstdio>
#include <vector>

static int Run(const char *name, bool smooth, const std::vector<std::pair<int, int>> &notches, int frames)
{
    Settings s;
    s.wheelSmooth = smooth;
    StickStepper st;
    std::string line;
    int above = 0;
    for (int f = 0; f < frames; f++)
    {
        int n = 0;
        for (auto &p : notches)
            if (p.first == f) n += p.second;
        float v = st.Update(n, s);
        float a = fabsf(v);
        if (a >= 0.5f) above++;
        int d = std::min(9, int(a * 10));
        line += a == 0 ? '.' : (v < 0 ? "abcdefghij"[d] : char('0' + d));
        usleep(8333);
    }
    printf("%-6s %-24s frames>=0.5: %3d\n       %s\n", smooth ? "smooth" : "step", name, above, line.c_str());
    return above;
}

int main()
{
    std::vector<std::pair<int, int>> spin, slow;
    for (int f = 2; f < 110; f += 12) spin.push_back({f, 1}); // ~10 notches/s
    for (int f = 2; f < 110; f += 40) slow.push_back({f, 1}); // ~3 notches/s

    int failures = 0;
    for (bool smooth : {true, false})
    {
        int one = Run("1 notch", smooth, {{2, 1}}, 40);
        Run("slow 3 notches/s", smooth, slow, 130);
        int fast = Run("spin 10 notches/s", smooth, spin, 130);
        Run("reverse (+2 then -1)", smooth, {{2, 2}, {8, -1}}, 50);
        // A single notch must register as a step; spinning must hold the stick most of the time.
        if (one < 5) printf("FAIL: single notch too short\n"), failures++;
        if (smooth && fast < 100) printf("FAIL: smooth spin does not hold the stick\n"), failures++;
    }
    return failures ? 1 : 0;
}

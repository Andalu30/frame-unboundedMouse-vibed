// SPDX-License-Identifier: MIT
// Prints how the wheel -> thumbstick logic (StickStepper in src/core.h) responds to a few
// notch patterns, with the default settings, as 120 Hz timelines. A tuning aid for the wheel*
// settings, not a test: the pass/fail checks live in tests/unit_tests.cpp.
// Build with -DMOUSELASER_TESTS=ON and run build/wheel_sim.
//
// Each character is one frame: a digit is the deflection in tenths, a letter (a-j) is a
// negative deflection, '.' is centred.

#include "core.h"

#include <cstdio>
#include <map>
#include <string>

using namespace mouselaser;

static void Run(const char *name, bool smooth, const std::map<int, int> &notches, int frames)
{
    Settings s;
    s.wheelSmooth = smooth;
    StickStepper st;
    TimePoint now{};
    std::string line;
    int above = 0;
    for (int f = 0; f < frames; f++)
    {
        const auto it = notches.find(f);
        const float v = st.Update(it == notches.end() ? 0 : it->second, s, now);
        now += std::chrono::microseconds(8333);
        const float a = std::fabs(v);
        if (a >= 0.5f)
            above++;
        const int d = std::min(9, static_cast<int>(a * 10));
        if (a == 0)
            line += '.';
        else if (v < 0)
            line += "abcdefghij"[d];
        else
            line += static_cast<char>('0' + d);
    }
    printf("%-6s %-24s frames>=0.5: %3d\n       %s\n", smooth ? "smooth" : "step", name, above, line.c_str());
}

int main()
{
    std::map<int, int> spin, slow;
    for (int f = 2; f < 110; f += 12)
        spin[f] = 1; // ~10 notches/s
    for (int f = 2; f < 110; f += 40)
        slow[f] = 1; // ~3 notches/s

    for (bool smooth : {true, false})
    {
        Run("1 notch", smooth, {{2, 1}}, 40);
        Run("slow 3 notches/s", smooth, slow, 130);
        Run("spin 10 notches/s", smooth, spin, 130);
        Run("reverse (+2 then -1)", smooth, {{2, 2}, {8, -1}}, 50);
    }
    return 0;
}

#define NOMINMAX
#include "night_lamps.h"
#include <cstdio>

using namespace WosNightLamps;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s (line %d)\n", #c, __LINE__); ++fails; } } while (0)

int main() {
    // Fingerprint: exact box of s_str_light_ssingle000 matches, a nudged one does not.
    const LampModel& m = kLampModels[0];
    float s[4] = { m.scale[0], m.scale[1], m.scale[2], 0 }, b[4] = { m.bias[0], m.bias[1], m.bias[2], 0 };
    CHECK(Match(s, b) == 0);
    s[1] += 0.01f;
    CHECK(Match(s, b) < 0);

    // Night window.
    CHECK(ClockNight(12 * 3600) == 0.0f);
    CHECK(ClockNight(0) == 1.0f);
    CHECK(ClockNight(18 * 3600 + 1800) > 0.49f && ClockNight(18 * 3600 + 1800) < 0.51f);
    CHECK(ClockNight(6 * 3600) > 0.49f && ClockNight(6 * 3600) < 0.51f);

    // Day: nothing appended.
    float texels[32 * 4] = {};
    texels[3] = 2;
    double cam[3] = { 100, 0, 100 };
    Update(true, 12 * 3600, false);
    Append(texels, cam);
    CHECK(texels[3] == 2);

    // Night: a remembered lamp is picked (selection uses the origin of the previous frame).
    const double lampPos[3] = { 103, 10.1, 100 };
    Remember(lampPos, 0, Now());
    Update(true, 0, false);          // first night frame: no origin yet
    Append(texels, cam);             // records the origin
    Update(true, 0, false);
    CHECK(pickCount == 1);
    texels[3] = 2;
    Append(texels, cam);
    CHECK(texels[3] == 3);
    CHECK(texels[(2 + 4) * 4 + 0] == 3.0f && std::fabs(texels[(2 + 4) * 4 + 1] - 10.1f) < 1e-4f);
    CHECK(std::fabs(texels[(2 + 4) * 4 + 3] - 1.0f / rangeTall) < 1e-6f);
    CHECK(std::fabs(texels[(3 + 4) * 4 + 0] - color[0] * intensity) < 1e-5f);

    // Full set stays full.
    texels[3] = 8;
    Append(texels, cam);
    CHECK(texels[3] == 8);

    // Sky night with a frozen noon clock (WoSTweaks) still lights.
    Update(true, 12 * 3600, true);
    CHECK(pickCount == 1);

    std::printf(fails ? "FAILED %d\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}

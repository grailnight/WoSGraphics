#define NOMINMAX
#include "day_cycle.h"
#include <cstdio>

using namespace WosDayCycle;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s (line %d)\n", #c, __LINE__); ++fails; } } while (0)

static float Elev(const float d[3]) {
    const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    return std::asin(std::fabs(d[1]) / l) * 180.0f / 3.14159265f;
}

int main() {
    const float base[3] = { 0.265f, 0.866f, -0.424f };   // live native direction, 2026-10-05
    float d[3];
    SunArc(base, 12.0f, d);
    CHECK(std::fabs(d[0] - base[0]) < 1e-4f && std::fabs(d[1] - base[1]) < 1e-4f && std::fabs(d[2] - base[2]) < 1e-4f);
    SunArc(base, 0.0f, d);                               // the moon at its highest
    CHECK(std::fabs(d[1] - base[1]) < 1e-4f);
    SunArc(base, 6.0f, d);
    CHECK(std::fabs(Elev(d) - minElevation) < 0.05f);
    float e6[3], e18[3];
    SunArc(base, 6.5f, e6);
    SunArc(base, 17.5f, e18);
    CHECK(std::fabs(Elev(e6) - Elev(e18)) < 0.05f);       // symmetric about noon
    CHECK(e6[0] * e18[0] + e6[2] * e18[2] < 0.0f);        // rises and sets on opposite sides
    SunArc(base, 9.0f, d);
    CHECK(Elev(d) > minElevation && Elev(d) < Elev(base));
    const float down[3] = { -0.265f, -0.866f, 0.424f };  // stored the other way round keeps its sign
    SunArc(down, 9.0f, d);
    CHECK(d[1] < 0.0f);
    std::printf("elevation 06:30 %.1f  09:00 %.1f  12:00 %.1f\n", Elev(e6), [&] { float t[3]; SunArc(base, 9.0f, t); return Elev(t); }(), Elev(base));
    std::printf(fails ? "FAILED %d\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}

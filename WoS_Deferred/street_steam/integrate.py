"""Wire WosStreetSteam into integration/wos_clouds_menu.cpp (idempotent, byte-exact edits)."""
from pathlib import Path
import shutil
import time

HERE = Path(__file__).resolve().parent
CPP = HERE.parent / 'full_deferred_staged' / 'integration' / 'wos_clouds_menu.cpp'
MARK = b'WosStreetSteam'

data = CPP.read_bytes()
if MARK in data:
    raise SystemExit('already integrated')
nl = b'\r\n' if b'\r\n' in data else b'\n'
backup = HERE / ('wos_clouds_menu.before_street_steam_%s.cpp' % time.strftime('%Y%m%d_%H%M%S'))
shutil.copyfile(CPP, backup)


def once(old, new, count=1):
    global data
    n = data.count(old)
    if n != count:
        raise SystemExit('expected %d match(es), found %d: %r' % (count, n, old[:80]))
    data = data.replace(old, new)


L = lambda s: s.replace('\n', nl.decode()).encode('utf-8')

# 1. include after the chimney smoke header (it reuses its observer and helpers)
inc = b'#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/chimney_smoke/chimney_smoke.h"'
once(inc, inc + nl + b'#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/street_steam/street_steam.h"')

# 2. steam at the bloom composite, right after the chimney plumes (before shafts and fog)
bd = b'    WosChimneySmoke::BloomDraw(device, ClassifyShader(g_current_shader)==kShaderBloom);'
once(bd, bd + nl + b'    WosStreetSteam::BloomDraw(device, ClassifyShader(g_current_shader)==kShaderBloom);', count=4)

# 3. device reset
once(b'    WosChimneySmoke::Reset();', b'    WosChimneySmoke::Reset();' + nl + b'    WosStreetSteam::Reset();')

# 4. settings loader, after LoadChimneySmoke's definition
anchor = b'// Cloud march resolution: settings live in [CloudLowRes] of clouds_menu.ini.'
once(anchor, L(
    '// Street steam (SM3 port): settings live in [StreetSteam] of clouds_menu.ini.\n'
    'static void LoadStreetSteam()\n'
    '{\n'
    '    using namespace WosStreetSteam;\n'
    '    auto clamp = [](float v, float lo, float hi) { return std::isfinite(v) ? (std::min)(hi, (std::max)(lo, v)) : lo; };\n'
    '    enabled = ReadFloatIn("StreetSteam", "Enabled", 1.0f) > 0.5f;\n'
    '    density = clamp(ReadFloatIn("StreetSteam", "Density", density), 0.0f, 4.0f);\n'
    '    brightness = clamp(ReadFloatIn("StreetSteam", "Brightness", brightness), 0.0f, 8.0f);\n'
    '    ambientShare = clamp(ReadFloatIn("StreetSteam", "Ambient", ambientShare), 0.0f, 4.0f);\n'
    '    sunShare = clamp(ReadFloatIn("StreetSteam", "Sun", sunShare), 0.0f, 4.0f);\n'
    '    smallScale = clamp(ReadFloatIn("StreetSteam", "SmallScale", smallScale), 1.0f, 200.0f);\n'
    '    largeScale = clamp(ReadFloatIn("StreetSteam", "LargeScale", largeScale), 1.0f, 200.0f);\n'
    '    largeShare = clamp(ReadFloatIn("StreetSteam", "LargeShare", largeShare), 0.0f, 1.0f);\n'
    '    rate = clamp(ReadFloatIn("StreetSteam", "Rate", rate), 0.1f, 20.0f);\n'
    '    lifeMin = clamp(ReadFloatIn("StreetSteam", "LifeMin", lifeMin), 0.5f, 60.0f);\n'
    '    lifeMax = clamp(ReadFloatIn("StreetSteam", "LifeMax", lifeMax), lifeMin, 60.0f);\n'
    '    rise = clamp(ReadFloatIn("StreetSteam", "Rise", rise), -2.0f, 10.0f);\n'
    '    windAngle = clamp(ReadFloatIn("StreetSteam", "WindAngle", windAngle), 0.0f, 360.0f);\n'
    '    windSpeed = clamp(ReadFloatIn("StreetSteam", "WindSpeed", windSpeed), 0.0f, 10.0f);\n'
    '    softRange = clamp(ReadFloatIn("StreetSteam", "SoftRange", softRange), 0.5f, 50.0f);\n'
    '    maxDistance = clamp(ReadFloatIn("StreetSteam", "MaxDistance", maxDistance), 50.0f, 2000.0f);\n'
    '    maxParticles = int(clamp(ReadFloatIn("StreetSteam", "MaxParticles", float(maxParticles)), 0.0f, 6000.0f));\n'
    '}\n') + anchor)

# 5. settings + observer hookup, after the chimney smoke settings
load = b'    LoadChimneySmoke();'
once(load, load + nl + L('    WosStreetSteam::directory=WosProbe::Root()+"\\\\street_steam";\n'
                         '    LoadStreetSteam();\n'
                         '    WosStreetSteam::Install();'))

CPP.write_bytes(data)
print('integrated; backup', backup.name)

"""Wire WosChimneySmoke into integration/wos_clouds_menu.cpp (idempotent, byte-exact edits)."""
from pathlib import Path
import shutil
import time

HERE = Path(__file__).resolve().parent
CPP = HERE.parent / 'full_deferred_staged' / 'integration' / 'wos_clouds_menu.cpp'
MARK = b'WosChimneySmoke'

data = CPP.read_bytes()
if MARK in data:
    raise SystemExit('already integrated')
nl = b'\r\n' if b'\r\n' in data else b'\n'
backup = HERE / ('wos_clouds_menu.before_chimney_smoke_%s.cpp' % time.strftime('%Y%m%d_%H%M%S'))
shutil.copyfile(CPP, backup)


def once(old, new, count=1):
    global data
    n = data.count(old)
    if n != count:
        raise SystemExit('expected %d match(es), found %d: %r' % (count, n, old[:80]))
    data = data.replace(old, new)


L = lambda s: s.replace('\n', nl.decode()).encode('utf-8')

# 1. include after the water SSR header
once(b'#include "B:/Games/Spider Man - Web of Shadows/WoS_Water/ssr/water_ssr.h"',
     b'#include "B:/Games/Spider Man - Web of Shadows/WoS_Water/ssr/water_ssr.h"' + nl +
     b'#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/chimney_smoke/chimney_smoke.h"')

# 2. observe native geometry draws (indexed and not), before any scope swaps state
once(b'    ApplyShaderConstants(device, count);',
     b'    if(!WosProbe::suppress&&!WosDeferred::busy)WosChimneySmoke::Observe(device);' + nl +
     b'    ApplyShaderConstants(device, count);')
old = b'    ApplyShaderConstants(device, primitives);'
first = data.find(old)
if first < 0:
    raise SystemExit('DIP ApplyShaderConstants not found')
# only the first "primitives" site is DrawIndexedPrimitive; the UP hooks follow it
dip = data.rfind(b'HookDrawIndexedPrimitive(', 0, first)
if dip < 0 or data.find(b'HookDrawPrimitiveUP', dip, first) >= 0:
    raise SystemExit('first primitives site is not inside HookDrawIndexedPrimitive')
data = data[:first] + b'    if(!WosProbe::suppress&&!WosDeferred::busy)WosChimneySmoke::Observe(device);' + nl + data[first:]

# 3. plumes at the bloom composite, after water reflections, before shafts and fog
once(b'    WosWaterSSR::BloomDraw(device, ClassifyShader(g_current_shader)==kShaderBloom);',
     b'    WosWaterSSR::BloomDraw(device, ClassifyShader(g_current_shader)==kShaderBloom);' + nl +
     b'    WosChimneySmoke::BloomDraw(device, ClassifyShader(g_current_shader)==kShaderBloom);', count=4)

# 4. device reset
once(b'    WosWaterSSR::Reset();', b'    WosWaterSSR::Reset();' + nl + b'    WosChimneySmoke::Reset();')

# 5. settings
once(b'    WosWater::onReplaced=',
     L('    WosChimneySmoke::directory=WosProbe::Root()+"\\\\chimney_smoke";\n'
       '    LoadChimneySmoke();\n') + b'    WosWater::onReplaced=')

# 6. menu tab
once(L('            {"Сглаживание", 8}, {"GI", 9}, {"Диагностика", 10},'),
     L('            {"Сглаживание", 8}, {"GI", 9}, {"Дым", 11}, {"Диагностика", 10},'))
once(b'            case 7: DrawWaterTab(); break;',
     b'            case 7: DrawWaterTab(); break;' + nl + b'            case 11: DrawSmokeTab(); break;')

TAB = r'''
// Smoke from chimneys: settings live in [ChimneySmoke] of clouds_menu.ini.
static void SaveChimneySmoke()
{
    WriteFloatIn("ChimneySmoke", "Enabled", WosChimneySmoke::enabled ? 1.0f : 0.0f);
    WriteFloatIn("ChimneySmoke", "Density", WosChimneySmoke::density);
    WriteFloatIn("ChimneySmoke", "Brightness", WosChimneySmoke::brightness);
    WriteFloatIn("ChimneySmoke", "PlumeHeight", WosChimneySmoke::plumeHeight);
    WriteFloatIn("ChimneySmoke", "StackHeight", WosChimneySmoke::stackHeight);
    WriteFloatIn("ChimneySmoke", "Spread", WosChimneySmoke::spread);
    WriteFloatIn("ChimneySmoke", "WindAngle", WosChimneySmoke::windAngle);
    WriteFloatIn("ChimneySmoke", "WindStrength", WosChimneySmoke::windStrength);
    WriteFloatIn("ChimneySmoke", "RiseSpeed", WosChimneySmoke::riseSpeed);
    WriteFloatIn("ChimneySmoke", "Turbulence", WosChimneySmoke::noiseStrength);
    WriteFloatIn("ChimneySmoke", "Detail", WosChimneySmoke::noiseFrequency);
    WriteFloatIn("ChimneySmoke", "MaxDistance", WosChimneySmoke::maxDistance);
    WriteFloatIn("ChimneySmoke", "StackDistance", WosChimneySmoke::stackDistance);
    WriteFloatIn("ChimneySmoke", "MaxPlumes", float(WosChimneySmoke::maxPlumes));
    WriteFloatIn("ChimneySmoke", "Steps", float(WosChimneySmoke::steps));
    for (int i = 0; i < 3; ++i) {
        char key[32];
        std::snprintf(key, sizeof(key), "HouseTint%d", i); WriteFloatIn("ChimneySmoke", key, WosChimneySmoke::houseTint[i]);
        std::snprintf(key, sizeof(key), "StackTint%d", i); WriteFloatIn("ChimneySmoke", key, WosChimneySmoke::stackTint[i]);
    }
}
static void LoadChimneySmoke()
{
    using namespace WosChimneySmoke;
    auto clamp = [](float v, float lo, float hi) { return std::isfinite(v) ? (std::min)(hi, (std::max)(lo, v)) : lo; };
    enabled = ReadFloatIn("ChimneySmoke", "Enabled", 1.0f) > 0.5f;
    density = clamp(ReadFloatIn("ChimneySmoke", "Density", density), 0.0f, 4.0f);
    brightness = clamp(ReadFloatIn("ChimneySmoke", "Brightness", brightness), 0.0f, 4.0f);
    plumeHeight = clamp(ReadFloatIn("ChimneySmoke", "PlumeHeight", plumeHeight), 3.0f, 60.0f);
    stackHeight = clamp(ReadFloatIn("ChimneySmoke", "StackHeight", stackHeight), 20.0f, 400.0f);
    spread = clamp(ReadFloatIn("ChimneySmoke", "Spread", spread), 0.02f, 0.5f);
    windAngle = clamp(ReadFloatIn("ChimneySmoke", "WindAngle", windAngle), 0.0f, 360.0f);
    windStrength = clamp(ReadFloatIn("ChimneySmoke", "WindStrength", windStrength), 0.0f, 2.0f);
    riseSpeed = clamp(ReadFloatIn("ChimneySmoke", "RiseSpeed", riseSpeed), 0.0f, 8.0f);
    noiseStrength = clamp(ReadFloatIn("ChimneySmoke", "Turbulence", noiseStrength), 0.0f, 2.0f);
    noiseFrequency = clamp(ReadFloatIn("ChimneySmoke", "Detail", noiseFrequency), 0.2f, 4.0f);
    maxDistance = clamp(ReadFloatIn("ChimneySmoke", "MaxDistance", maxDistance), 50.0f, 2000.0f);
    stackDistance = clamp(ReadFloatIn("ChimneySmoke", "StackDistance", stackDistance), 200.0f, 6000.0f);
    maxPlumes = int(clamp(ReadFloatIn("ChimneySmoke", "MaxPlumes", float(maxPlumes)), 1.0f, 64.0f));
    steps = int(clamp(ReadFloatIn("ChimneySmoke", "Steps", float(steps)), 8.0f, 64.0f));
    for (int i = 0; i < 3; ++i) {
        char key[32];
        std::snprintf(key, sizeof(key), "HouseTint%d", i); houseTint[i] = clamp(ReadFloatIn("ChimneySmoke", key, houseTint[i]), 0.0f, 1.0f);
        std::snprintf(key, sizeof(key), "StackTint%d", i); stackTint[i] = clamp(ReadFloatIn("ChimneySmoke", key, stackTint[i]), 0.0f, 1.0f);
    }
}
static void DrawSmokeTab()
{
    using namespace WosChimneySmoke;
    bool changed = ImGui::Checkbox("Chimney smoke", &enabled);
    if (enabled) {
        ImGui::SeparatorText("Дым");
        changed |= ImGui::SliderFloat("Плотность", &density, 0.0f, 4.0f, "%.2f");
        changed |= ImGui::SliderFloat("Яркость", &brightness, 0.0f, 4.0f, "%.2f");
        changed |= ImGui::SliderFloat("Высота шлейфа (дома)", &plumeHeight, 3.0f, 60.0f, "%.0f м");
        changed |= ImGui::SliderFloat("Высота шлейфа (заводская труба)", &stackHeight, 20.0f, 400.0f, "%.0f м");
        changed |= ImGui::SliderFloat("Расширение", &spread, 0.02f, 0.5f, "%.2f");
        changed |= ImGui::SliderFloat("Клубы", &noiseStrength, 0.0f, 2.0f, "%.2f");
        changed |= ImGui::SliderFloat("Детали клубов", &noiseFrequency, 0.2f, 4.0f, "%.2f");
        changed |= ImGui::ColorEdit3("Цвет дыма (дома)", houseTint);
        changed |= ImGui::ColorEdit3("Цвет дыма (заводская труба)", stackTint);
        ImGui::SeparatorText("Ветер");
        changed |= ImGui::SliderFloat("Направление", &windAngle, 0.0f, 360.0f, "%.0f°");
        changed |= ImGui::SliderFloat("Снос ветром", &windStrength, 0.0f, 2.0f, "%.2f");
        changed |= ImGui::SliderFloat("Скорость подъёма", &riseSpeed, 0.0f, 8.0f, "%.1f м/с");
        ImGui::SeparatorText("Качество");
        changed |= ImGui::SliderFloat("Дальность (дома)", &maxDistance, 50.0f, 2000.0f, "%.0f м");
        changed |= ImGui::SliderFloat("Дальность (заводская труба)", &stackDistance, 200.0f, 6000.0f, "%.0f м");
        changed |= ImGui::SliderInt("Шлейфов в кадре", &maxPlumes, 1, 64);
        changed |= ImGui::SliderInt("Шагов луча", &steps, 8, 64);
        if (ImGui::Button("Забыть найденные трубы")) Forget();
    }
    if (faulted && ImGui::Button("Повторить загрузку шейдера")) Reset();
    ImGui::TextDisabled("%s", status);
    ImGui::TextDisabled("труб известно %u | шлейфов в кадре %lu | совпадений %lu | VS с world %lu/%lu",
                        Known(Now()), plumesLastFrame, matchedDraws, layoutsUsable, layoutsParsed);
    if (changed) SaveChimneySmoke();
}
'''
anchor = b'// The deferred tabs all live behind the one master switch'
once(anchor, L(TAB.lstrip('\n')) + nl + anchor)

CPP.write_bytes(data)
print('integrated; backup', backup)

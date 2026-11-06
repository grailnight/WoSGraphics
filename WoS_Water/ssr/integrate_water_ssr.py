"""Wires shaderpack-style water reflections into the winmm hook. Binary-safe (CRLF, raw NUL bytes).

- include water_ssr.h after taa_runtime.h
- mask replay around the innermost native DP / DIP call (inside the TAA jitter)
- reflection pass at the bloom composite, before god rays and SSGI
- host pointers, settings [WaterSSR], replacement callback, Reset and F3 menu
"""
from pathlib import Path
import shutil, time

SRC = Path(r'B:\Games\Spider Man - Web of Shadows\WoS_Deferred\full_deferred_staged\integration\wos_clouds_menu.cpp')
data = SRC.read_bytes()
assert b'water_ssr.h' not in data, 'already integrated'
backup = Path(__file__).resolve().parent / f'wos_clouds_menu.before_water_ssr_{time.strftime("%Y%m%d_%H%M%S")}.cpp'
shutil.copyfile(SRC, backup)


def crlf(text):
    return text.replace('\r\n', '\n').replace('\n', '\r\n').encode('utf-8')


def replace(old, new, count=1):
    global data
    old_b, new_b = crlf(old), crlf(new)
    assert data.count(old_b) == count, (data.count(old_b), old)
    data = data.replace(old_b, new_b)


# 1. Header.
replace('''#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/taa_native/taa_runtime.h"
''', '''#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/taa_native/taa_runtime.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Water/ssr/water_ssr.h"
''')

# 2. Mask replay. DrawPrimitiveUP clears the stream binding after its call, so
#    only DP and DIP replay; the water mesh is an indexed draw.
for call in ('g_original_draw_primitive(device, type, start, count)',
             'g_original_draw_indexed_primitive(device, type, base, min_index, vertices, start, primitives)'):
    replace('taaJitter.Draw([&](){return %s;})' % call,
            'taaJitter.Draw([&](){return WosWaterSSR::Draw(device,[&](){return %s;});})' % call)

# 3. Reflections before the bloom composite reads the scene.
replace('''    WosVolumetricBridge::Draw(device, ClassifyShader(g_current_shader)==kShaderBloom);
''', '''    WosWaterSSR::BloomDraw(device, ClassifyShader(g_current_shader)==kShaderBloom);
    WosVolumetricBridge::Draw(device, ClassifyShader(g_current_shader)==kShaderBloom);
''', count=4)

# 4. Reset.
replace('''    WosSmaa::Reset();
    WosTaa::Reset();''', '''    WosSmaa::Reset();
    WosTaa::Reset();
    WosWaterSSR::Reset();''')

# 5. Host, settings and the replacement callback, after the TAA setup.
replace('''    if(!WosTaa::LoadShaders(WosProbe::Root()+"\\\\taa")){WosTaa::faulted=true;WosTaa::status="TAA shaders missing in WoS_Deferred\\\\taa";}
''', '''    if(!WosTaa::LoadShaders(WosProbe::Root()+"\\\\taa")){WosTaa::faulted=true;WosTaa::status="TAA shaders missing in WoS_Deferred\\\\taa";}
    WosWaterSSR::host.setPS=g_original_set_pixel_shader;
    WosWaterSSR::directory=WosProbe::Root()+"\\\\water_ssr";
    WosWaterSSR::enabled=ReadFloatIn("WaterSSR","Enabled",1.0f)>0.5f;
    WosWaterSSR::strength=ReadFloatIn("WaterSSR","Strength",WosWaterSSR::strength);
    WosWaterSSR::rippleStrength=ReadFloatIn("WaterSSR","Ripples",WosWaterSSR::rippleStrength);
    WosWaterSSR::f0=ReadFloatIn("WaterSSR","F0",WosWaterSSR::f0);
    WosWaterSSR::roughness=ReadFloatIn("WaterSSR","Roughness",WosWaterSSR::roughness);
    WosWaterSSR::glint=ReadFloatIn("WaterSSR","Glint",WosWaterSSR::glint);
    WosWaterSSR::maxDistance=ReadFloatIn("WaterSSR","MaxDistance",WosWaterSSR::maxDistance);
    WosWater::onReplaced=[](const char*name,IDirect3DPixelShader9*shader){if(!std::strcmp(name,"sm_water_mesh"))WosWaterSSR::RegisterWater(shader);};
''')

# 6. Menu, under TAA.
replace('''            WriteFloatIn("TAA","ClipGamma",WosTaa::clipGamma);
        }
    }
''', '''            WriteFloatIn("TAA","ClipGamma",WosTaa::clipGamma);
        }
    }
    {
        bool waterChanged=ImGui::Checkbox("Water reflections (shaderpack style)",&WosWaterSSR::enabled);
        if(WosWaterSSR::enabled){
            waterChanged|=ImGui::SliderFloat("Water reflection strength",&WosWaterSSR::strength,0.0f,2.0f,"%.2f");
            waterChanged|=ImGui::SliderFloat("Water ripples",&WosWaterSSR::rippleStrength,0.0f,1.0f,"%.2f");
            waterChanged|=ImGui::SliderFloat("Water F0 (base reflectance)",&WosWaterSSR::f0,0.0f,0.2f,"%.3f");
            waterChanged|=ImGui::SliderFloat("Water sun glint",&WosWaterSSR::glint,0.0f,2.0f,"%.2f");
            waterChanged|=ImGui::SliderFloat("Water glint roughness",&WosWaterSSR::roughness,0.03f,0.4f,"%.3f");
            waterChanged|=ImGui::SliderFloat("Water ray length",&WosWaterSSR::maxDistance,50.0f,800.0f,"%.0f");
            ImGui::Combo("Water view",&WosWaterSSR::debugView,"Normal\\0Ripple normal\\0Reflection only\\0Confidence / hit / sky\\0\\0");
        }
        ImGui::TextWrapped("Water: %s | frames %lu | mask draws %lu | skipped %lu | failures %lu",WosWaterSSR::status,WosWaterSSR::frames,WosWaterSSR::lastMaskDraws,WosWaterSSR::skipped,WosWaterSSR::failures+WosWaterSSR::replayFailures);
        if(waterChanged){
            WriteFloatIn("WaterSSR","Enabled",WosWaterSSR::enabled?1.0f:0.0f);
            WriteFloatIn("WaterSSR","Strength",WosWaterSSR::strength);
            WriteFloatIn("WaterSSR","Ripples",WosWaterSSR::rippleStrength);
            WriteFloatIn("WaterSSR","F0",WosWaterSSR::f0);
            WriteFloatIn("WaterSSR","Roughness",WosWaterSSR::roughness);
            WriteFloatIn("WaterSSR","Glint",WosWaterSSR::glint);
            WriteFloatIn("WaterSSR","MaxDistance",WosWaterSSR::maxDistance);
        }
    }
''')

SRC.write_bytes(data)
print('integrated', len(data), 'backup', backup)

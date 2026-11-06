"""Wires native TAA into the winmm hook. Binary-safe (CRLF, raw NUL bytes).

- include taa_runtime.h next to the other deferred headers
- host pointers after the vtable patch, settings from [TAA]
- jitter scope around the innermost native call at all four draw sites
- resolve right after SMAA at the recognised bloom composite
- FrameEnd, Reset and the F3 menu
"""
from pathlib import Path

SRC = Path(r'B:\Games\Spider Man - Web of Shadows\WoS_Deferred\full_deferred_staged\integration\wos_clouds_menu.cpp')
data = SRC.read_bytes()
assert b'taa_runtime.h' not in data, 'already integrated'


def crlf(text):
    return text.replace('\r\n', '\n').replace('\n', '\r\n').encode('utf-8')


def replace(old, new, count=1):
    global data
    old_b, new_b = crlf(old), crlf(new)
    assert data.count(old_b) == count, (data.count(old_b), old)
    data = data.replace(old_b, new_b)


# 1. Header, after the deferred includes.
replace('''#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/deferred_local_shadow_replay.h"
''', '''#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/deferred_local_shadow_replay.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/taa_native/taa_runtime.h"
''')

# 2. Resolve after SMAA. The camera and depth are the ones SSGI reads at the
#    same composite; without them the frame is skipped and jitter stops.
replace('''        candidate=WosSmaa::enabled&&!WosSmaa::done&&!WosSmaa::faulted
            &&!WosProbe::suppress&&!WosDeferred::busy&&!WosDeferredShadow::recording
            &&ClassifyShader(g_current_shader)==kShaderBloom;''', '''        candidate=((WosSmaa::enabled&&!WosSmaa::done&&!WosSmaa::faulted)||(WosTaa::enabled&&!WosTaa::faulted&&!WosTaa::attemptedThisFrame))
            &&!WosProbe::suppress&&!WosDeferred::busy&&!WosDeferredShadow::recording
            &&ClassifyShader(g_current_shader)==kShaderBloom;''')
replace('''        WosSmaa::Render(d);
        g_current_shader=shader;''', '''        if(WosSmaa::enabled&&!WosSmaa::done&&!WosSmaa::faulted)WosSmaa::Render(d);
        if(WosTaa::enabled&&!WosTaa::faulted&&!WosTaa::attemptedThisFrame){
            IDirect3DSurface9*rt=nullptr;D3DSURFACE_DESC desc={};
            if(SUCCEEDED(d->GetRenderTarget(0,&rt))&&rt&&SUCCEEDED(rt->GetDesc(&desc))){
                WosIndirectCamera::Camera camera;IDirect3DTexture9*depth=nullptr;
                if(WosIndirectCamera::Acquire(desc.Width,desc.Height,camera,depth,WosIndirectCamera::CurrentScene()))
                    WosTaa::Resolve(d,depth,camera.relativeInverse,camera.worldOrigin);
                else{WosTaa::attemptedThisFrame=true;++WosTaa::skipped;WosTaa::historyValid=false;WosTaa::status="TAA waiting for the engine camera and scene depth";}
                if(depth)depth->Release();
            }
            if(rt)rt->Release();
        }
        g_current_shader=shader;''')

# 3. Menu, under SMAA.
replace('''    ImGui::Text("SMAA: %s | frames %lu | failures %lu",WosSmaa::status,WosSmaa::frames,WosSmaa::failures);
''', '''    ImGui::Text("SMAA: %s | frames %lu | failures %lu",WosSmaa::status,WosSmaa::frames,WosSmaa::failures);
    {
        bool taaChanged=ImGui::Checkbox("TAA (temporal anti-aliasing)",&WosTaa::enabled);
        if(WosTaa::enabled){
            taaChanged|=ImGui::SliderFloat("TAA history weight",&WosTaa::feedbackMax,0.80f,0.98f,"%.3f");
            taaChanged|=ImGui::SliderFloat("TAA sharpen",&WosTaa::sharpen,0.0f,1.0f,"%.2f");
            taaChanged|=ImGui::SliderFloat("TAA jitter",&WosTaa::jitterScale,0.0f,1.0f,"%.2f");
            taaChanged|=ImGui::SliderFloat("TAA anti-ghosting box",&WosTaa::clipGamma,0.75f,3.0f,"%.2f");
            ImGui::Combo("TAA view",&WosTaa::debugView,"Normal\\0Motion\\0History weight\\0Raw history\\0\\0");
        }
        ImGui::TextWrapped("TAA: %s | frames %lu | resets %lu | skipped %lu | failures %lu",WosTaa::status,WosTaa::frames,WosTaa::historyResets,WosTaa::skipped,WosTaa::failures);
        ImGui::Text("TAA draws jittered %lu, unjittered %lu | shaders %lu patched, %lu rejected",WosTaa::lastJitteredDraws,WosTaa::lastUnjitteredDraws,WosTaa::patchedShaders,WosTaa::rejectedShaders);
        if(taaChanged){
            if(WosTaa::feedbackMin>WosTaa::feedbackMax)WosTaa::feedbackMin=WosTaa::feedbackMax;
            WriteFloatIn("TAA","Enabled",WosTaa::enabled?1.0f:0.0f);
            WriteFloatIn("TAA","HistoryWeight",WosTaa::feedbackMax);
            WriteFloatIn("TAA","Sharpen",WosTaa::sharpen);
            WriteFloatIn("TAA","Jitter",WosTaa::jitterScale);
            WriteFloatIn("TAA","ClipGamma",WosTaa::clipGamma);
        }
    }
''')

# 4. Reset.
replace('''    WosSmaa::Reset();
    WosDeferredControl::Reset();''', '''    WosSmaa::Reset();
    WosTaa::Reset();
    WosDeferredControl::Reset();''')

# 5. Frame end.
replace('''static void RendererFrameEnd(){WosSmaa::FrameEnd();''', '''static void RendererFrameEnd(){WosSmaa::FrameEnd();WosTaa::FrameEnd();''')

# 6. Host pointers and settings once the originals exist.
replace('''    PatchVtable(vtable, 91, reinterpret_cast<void *>(HookCreateVertexShader), reinterpret_cast<void **>(&g_original_create_vertex_shader));
''', '''    PatchVtable(vtable, 91, reinterpret_cast<void *>(HookCreateVertexShader), reinterpret_cast<void **>(&g_original_create_vertex_shader));
    WosTaa::host.setVS=g_original_set_vertex_shader;WosTaa::host.setPS=g_original_set_pixel_shader;
    WosTaa::host.createVS=g_original_create_vertex_shader;WosTaa::host.drawUP=g_original_draw_primitive_up;
    WosTaa::enabled=ReadFloatIn("TAA","Enabled",1.0f)>0.5f;
    WosTaa::feedbackMax=ReadFloatIn("TAA","HistoryWeight",WosTaa::feedbackMax);
    WosTaa::sharpen=ReadFloatIn("TAA","Sharpen",WosTaa::sharpen);
    WosTaa::jitterScale=ReadFloatIn("TAA","Jitter",WosTaa::jitterScale);
    WosTaa::clipGamma=ReadFloatIn("TAA","ClipGamma",WosTaa::clipGamma);
    if(!WosTaa::LoadShaders(WosProbe::Root()+"\\\\taa")){WosTaa::faulted=true;WosTaa::status="TAA shaders missing in WoS_Deferred\\\\taa";}
''')

# 7. Jitter around the innermost native call of each draw site.
sites = [
    ('g_original_draw_primitive(device, type, start, count)', 'DP'),
    ('g_original_draw_indexed_primitive(device, type, base, min_index, vertices, start, primitives)', 'DIP'),
    ('g_original_draw_primitive_up(device, type, primitives, vertices, stride)', 'DPUP'),
]
for call, _ in sites:
    replace('''    const HRESULT result=nativeRsmScope.Execute([&](){return rsmScope.Execute([&](){return materialScope.Execute([&](){return deferredScope.Execute([&](){return %s;});});});});''' % call,
            '''    WosTaa::JitterScope taaJitter(device,!WosProbe::suppress&&!WosDeferred::busy&&WosIndirectCamera::frameReady);
    const HRESULT result=nativeRsmScope.Execute([&](){return rsmScope.Execute([&](){return materialScope.Execute([&](){return deferredScope.Execute([&](){return taaJitter.Draw([&](){return %s;});});});});});''' % call)

# DrawIndexedPrimitiveUP spans two lines in the source.
marker = crlf('''    const HRESULT result=nativeRsmScope.Execute([&](){return rsmScope.Execute([&](){return materialScope.Execute([&](){return deferredScope.Execute([&](){return g_original_draw_indexed_primitive_up(device, type, min_index, vertices, primitives,''')
assert data.count(marker) == 1
start = data.index(marker)
end = data.index(b';\r\n', start) + 3
statement = data[start:end]
call_start = statement.index(b'g_original_draw_indexed_primitive_up(')
depth, i = 0, call_start
while True:
    c = statement[i:i + 1]
    if c == b'(':
        depth += 1
    elif c == b')':
        depth -= 1
        if depth == 0:
            break
    i += 1
call = statement[call_start:i + 1]
new_statement = (crlf('    WosTaa::JitterScope taaJitter(device,!WosProbe::suppress&&!WosDeferred::busy&&WosIndirectCamera::frameReady);\n') +
                 statement.replace(call, b'taaJitter.Draw([&](){return ' + call + b';})', 1).replace(b'});});});});', b'});});});});', 1))
# The inner lambda gained one level: "return CALL;}" becomes "return taaJitter.Draw([&](){return CALL;});}".
assert new_statement.count(b'taaJitter.Draw') == 1
data = data[:start] + new_statement + data[end:]

SRC.write_bytes(data)
print('integrated', len(data))

"""Snapshot the latest shared renderer, then patch ONLY the isolated snapshot."""
from pathlib import Path
import json,re,hashlib,datetime,shutil
ROOT=Path(__file__).resolve().parent
GAME=ROOT.parent.parent
STAGE=GAME/'WoS_Deferred/full_deferred_staged'
OUT=ROOT/'candidate_source'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def once(s,old,new):
    assert s.count(old)==1,(old[:90],s.count(old));return s.replace(old,new,1)
def main():
    OUT.mkdir(exist_ok=True)
    entry=STAGE/'integration/wos_clouds_menu.cpp'
    imgui=GAME/'WoS_VolumetricClouds/tools/imgui'
    inputs={};destinations={}
    def snapshot(path):
        path=path.resolve()
        if path in destinations:return destinations[path]
        assert path.is_relative_to(GAME),path
        dest=OUT/path.relative_to(GAME);destinations[path]=dest
        raw=path.read_bytes();inputs[str(path.relative_to(GAME))]=hashlib.sha256(raw).hexdigest()
        dest.parent.mkdir(parents=True,exist_ok=True)
        def rewrite(match):
            inc=match[1].decode('utf-8')
            options=[path.parent/inc,imgui/inc,imgui/'backends'/inc]
            found=next((p.resolve() for p in options if p.is_file()),None)
            if found is None:return match[0]
            copied=snapshot(found)
            return b'#include "'+copied.as_posix().encode()+b'"'
        if path.suffix in ('.cpp','.h','.hpp'):
            raw=re.sub(rb'^\s*#\s*include\s*"([^"]+)"',rewrite,raw,flags=re.M)
        dest.write_bytes(raw);return dest
    sources=[snapshot(entry)]
    for name in ['imgui.cpp','imgui_draw.cpp','imgui_tables.cpp','imgui_widgets.cpp','backends/imgui_impl_win32.cpp','backends/imgui_impl_dx9.cpp']:
        sources.append(snapshot(imgui/name))
    module=snapshot(ROOT/'sky_ibl_runtime.h')
    runtime=destinations[(STAGE/'integration/deferred_runtime.h').resolve()]
    body=runtime.read_bytes()
    body=once(body,b'#include <cstdlib>',b'#include <cstdlib>\n#include "'+module.as_posix().encode()+b'"')
    body=once(body,b'static void Reset(){',b'static void Reset(){\n    WosSkyIBL::Reset();')
    body=once(body,b'static void FrameEnd(){',b'static void FrameEnd(){\n    WosSkyIBL::FrameEnd();')
    anchor=b'if(ok){WosDeferredGpu::Scope gpu(d,WosDeferredGpu::SectionResolve);ok=WosIndirect::Quad(d,width,height);}'
    body=once(body,anchor,anchor+b'\n        if(ok)ok=WosSkyIBL::Render(d,camera.scene,debugView,[&](){return WosIndirect::Quad(d,width,height);},TrackedSetPixelShader);')
    body=once(body,b'    intensity=ReadSetting(',b'    WosSkyIBL::enabled=ReadSetting("SkyIBL","Enabled",1,0,1)>.5f;\n    WosSkyIBL::diffuseStrength=ReadSetting("SkyIBL","Diffuse",.65f,0,2);\n    WosSkyIBL::specularStrength=ReadSetting("SkyIBL","Specular",1,0,3);\n    intensity=ReadSetting(')
    anchor=b'    put("Pbr","RoughnessFloor",pbrRoughnessFloor);'
    body=once(body,anchor,b'    put("SkyIBL","Enabled",WosSkyIBL::enabled?1.f:0.f);put("SkyIBL","Diffuse",WosSkyIBL::diffuseStrength);put("SkyIBL","Specular",WosSkyIBL::specularStrength);\n'+anchor)
    anchor=b'    std::fprintf(f,"\\\"cascades\\\":'
    body=once(body,anchor,b'    std::fprintf(f,"\\\"sky_ibl\\\":{\\\"enabled\\\":%s,\\\"valid\\\":%s,\\\"profile\\\":%d,\\\"captures\\\":%u,\\\"binds\\\":%u,\\\"failures\\\":%u},",WosSkyIBL::enabled?"true":"false",WosSkyIBL::valid?"true":"false",WosSkyIBL::profile,WosSkyIBL::captures,WosSkyIBL::binds,WosSkyIBL::failures);\n'+anchor)
    runtime.write_bytes(body)
    cpp=destinations[entry.resolve()];body=cpp.read_bytes()
    anchor=b'    WosVolumetricBridge::Draw(device, ClassifyShader(g_current_shader)==kShaderBloom);'
    assert body.count(anchor)==4
    body=body.replace(anchor,anchor+b'\n    if(!WosProbe::suppress&&!WosDeferred::busy&&ClassifyShader(g_current_shader)==kShaderCloud)WosSkyIBL::Capture(device,WosIndirectCamera::CurrentScene());')
    anchor=b'        if(ImGui::TreeNode("PBR surface estimate")) {'
    menu=b'''        if(ImGui::TreeNode("Sky IBL")) {
            deferredChanged|=ImGui::Checkbox("Sky lighting", &WosSkyIBL::enabled);
            deferredChanged|=ImGui::SliderFloat("Sky diffuse", &WosSkyIBL::diffuseStrength, 0.f, 2.f);
            deferredChanged|=ImGui::SliderFloat("Sky reflections", &WosSkyIBL::specularStrength, 0.f, 3.f);
            ImGui::Text("Sky captures: %u, draws: %u",WosSkyIBL::captures,WosSkyIBL::binds);
            ImGui::TreePop();
        }
'''
    body=once(body,anchor,menu+anchor);cpp.write_bytes(body)
    # All source inputs must still match after this multi-file snapshot.
    for p,h in inputs.items():assert sha(GAME/p)==h,('Source changed during snapshot',p)
    definition=ROOT/'winmm.generated.def';shutil.copy2(STAGE/'build/winmm.generated.def',definition)
    command='@echo off\ncall "C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\VC\\Auxiliary\\Build\\vcvars32.bat"\nif errorlevel 1 exit /b %errorlevel%\ncd /d "%~dp0"\n'
    command+='cl /nologo /LD /MT /EHsc /std:c++17 /utf-8 /O2 '+f'/I"{OUT/imgui.relative_to(GAME)}" /I"{OUT/imgui.relative_to(GAME)/"backends"}" '+' '.join('"'+str(p)+'"' for p in sources)
    command+=' /link /DEF:winmm.generated.def /OUT:winmm.candidate.dll d3d9.lib user32.lib winmm.lib dxguid.lib\n'
    (ROOT/'build_candidate.cmd').write_text(command)
    receipt={'time':datetime.datetime.now().astimezone().isoformat(),'source_inputs':inputs,'baseline_dll_sha256':sha(GAME/'winmm.dll'),'candidate_inputs':{str(p.relative_to(ROOT)):sha(p) for p in destinations.values()},'installed':False}
    (ROOT/'candidate.json').write_text(json.dumps(receipt,indent=2));print(f'Isolated {len(inputs)} inputs; live source and DLL unchanged.')
if __name__=='__main__':main()

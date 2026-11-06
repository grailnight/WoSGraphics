"""Apply bounded additions only to the byte-proven renderer baseline."""
from pathlib import Path
import json,hashlib,difflib
G=Path(__file__).resolve().parents[2]
M=Path(__file__).resolve().parent
I=G/'WoS_Deferred/full_deferred_staged/integration'
baseline=json.loads((G/'WoS_Deferred/effects_current/verified_baseline.json').read_text())['source_files']
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
for p,h in baseline.items():
    if p.endswith('/prepare_integration.py'):continue
    assert sha(G/p)==h,p
edits={}
def edit(name,fn):
    p=I/name;old=p.read_bytes();new=fn(old);edits[p]=(old,new)
def once(b,a,c):
    assert b.count(a)==1,(a[:100],b.count(a))
    return b.replace(a,c,1)
params=[('RoughnessFloor','.4f','0.05f','1'),('RoughnessCeiling','.95f','.05f','1'),('RoughnessVariance','.12f','0','1'),('DielectricF0','.04f','.001f','.16f'),('MetalStrength','0','0','1'),('MetalThreshold','.35f','0','1'),('MetalWidth','.25f','.01f','1'),('MetalTint','.5f','0','1')]
def runtime(b):
    b=once(b,b'#include <cstdlib>',b'#include <cstdlib>\n#include "../../sky_ibl/sky_ibl_runtime.h"')
    b=once(b,b'static void Reset(){',b'static void Reset(){\n    WosSkyIBL::Reset();')
    b=once(b,b'static void FrameEnd(){',b'static void FrameEnd(){\n    WosSkyIBL::FrameEnd();')
    b=once(b,b'    intensity=ReadSetting(',('    WosSkyIBL::enabled=ReadSetting("SkyIBL","Enabled",1,0,1)>.5f;\n    WosSkyIBL::diffuseStrength=ReadSetting("SkyIBL","Diffuse",.65f,0,2);\n    WosSkyIBL::specularStrength=ReadSetting("SkyIBL","Specular",1,0,3);\n'+''.join(f'    WosSkyIBL::material[{i}]=ReadSetting("Pbr","{n}",{d},{lo},{hi});\n' for i,(n,d,lo,hi) in enumerate(params))).encode()+b'    intensity=ReadSetting(')
    anchor=b'    put("Deferred","Enabled",enabled?1.f:0.f);'
    save='    put("SkyIBL","Enabled",WosSkyIBL::enabled?1.f:0.f);put("SkyIBL","Diffuse",WosSkyIBL::diffuseStrength);put("SkyIBL","Specular",WosSkyIBL::specularStrength);\n'+''.join(f'    put("Pbr","{n}",WosSkyIBL::material[{i}]);\n' for i,(n,*_) in enumerate(params))
    b=once(b,anchor,save.encode()+anchor)
    anchor=b'        check(d->SetPixelShaderConstantF(42,rainReflection,1));'
    b=once(b,anchor,anchor+b'\n        check(d->SetPixelShaderConstantF(43,WosSkyIBL::material,2));')
    anchor=b'if(ok){WosDeferredGpu::Scope gpu(d,WosDeferredGpu::SectionResolve);ok=WosIndirect::Quad(d,width,height);}'
    b=once(b,anchor,anchor+b'\n        if(ok){WosDeferredGpu::Scope gpu(d,WosDeferredGpu::SectionResolve);ok=WosSkyIBL::Render(d,camera.scene,debugView,[&](){return WosIndirect::Quad(d,width,height);},TrackedSetPixelShader);}')
    anchor=b'    std::fprintf(f,"\\\"cascades\\\":'
    line=r'    std::fprintf(f,"\"sky_ibl\":{\"enabled\":%s,\"valid\":%s,\"profile\":%d,\"captures\":%u,\"binds\":%u,\"failures\":%u,\"sky_scene\":%u,\"resolve_scene\":%u},",WosSkyIBL::enabled?"true":"false",WosSkyIBL::valid?"true":"false",WosSkyIBL::profile,WosSkyIBL::captures,WosSkyIBL::binds,WosSkyIBL::failures,WosSkyIBL::skyScene,WosSkyIBL::resolveScene);'
    return once(b,anchor,line.encode()+b'\n'+anchor)
def menu(b):
    anchor=b'    WosVolumetricBridge::Draw(device, ClassifyShader(g_current_shader)==kShaderBloom);'
    assert b.count(anchor)==4
    b=b.replace(anchor,anchor+b'\n    if(!WosProbe::suppress&&!WosDeferred::busy&&ClassifyShader(g_current_shader)==kShaderCloud)WosSkyIBL::Capture(device,WosIndirectCamera::CurrentScene());')
    anchor=b'        deferredChanged|=ImGui::Checkbox("Four dynamic cascades (PCSS)"'
    ui='''        if(ImGui::TreeNode("PBR sky lighting")) {
            deferredChanged|=ImGui::Checkbox("Sky lighting", &WosSkyIBL::enabled);
            deferredChanged|=ImGui::SliderFloat("Sky diffuse", &WosSkyIBL::diffuseStrength,0.f,2.f);
            deferredChanged|=ImGui::SliderFloat("Sky reflections", &WosSkyIBL::specularStrength,0.f,3.f);
            ImGui::Text("Sky captures: %u, draws: %u",WosSkyIBL::captures,WosSkyIBL::binds);
'''
    for i,(n,d,lo,hi) in enumerate(params):ui+=f'            deferredChanged|=ImGui::SliderFloat("{n}",&WosSkyIBL::material[{i}],float({lo}),float({hi}));\n'
    ui+='            ImGui::TreePop();\n        }\n'
    return once(b,anchor,ui.encode()+anchor)
def control(b):
    b=once(b,b'struct Request {',b'struct Request {\n    bool hasSky=false,sky=false;')
    b=once(b,b'"ShadowGlobals","ShadowWork"',b'"ShadowGlobals","ShadowWork","SkyIBL"')
    b=once(b,b'index<20',b'index<21');b=once(b,b'index==20',b'index==21')
    anchor=b'        case 19:'
    b=once(b,anchor,b'        case 20:good=value=="0"||value=="1";out.hasSky=true;out.sky=value=="1";break;\n'+anchor)
    anchor=b'    if(request.hasShadowWork)'
    b=once(b,anchor,b'    if(request.hasSky)WosSkyIBL::enabled=request.sky;\n'+anchor)
    return once(b,b'if(request.hasShadowWork||',b'if(request.hasSky||request.hasShadowWork||')
edit('deferred_runtime.h',runtime);edit('wos_clouds_menu.cpp',menu);edit('deferred_control.h',control)
for p,(old,new) in edits.items():assert p.read_bytes()==old
for p,(old,new) in edits.items():p.write_bytes(new)
(M/'verified_integration.patch').write_text('\n'.join(''.join(difflib.unified_diff(old.decode('utf8').splitlines(True),new.decode('utf8').splitlines(True),fromfile=str(p),tofile=str(p))) for p,(old,new) in edits.items()),encoding='utf8')
(M/'verified_changes.json').write_text(json.dumps({str(p.relative_to(G)):sha(p) for p in edits},indent=2))
print('Applied three bounded source changes to verified baseline.')

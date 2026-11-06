from pathlib import Path
import re
ROOT=Path(__file__).resolve().parent
def apply(text):
    if 'struct NativeSmaaScope' in text:return text
    text='#include "'+(ROOT/'smaa_runtime.h').as_posix()+'"\n'+text
    bridge='''
// Constructed before the other draw scopes, destroyed after they restore state.
// The recognized native postprocess has already drawn; HUD draws come later.
struct NativeSmaaScope {
    IDirect3DDevice9*d; bool candidate=false,success=false;
    NativeSmaaScope(IDirect3DDevice9*dev):d(dev){
        candidate=WosSmaa::enabled&&!WosSmaa::done&&!WosSmaa::faulted
            &&!WosProbe::suppress&&!WosDeferred::busy&&!WosDeferredShadow::recording
            &&ClassifyShader(g_current_shader)==kShaderBloom;
    }
    ~NativeSmaaScope(){
        if(!candidate||!success)return;
        const bool suppress=WosProbe::suppress,busy=WosDeferred::busy;
        auto*shader=g_current_shader;
        WosProbe::suppress=true;WosDeferred::busy=true;
        WosSmaa::Render(d);
        g_current_shader=shader;
        WosDeferred::busy=busy;WosProbe::suppress=suppress;
        WosDeferredShadow::Invalidate();
    }
};
'''
    text=text.replace('static void DrawMenu()',bridge+'\nstatic void DrawMenu()',1)
    for name in ('HookDrawPrimitive','HookDrawIndexedPrimitive','HookDrawPrimitiveUP','HookDrawIndexedPrimitiveUP'):
        start=text.index('static HRESULT STDMETHODCALLTYPE '+name+'(')
        end=text.index('\n}',start)+2
        chunk=text[start:end]
        chunk=chunk.replace('\n{','\n{\n    NativeSmaaScope smaaScope(device);',1)
        chunk=chunk.replace('    return nativeRsmScope.Execute(', '    const HRESULT result=nativeRsmScope.Execute(',1)
        chunk=chunk[:-2]+'\n    smaaScope.success=SUCCEEDED(result);\n    return result;\n}'
        text=text[:start]+chunk+text[end:]
    text=text.replace('    WosDeferredControl::Reset();','    WosSmaa::Reset();\n    WosDeferredControl::Reset();',1)
    text=text.replace('static void RendererFrameEnd(){','static void RendererFrameEnd(){WosSmaa::FrameEnd();',1)
    text=text.replace('    WosDeferred::LoadSettings();','    WosSmaa::enabled=ReadFloatIn("SMAA","Enabled",1.0f)>0.5f;\n    WosDeferred::LoadSettings();',1)
    anchor='    bool deferredChanged=ImGui::Checkbox'
    ui='''    if(ImGui::Checkbox("SMAA 1x High (native)",&WosSmaa::enabled)){
        WriteFloatIn("SMAA","Enabled",WosSmaa::enabled?1.0f:0.0f);
    }
    ImGui::Text("SMAA: %s | frames %lu | failures %lu",WosSmaa::status,WosSmaa::frames,WosSmaa::failures);
'''
    assert anchor in text
    return text.replace(anchor,ui+anchor,1)
if __name__=='__main__':
    p=ROOT.parent/'full_deferred_staged/integration/wos_clouds_menu.cpp'
    p.write_text(apply(p.read_text(encoding='utf-8')),encoding='utf-8')

#pragma once
namespace WosLocalShadow {
template<class Draw>static void Replay(IDirect3DDevice9*d,const DrawArgs&args,Draw draw){
    if(!valid||!enabled||!WosCascadeProbe::ready||WosCascadeProbe::busy||WosProbe::suppress)return;
    // The cascade replay of this same draw has usually read all of this
    // already; its gates are the same as the ones below.
    const auto&info=WosCascadeProbe::casterInfo;
    bool fromNear=false;int kind=-1;UINT reg=0;Matrix original;size_t matrixInstance=0;
    WosShadowWork::CasterBox box;bool boxKnown=false;
    if(info.valid){
        fromNear=info.fromNear;kind=info.kind;reg=info.reg;original=info.original;matrixInstance=info.instance;box=info.box;boxKnown=true;
    }else{
        auto g=WosShadowFastGlobals::Read(WosEngineProbe::imageBase,WosCascadeProbe::verified&&WosCascadeProbe::ready&&WosEngineProbe::imageBase==0x400000,[](){return WosEngineProbe::ReadGlobals();});
        fromNear=WosCascadeProbe::nearReady&&g.current==WosCascadeProbe::nearScene;
        if(g.valid!=7||g.shadowActive!=1||(!fromNear&&g.current!=WosCascadeProbe::farScene))return;
        DWORD zwrite=0,blend=0;
        if(FAILED(d->GetRenderState(D3DRS_ZWRITEENABLE,&zwrite))||!zwrite||FAILED(d->GetRenderState(D3DRS_ALPHABLENDENABLE,&blend))||blend)return;
    }
    WosCascadeProbe::CpuTimer replayTimer(WosShadowWork::cpuCurrent.localReplayMs);
    if(!info.valid){
        IDirect3DVertexShader9*vs=nullptr;if(FAILED(d->GetVertexShader(&vs)))return;
        kind=WosCascadeProbe::ShaderKind(vs);if(vs)vs->Release();
        if(kind<0){++skipped;return;}
        reg=WosCascadeProbe::kindRegister[kind];
        if(FAILED(d->GetVertexShaderConstantF(reg,original.v,4))){++failures;valid=false;return;}
    }
    Matrix world={};
    if(kind==1){
        auto&index=fromNear?WosCascadeProbe::nearInstanceIndex:WosCascadeProbe::instanceIndex;
        auto&source=fromNear?WosCascadeProbe::nearSource:WosCascadeProbe::source;
        if(!info.valid){const size_t*found=index.Find(&original);if(!found){++skipped;return;}matrixInstance=*found;}
        if(matrixInstance>=source.instances.size()){++skipped;return;}
        world=source.instances[matrixInstance].world;
    }
    ++WosShadowWork::cpuCurrent.localCasters;
    // Route per slot: 0 live atlas, 1 static atlas (first time seen), 2 already
    // held by the static layer. Only rigid casters with a complete key can be
    // routed anywhere but live.
    enum {kLive=0,kStatic=1,kHeld=2};
    unsigned route[2]={kLive,kLive};DrawKey key;bool keyed=false;
    if(kind==1&&StaticUsable()){
        keyed=MakeKey(d,args,world,key);
        if(keyed)for(unsigned s=0;s<count;++s)if(slots[s].active)route[s]=Held(slots[s],key)?kHeld:(previousWorlds->Find(&world)?kStatic:kLive);
    }
    bool wantLive=false,wantStatic=false;
    for(unsigned s=0;s<count;++s){wantLive|=route[s]==kLive;wantStatic|=route[s]==kStatic;}
    for(unsigned s=0;s<count;++s)if(route[s]==kHeld)frameSkippedDraws+=6;
    if(!wantLive&&!wantStatic)return;
    // Only the rigid shader has a known position box; a face it misses would
    // clip the whole draw away, so it is not submitted there.
    if(kind==1&&!boxKnown)WosShadowWork::BuildCasterBox(d,box);
    // A light whose cube cannot reach the box: every point of a face frustum
    // lies within sqrt(3) * far of the light, so a box farther than that is
    // outside all six faces and their tests would all come back culled.
    bool lightReached[2]={true,true};
    if(kind==1&&box.valid&&box.corner[0][3]==1.f){
        float lo[3]={1e30f,1e30f,1e30f},hi[3]={-1e30f,-1e30f,-1e30f};
        for(unsigned k=0;k<8;++k)for(unsigned j=0;j<3;++j){
            const float*c=box.corner[k];
            const float v=c[0]*world.v[j]+c[1]*world.v[4+j]+c[2]*world.v[8+j]+c[3]*world.v[12+j];
            lo[j]=(std::min)(lo[j],v);hi[j]=(std::max)(hi[j],v);
        }
        bool finite=true;for(unsigned j=0;j<3;++j)finite=finite&&std::isfinite(lo[j])&&std::isfinite(hi[j]);
        for(unsigned s=0;s<count&&finite;++s){
            double gap2=0;
            for(unsigned j=0;j<3;++j){const double p=lights[s].p[j];const double g=p<lo[j]?lo[j]-p:(p>hi[j]?p-hi[j]:0.0);gap2+=g*g;}
            const double reach=(std::min)(double(lights[s].range),500.0)*1.7320508075688772+1.0;
            if(gap2>reach*reach)lightReached[s]=false;
        }
    }
    bool anyReached=false;for(unsigned s=0;s<count;++s)anyReached|=lightReached[s]&&route[s]!=kHeld;
    if(!anyReached&&!wantStatic){WosShadowWork::cpuCurrent.localCulled+=count*6;return;}
    const Matrix*cached=WosShadowWork::enabled?WosShadowWork::local.Get(unsigned(kind),fromNear,matrixInstance,world):nullptr;
    // Face matrices and the faces the caster's box reaches, decided before any
    // device state changes: a caster no face can see costs no target switch.
    Matrix faceMatrix[12];unsigned visible=0;bool built=true;
    for(unsigned face=0;face<count*6;++face){
        if(route[face/6]==kHeld)continue;
        if(!lightReached[face/6]){++WosShadowWork::cpuCurrent.localCulled;continue;}
        Matrix&m=faceMatrix[face];const Matrix&vp=matrices[face];
        if(cached)m=cached[face];
        else if(kind==0){for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)m.v[r*4+c]=vp.v[c*4+r];}
        else if(kind>=2){for(unsigned c=0;c<4;++c){m.v[c]=vp.v[c];m.v[4+c]=vp.v[8+c];m.v[8+c]=vp.v[4+c];m.v[12+c]=vp.v[12+c];}}
        else if(!WosShadowTransform::Build(world,vp,m)){built=false;break;}
        if(WosShadowWork::OutsideClip(box,m.v)){++WosShadowWork::cpuCurrent.localCulled;continue;}
        visible|=1u<<face;
    }
    if(built&&!visible){
        // Nothing reaches the atlas. A static-routed caster still counts as
        // recorded: its six faces are complete, they are just all empty.
        if(wantStatic)for(unsigned s=0;s<count;++s)if(route[s]==kStatic)Remember(slots[s],key,world);
        return;
    }
    const D3DRENDERSTATETYPE states[]={D3DRS_SCISSORTESTENABLE,D3DRS_STENCILENABLE,D3DRS_COLORWRITEENABLE,
        D3DRS_CLIPPLANEENABLE,D3DRS_DEPTHBIAS,D3DRS_SLOPESCALEDEPTHBIAS,D3DRS_ZFUNC};
    DWORD saved[7]={};for(unsigned i=0;i<7;++i)if(FAILED(d->GetRenderState(states[i],&saved[i]))){++failures;valid=false;return;}
    WosCascadeShadow::HostScope host;bool previous=WosCascadeProbe::busy;WosCascadeProbe::busy=true;
    bool ok=built;
    for(unsigned i=0;i<7;++i)ok=SUCCEEDED(d->SetRenderState(states[i],i==6?D3DCMP_LESSEQUAL:0))&&ok;
    for(unsigned target=kLive;target<=kStatic&&ok;++target){
        if(target==kLive?!wantLive:!wantStatic)continue;
        unsigned mine=0;for(unsigned face=0;face<count*6;++face)if(route[face/6]==target&&(visible&(1u<<face)))mine|=1u<<face;
        if(!mine)continue;
        Binding binding;ok=binding.Begin(d,target==kStatic?staticDepth:depth);
        for(unsigned face=0;face<count*6&&ok;++face){
            if(!(mine&(1u<<face)))continue;
            ok=binding.Select(face)&&SUCCEEDED(d->SetVertexShaderConstantF(reg,faceMatrix[face].v,4))&&SUCCEEDED(draw());++draws;++WosShadowWork::cpuCurrent.localDraws;
            if(target==kStatic)++frameStaticDraws;else ++frameLiveDraws;
        }
        ok=binding.End()&&ok;
    }
    // Only a caster whose six faces all reached the static atlas is recorded.
    if(ok&&wantStatic)for(unsigned s=0;s<count;++s)if(route[s]==kStatic)Remember(slots[s],key,world);
    ok=SUCCEEDED(d->SetVertexShaderConstantF(reg,original.v,4))&&ok;
    for(unsigned i=0;i<7;++i)ok=SUCCEEDED(d->SetRenderState(states[i],saved[i]))&&ok;
    WosCascadeProbe::busy=previous;
    if(!ok){
        valid=false;++failures;status="Local shadow replay failed; unshadowed fallback";
        for(auto&slot:slots)slot.dirty=true;
    }
}
}

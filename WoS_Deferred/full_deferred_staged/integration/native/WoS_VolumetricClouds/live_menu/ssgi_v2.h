// SSGI v2 (included inside namespace WosIndirect, after State and Quad).
// Proper Shaders' SSILVB scheme on this renderer's inputs: prepare -> mip chain
// -> 16-sector visibility-mask trace -> temporal (exact camera reprojection) ->
// a-trous x2 -> depth/normal-aware upsample + composite. Shaders and the spec
// live in WoS_Deferred/ssgi_v2. [SSGI] Version=1 keeps the previous pass.
namespace V2 {
struct Settings {
    int version=2,divisor=2,slices=3,steps=6;
    float radius=5.f,thickness=.5f,intensity=3.f,saturation=1.1f,maxIndirect=4.f,angleBias=25.f,aoPower=10.f,aoWhite=.95f;
    float emissiveThreshold=1.5f,emissiveStrength=1.f,firefly=30.f,multiBounce=.35f,backface=0.f;
    float historyWeight=.98f,depthThreshold=.03f,clamp=1.f,clampAO=.7f,wideSpacing=3.f,depthSigma=10.f,normalSigma=8.f,lumaSigma=.5f;
    float fadeout=400.f,fallbackAlbedo=.6f;int debugView=0;float debugGain=8.f;
};
static Settings s;
static bool loaded=false;
static IDirect3DPixelShader9*prepare=nullptr,*trace=nullptr,*temporal=nullptr,*filter=nullptr,*compose=nullptr;
static IDirect3DTexture9*geom=nullptr,*radiance=nullptr,*traceTex=nullptr,*history[2]={},*meta[2]={},*filterTmp=nullptr,*cache=nullptr;
static IDirect3DSurface9*traceRT=nullptr,*historyRT[2]={},*metaRT[2]={},*filterRT=nullptr,*cacheRT=nullptr;
static std::vector<IDirect3DSurface9*> geomLevels,radianceLevels;
static unsigned tw=0,th=0,levels=1,current=0,frame=0,builtDivisor=0;
static bool valid=false,mipsOk=true;
static float prevRows[9]={},prevUVToView[4]={};static double prevWorld[3]={};
static const char*Path(){static std::string p;p=WosProbe::Root()+"\\full_deferred\\settings.ini";return p.c_str();}
static float Read(const char*k,float f,float lo,float hi){char v[64]={},d[64]={};std::snprintf(d,sizeof(d),"%.9g",f);GetPrivateProfileStringA("SSGI2",k,d,v,sizeof(v),Path());float x=float(std::atof(v));return std::isfinite(x)?(std::min)((std::max)(x,lo),hi):f;}
static void Write(const char*k,float v){char t[64]={};std::snprintf(t,sizeof(t),"%.9g",v);WritePrivateProfileStringA("SSGI2",k,t,Path());}
static void Load(){
    Settings d;s.version=int(Read("Version",2,1,2));s.divisor=Read("Resolution",2,1,4)>=3?4:(Read("Resolution",2,1,4)>=1.5f?2:1);
    s.slices=int(Read("Slices",3,1,8));s.steps=int(Read("Steps",6,4,32));s.radius=Read("Radius",d.radius,.05f,50);s.thickness=Read("Thickness",d.thickness,0,5);
    s.intensity=Read("Intensity",d.intensity,0,20);s.saturation=Read("Saturation",d.saturation,0,2);s.maxIndirect=Read("MaxIndirect",d.maxIndirect,.01f,100);
    s.angleBias=Read("AngleBias",d.angleBias,0,60);s.multiBounce=Read("MultiBounce",d.multiBounce,0,1);s.backface=Read("Backface",d.backface,0,1);
    s.historyWeight=Read("HistoryWeight",d.historyWeight,0,.98f);s.clamp=Read("Clamp",d.clamp,0,4);s.fadeout=Read("Fadeout",d.fadeout,1,5000);
    s.emissiveThreshold=Read("EmissiveThreshold",d.emissiveThreshold,0,100);s.emissiveStrength=Read("EmissiveStrength",d.emissiveStrength,0,10);
    loaded=true;
}
static void Save(){
    Write("Version",float(s.version));Write("Resolution",float(s.divisor));Write("Slices",float(s.slices));Write("Steps",float(s.steps));Write("Radius",s.radius);
    Write("Thickness",s.thickness);Write("Intensity",s.intensity);Write("Saturation",s.saturation);Write("MaxIndirect",s.maxIndirect);Write("AngleBias",s.angleBias);
    Write("MultiBounce",s.multiBounce);Write("Backface",s.backface);Write("HistoryWeight",s.historyWeight);Write("Clamp",s.clamp);Write("Fadeout",s.fadeout);
    Write("EmissiveThreshold",s.emissiveThreshold);Write("EmissiveStrength",s.emissiveStrength);
}
template<class T> static void Drop(T*&p){if(p)p->Release();p=nullptr;}
static void Release(){
    Drop(prepare);Drop(trace);Drop(temporal);Drop(filter);Drop(compose);
    for(auto*l:geomLevels)if(l)l->Release();for(auto*l:radianceLevels)if(l)l->Release();geomLevels.clear();radianceLevels.clear();
    Drop(traceRT);Drop(filterRT);Drop(cacheRT);for(unsigned i=0;i<2;++i){Drop(historyRT[i]);Drop(metaRT[i]);Drop(history[i]);Drop(meta[i]);}
    Drop(geom);Drop(radiance);Drop(traceTex);Drop(filterTmp);Drop(cache);tw=th=0;valid=false;
}
static bool Create(IDirect3DDevice9*d,unsigned w,unsigned h){
    const unsigned nw=(std::max)(1u,w/unsigned(s.divisor)),nh=(std::max)(1u,h/unsigned(s.divisor));
    if(prepare&&tw==nw&&th==nh&&builtDivisor==unsigned(s.divisor))return true;
    Release();tw=nw;th=nh;builtDivisor=unsigned(s.divisor);
    auto shader=[&](const char*name,IDirect3DPixelShader9*&out){auto bytes=WosCascadeProbe::ReadFile(WosProbe::Root()+"\\ssgi_v2\\"+name);return !bytes.empty()&&bytes.size()%4==0&&SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(bytes.data()),&out));};
    if(!shader("ssgi_v2_prepare.cso",prepare)||!shader("ssgi_v2_trace.cso",trace)||!shader("ssgi_v2_temporal.cso",temporal)||!shader("ssgi_v2_filter.cso",filter)||!shader("ssgi_v2_composite.cso",compose)){Release();return false;}
    // Mip count as the original: one level per two steps, at most four, none below 8 texels.
    levels=(std::min)(((unsigned(s.steps)-1)>>1)+1,4u);while(levels>1&&((tw>>(levels-1))<=7||(th>>(levels-1))<=7))--levels;
    auto target=[&](IDirect3DTexture9*&t,IDirect3DSurface9*&sf){return SUCCEEDED(d->CreateTexture(tw,th,1,D3DUSAGE_RENDERTARGET,D3DFMT_A16B16G16R16F,D3DPOOL_DEFAULT,&t,nullptr))&&SUCCEEDED(t->GetSurfaceLevel(0,&sf));};
    auto chain=[&](IDirect3DTexture9*&t,std::vector<IDirect3DSurface9*>&v){
        if(FAILED(d->CreateTexture(tw,th,levels,D3DUSAGE_RENDERTARGET,D3DFMT_A16B16G16R16F,D3DPOOL_DEFAULT,&t,nullptr)))return false;
        v.assign(t->GetLevelCount(),nullptr);for(unsigned i=0;i<v.size();++i)if(FAILED(t->GetSurfaceLevel(i,&v[i])))return false;return !v.empty();};
    bool ok=chain(geom,geomLevels)&&chain(radiance,radianceLevels)&&target(traceTex,traceRT)&&target(filterTmp,filterRT)&&target(cache,cacheRT);
    for(unsigned i=0;i<2&&ok;++i)ok=target(history[i],historyRT[i])&&target(meta[i],metaRT[i]);
    if(!ok){Release();return false;}
    levels=unsigned(geomLevels.size());mipsOk=true;valid=false;return true;
}
// Camera basis from the camera-relative inverse VP: forward through the screen
// centre, right and up scaled to tan(half fov). The engine projection is symmetric.
struct Basis{double r[3],u[3],f[3],tanX,tanY;bool ok;};
static Basis CameraBasis(const float inv[16]){
    Basis b={};auto un=[&](double x,double y,double out[3]){double c[4]={x,y,.5,1},w[4];for(unsigned i=0;i<4;++i)w[i]=inv[i*4]*c[0]+inv[i*4+1]*c[1]+inv[i*4+2]*c[2]+inv[i*4+3]*c[3];if(!(std::fabs(w[3])>1e-12))return false;for(unsigned i=0;i<3;++i)out[i]=w[i]/w[3];return true;};
    double c[3],px[3],py[3];if(!un(0,0,c)||!un(1,0,px)||!un(0,1,py))return b;
    double len=std::sqrt(c[0]*c[0]+c[1]*c[1]+c[2]*c[2]);if(!(len>1e-9))return b;for(unsigned i=0;i<3;++i)b.f[i]=c[i]/len;
    auto side=[&](const double p[3],double out[3],double&tan){double z=p[0]*b.f[0]+p[1]*b.f[1]+p[2]*b.f[2];if(!(z>1e-9))return false;double v[3];for(unsigned i=0;i<3;++i)v[i]=p[i]/z-b.f[i];tan=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);if(!(tan>1e-6))return false;for(unsigned i=0;i<3;++i)out[i]=v[i]/tan;return true;};
    b.ok=side(px,b.r,b.tanX)&&side(py,b.u,b.tanY);return b;
}
static bool Render(IDirect3DDevice9*d,IDirect3DBaseTexture9*source,IDirect3DBaseTexture9*depth,const WosIndirectCamera::Camera&camera,IDirect3DTexture9*normal,IDirect3DTexture9*rho,bool continuous){
    if(!Create(d,width,height))return false;
    const Basis b=CameraBasis(camera.relativeInverse);if(!b.ok)return false;
    const bool materials=normal&&rho;
    double delta[3];for(unsigned i=0;i<3;++i)delta[i]=camera.worldOrigin[i]-prevWorld[i];
    bool useHistory=valid&&continuous&&delta[0]*delta[0]+delta[1]*delta[1]+delta[2]*delta[2]<32.*32.;
    float c[24*4]={};
    const float uvToView[4]={float(2*b.tanX),float(-2*b.tanY),float(-b.tanX),float(b.tanY)};
    memcpy(c,uvToView,16);
    c[4]=float(tw);c[5]=float(th);c[6]=1.f/tw;c[7]=1.f/th;
    c[8]=1.f/width;c[9]=1.f/height;c[10]=float(th*.5/b.tanY);c[11]=(std::max)(th*.5f,4.f);
    c[12]=std::fmod(frame*.618034f,1.f);c[13]=frame*.7548777f+.5f;c[14]=float(s.slices);c[15]=float(s.steps);
    c[16]=1.f/s.steps;c[17]=s.radius;c[18]=1.f/s.radius;c[19]=mipsOk?float(levels-1):0.f;
    c[20]=s.thickness;c[21]=s.thickness*.02f;c[22]=s.fadeout*2.8f;c[23]=s.fadeout;
    c[24]=std::sin((std::min)((std::max)(s.angleBias,0.f),60.f)*3.14159265f/180.f);c[25]=s.aoPower;c[26]=1.f/s.aoWhite;c[27]=s.maxIndirect;
    c[28]=s.emissiveThreshold;c[29]=s.emissiveStrength;c[30]=s.firefly;c[31]=s.multiBounce;
    c[32]=s.backface;c[33]=s.intensity;c[34]=s.saturation;c[35]=useHistory?1.f:0.f;
    c[36]=s.historyWeight;c[37]=s.depthThreshold;c[38]=s.clamp;c[39]=s.clampAO;
    c[40]=s.wideSpacing;c[41]=s.depthSigma;c[42]=s.normalSigma*1.442695f;c[43]=s.lumaSigma;
    c[44]=96.f/s.divisor;c[45]=float(s.debugView);c[46]=s.debugGain;c[47]=materials?1.f:0.f;
    const double*axes[3]={b.r,b.u,b.f};
    for(unsigned i=0;i<3;++i)for(unsigned k=0;k<3;++k)c[48+i*4+k]=float(axes[i][k]);
    memcpy(c+60,camera.relativeInverse,64);
    // Current view -> previous view: back to camera-relative world (columns R,U,F),
    // shift by the camera move, then into last frame's axes.
    float rows[9];for(unsigned i=0;i<3;++i)for(unsigned k=0;k<3;++k)rows[i*3+k]=float(axes[i][k]);
    if(useHistory)for(unsigned i=0;i<3;++i){
        for(unsigned j=0;j<3;++j){double v=0;for(unsigned k=0;k<3;++k)v+=prevRows[i*3+k]*axes[j][k];c[76+i*4+j]=float(v);}
        double t=0;for(unsigned k=0;k<3;++k)t+=prevRows[i*3+k]*delta[k];c[76+i*4+3]=float(t);
    }
    memcpy(c+88,prevUVToView,16);
    c[92]=s.fallbackAlbedo;c[93]=maxDistance;c[94]=float(s.divisor);

    State state(d);if(!state.valid||state.slots<2)return false;busy=true;bool ok=true;
    auto check=[&](HRESULT h){ok=SUCCEEDED(h)&&ok;};
    check(d->SetDepthStencilSurface(nullptr));for(unsigned i=1;i<state.slots;++i)check(d->SetRenderTarget(i,nullptr));
    check(d->SetVertexShader(nullptr));check(d->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1));
    const D3DRENDERSTATETYPE zeros[]={D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ALPHABLENDENABLE,D3DRS_ALPHATESTENABLE,D3DRS_STENCILENABLE,D3DRS_SCISSORTESTENABLE,D3DRS_SRGBWRITEENABLE,D3DRS_FOGENABLE,D3DRS_CLIPPLANEENABLE,D3DRS_SEPARATEALPHABLENDENABLE};for(auto z:zeros)check(d->SetRenderState(z,0));
    check(d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE));check(d->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID));check(d->SetRenderState(D3DRS_COLORWRITEENABLE,15));check(d->SetRenderState(D3DRS_COLORWRITEENABLE1,15));check(d->SetRenderState(D3DRS_MULTISAMPLEMASK,0xffffffff));
    auto sampler=[&](unsigned i,D3DTEXTUREFILTERTYPE f,D3DTEXTUREFILTERTYPE mip){check(d->SetSamplerState(i,D3DSAMP_MINFILTER,f));check(d->SetSamplerState(i,D3DSAMP_MAGFILTER,f));check(d->SetSamplerState(i,D3DSAMP_MIPFILTER,mip));check(d->SetSamplerState(i,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP));check(d->SetSamplerState(i,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP));check(d->SetSamplerState(i,D3DSAMP_SRGBTEXTURE,0));check(d->SetSamplerState(i,D3DSAMP_MAXMIPLEVEL,0));};
    for(unsigned i=0;i<6;++i){check(d->SetTexture(i,nullptr));sampler(i,D3DTEXF_POINT,D3DTEXF_NONE);}
    check(d->SetPixelShaderConstantF(0,c,24));
    const unsigned prev=current,next=current^1u;
    // 1. Prepare (MRT) at trace resolution, then the mip chain.
    check(d->SetRenderTarget(0,geomLevels[0]));check(d->SetRenderTarget(1,radianceLevels[0]));
    check(d->SetTexture(0,source));check(d->SetTexture(1,depth));check(d->SetTexture(2,materials?normal:nullptr));check(d->SetTexture(3,materials?rho:nullptr));check(d->SetTexture(4,useHistory?cache:nullptr));sampler(4,D3DTEXF_LINEAR,D3DTEXF_NONE);
    check(g_original_set_pixel_shader(d,prepare));if(ok)ok=Quad(d,tw,th);
    check(d->SetRenderTarget(1,nullptr));for(unsigned i=0;i<6;++i)check(d->SetTexture(i,nullptr));
    if(ok&&mipsOk)for(unsigned i=1;i<levels;++i)
        if(FAILED(d->StretchRect(geomLevels[i-1],nullptr,geomLevels[i],nullptr,D3DTEXF_LINEAR))||FAILED(d->StretchRect(radianceLevels[i-1],nullptr,radianceLevels[i],nullptr,D3DTEXF_LINEAR))){mipsOk=false;c[19]=0;check(d->SetPixelShaderConstantF(4,c+16,1));break;}
    // 2. Trace.
    check(d->SetRenderTarget(0,traceRT));check(d->SetTexture(0,geom));check(d->SetTexture(1,radiance));
    sampler(0,D3DTEXF_POINT,D3DTEXF_POINT);sampler(1,D3DTEXF_LINEAR,D3DTEXF_POINT);
    check(g_original_set_pixel_shader(d,trace));if(ok)ok=Quad(d,tw,th);
    sampler(0,D3DTEXF_POINT,D3DTEXF_NONE);sampler(1,D3DTEXF_POINT,D3DTEXF_NONE);
    // 3. Temporal (MRT).
    check(d->SetRenderTarget(0,historyRT[next]));check(d->SetRenderTarget(1,metaRT[next]));
    check(d->SetTexture(0,geom));check(d->SetTexture(1,traceTex));check(d->SetTexture(2,history[prev]));check(d->SetTexture(3,meta[prev]));sampler(2,D3DTEXF_LINEAR,D3DTEXF_NONE);
    check(g_original_set_pixel_shader(d,temporal));if(ok)ok=Quad(d,tw,th);
    check(d->SetRenderTarget(1,nullptr));sampler(2,D3DTEXF_POINT,D3DTEXF_NONE);
    // 4-5. A-trous, wide then narrow; history keeps the unfiltered result.
    check(d->SetRenderTarget(0,filterRT));check(d->SetTexture(0,geom));check(d->SetTexture(1,history[next]));check(d->SetTexture(2,nullptr));check(d->SetTexture(3,nullptr));
    check(g_original_set_pixel_shader(d,filter));if(ok)ok=Quad(d,tw,th);
    const float narrow[4]={1.f,s.depthSigma,s.normalSigma*1.442695f,s.lumaSigma};
    check(d->SetPixelShaderConstantF(10,narrow,1));check(d->SetRenderTarget(0,cacheRT));check(d->SetTexture(1,filterTmp));
    if(ok)ok=Quad(d,tw,th);
    // 6. Composite at full resolution.
    check(d->SetRenderTarget(0,compositeRT));
    check(d->SetTexture(0,source));check(d->SetTexture(1,depth));check(d->SetTexture(2,materials?normal:nullptr));check(d->SetTexture(3,materials?rho:nullptr));check(d->SetTexture(4,cache));check(d->SetTexture(5,geom));
    check(g_original_set_pixel_shader(d,compose));if(ok)ok=Quad(d,width,height);
    for(unsigned i=0;i<6;++i)check(d->SetTexture(i,nullptr));
    ok=state.Restore()&&ok;busy=false;
    if(ok){current=next;valid=true;memcpy(prevRows,rows,sizeof(rows));memcpy(prevUVToView,uvToView,sizeof(prevUVToView));for(unsigned i=0;i<3;++i)prevWorld[i]=camera.worldOrigin[i];frame=(frame+1)%1024;if(useHistory)++historyFrames;else ++historyResets;}
    else valid=false;
    return ok;
}
}

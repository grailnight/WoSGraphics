#define main unused_fixture_main
#include "fixture.cpp"
#undef main
#include "sky_ibl_runtime.h"
static IDirect3DTexture9* solid(float r,float g,float b,float a){
    IDirect3DTexture9*t=nullptr;CHECK(SUCCEEDED(device->CreateTexture(1,1,1,0,D3DFMT_A32B32G32R32F,D3DPOOL_MANAGED,&t,nullptr)));
    D3DLOCKED_RECT lock={};CHECK(SUCCEEDED(t->LockRect(0,&lock,nullptr,0)));
    float values[4]={r,g,b,a};std::memcpy(lock.pBits,values,16);t->UnlockRect(0);return t;
}
static void constant(unsigned reg,std::array<float,4>v){CHECK(SUCCEEDED(device->SetPixelShaderConstantF(reg,v.data(),1)));}
static void sky(float h,float m,float z,int profile){
    constant(45,{h,h,h,0});constant(46,{m,m,m,0});constant(47,{z,z,z,0});constant(48,{1,1,1,float(profile)});
}
static void bind(unsigned slot,IDirect3DTexture9*t){
    CHECK(SUCCEEDED(device->SetTexture(slot,t)));
    CHECK(SUCCEEDED(device->SetSamplerState(slot,D3DSAMP_MINFILTER,D3DTEXF_POINT)));
    CHECK(SUCCEEDED(device->SetSamplerState(slot,D3DSAMP_MAGFILTER,D3DTEXF_POINT)));
}
static std::vector<float> read(){
    IDirect3DSurface9*target=nullptr,*cpu=nullptr;CHECK(SUCCEEDED(device->GetRenderTarget(0,&target)));
    D3DSURFACE_DESC d={};target->GetDesc(&d);
    CHECK(SUCCEEDED(device->CreateOffscreenPlainSurface(d.Width,d.Height,d.Format,D3DPOOL_SYSTEMMEM,&cpu,nullptr)));
    CHECK(SUCCEEDED(device->GetRenderTargetData(target,cpu)));
    D3DLOCKED_RECT l={};CHECK(SUCCEEDED(cpu->LockRect(&l,nullptr,D3DLOCK_READONLY)));
    std::vector<float> out(d.Width*d.Height*4);
    for(unsigned y=0;y<d.Height;++y)std::memcpy(out.data()+y*d.Width*4,static_cast<char*>(l.pBits)+y*l.Pitch,d.Width*16);
    cpu->UnlockRect();cpu->Release();target->Release();return out;
}
static bool quad(){
    struct P{float x,y,z,w,u,v;};P q[]={{-.5f,-.5f,0,1,0,0},{3.5f,-.5f,0,1,1,0},{-.5f,.5f,0,1,0,1},{3.5f,.5f,0,1,1,1}};
    CHECK(SUCCEEDED(device->BeginScene()));
    HRESULT result=device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,q,sizeof(P));
    CHECK(SUCCEEDED(device->EndScene()));return SUCCEEDED(result);
}
static void ppm(const char*path,const std::vector<float>&pixels,unsigned w,unsigned h){
    std::ofstream f(path,std::ios::binary);f<<"P6\n"<<w<<" "<<h<<"\n255\n";
    for(unsigned i=0;i<w*h;++i)for(int c=0;c<3;++c){unsigned char x=(unsigned char)(std::clamp(pixels[i*4+c],0.f,1.f)*255);f.write((char*)&x,1);}
}
int main(){
    HWND window=CreateWindowExW(0,L"STATIC",L"Sky IBL validation",WS_OVERLAPPEDWINDOW,0,0,800,600,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);CHECK(window);
    IDirect3D9*api=Direct3DCreate9(D3D_SDK_VERSION);CHECK(api);
    D3DADAPTER_IDENTIFIER9 adapter={};api->GetAdapterIdentifier(0,0,&adapter);printf("HAL: %s\n",adapter.Description);
    D3DPRESENT_PARAMETERS pp={};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=window;pp.BackBufferWidth=800;pp.BackBufferHeight=600;pp.BackBufferFormat=D3DFMT_A8R8G8B8;
    CHECK(SUCCEEDED(api->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&device)));
    device->SetRenderState(D3DRS_ZENABLE,FALSE);device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);device->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1);
    float zeros[224*4]={};device->SetPixelShaderConstantF(0,zeros,224);
    // Test tagged texture identification, including invalid/missing tags.
    IDirect3DTexture9*texture=nullptr;
    CHECK(SUCCEEDED(device->CreateTexture(4096,512,1,0,D3DFMT_DXT5,D3DPOOL_MANAGED,&texture,nullptr)));
    for(int id=0;id<10;++id){
        D3DLOCKED_RECT r={};CHECK(SUCCEEDED(texture->LockRect(0,&r,nullptr,0)));std::memset(r.pBits,0,r.Pitch*128);
        unsigned short words[4]={0xf81f,0x07e0,(unsigned short)(((id&1)?0xf800:0)|((id&2)?0x07e0:0)|((id&4)?31:0)),(unsigned short)((id&8)?0xffe0:0x07e0)};
        for(unsigned i=0;i<4;++i){auto p=static_cast<char*>(r.pBits)+16*i+8;std::memcpy(p,&words[i],2);std::memcpy(p+2,&words[i],2);}
        texture->UnlockRect(0);CHECK(WosSkyIBL::Profile(texture)==id);
    }
    bind(0,texture);constant(5,{.3f,.4f,.5f,0});constant(6,{.4f,.5f,.6f,0});constant(7,{.1f,.2f,.4f,0});
    WosSkyIBL::Capture(device,123);CHECK(WosSkyIBL::valid&&WosSkyIBL::profile==9);
    CHECK(WosSkyIBL::Bind(device,123));float controls[4]={};device->GetPixelShaderConstantF(48,controls,1);CHECK(controls[0]==1);
    CHECK(WosSkyIBL::Bind(device,456));device->GetPixelShaderConstantF(48,controls,1);CHECK(controls[0]==0);
    WosSkyIBL::FrameEnd();CHECK(WosSkyIBL::Bind(device,123));device->GetPixelShaderConstantF(48,controls,1);CHECK(controls[0]==1);
    WosSkyIBL::FrameEnd();CHECK(WosSkyIBL::Bind(device,123));device->GetPixelShaderConstantF(48,controls,1);CHECK(controls[0]==0);
    WosSkyIBL::Capture(device,123);CHECK(WosSkyIBL::Bind(device,123));
    auto probe=load("probe.cso");
    constant(0,{0,1,0,0});constant(1,{0,1,0,0});constant(2,{.5f,.3f,.1f,1});constant(3,{0,0,0,32});
    constant(43,{.02f,1,0,.04f});constant(44,{0,0,1,1});
    double error=0;int scenarios=0;
    for(int profile=0;profile<2;++profile){
        sky(1,1,1,profile);
        auto upward=draw(probe,1,1);
        for(int c=0;c<3;++c){error=std::max(error,std::abs(upward[c]-(.5-.2*c)));CHECK(std::abs(upward[c]-(.5-.2*c))<.002);}
        constant(0,{1,0,0,0});constant(1,{1,0,0,0});auto sideways=draw(probe,1,1);
        for(int c=0;c<3;++c)CHECK(std::abs(sideways[c]-(.25-.1*c))<.003);
        constant(0,{0,-1,0,0});constant(1,{0,-1,0,0});auto downward=draw(probe,1,1);for(int c=0;c<3;++c)CHECK(downward[c]<.002);
        constant(0,{0,1,0,0});constant(1,{0,1,0,0});scenarios+=3;
    }
    for(float power:{1.f,8.f,64.f,512.f,2048.f})for(float metal:{0.f,1.f})for(float view:{1.f,.5f,.05f})for(float radiance:{0.f,.02f,1.f}){
        constant(1,{sqrtf(1-view*view),view,0,0});constant(3,{1,1,1,power});constant(44,{metal,0,1,1});sky(radiance,radiance,radiance,1);
        auto out=draw(probe,1,1);for(int c=0;c<3;++c){CHECK(std::isfinite(out[c])&&out[c]>=0&&out[c]<1.01f);if(radiance==0)CHECK(out[c]==0);}CHECK(out[3]==.75f);++scenarios;
    }
    // Full sky resolve / production Render: additive RGB preserves existing alpha.
    auto base=load("baseline.cso");
    auto n=solid(.5f,1,1,1),rho=solid(.5f,.3f,.1f,1),sp=solid(0,0,0,32),depth=solid(.5f,0,0,0),ao=solid(1,1,1,1),native=solid(.125f,.25f,.375f,.75f);
    bind(0,native);bind(1,n);bind(2,rho);bind(3,sp);bind(4,depth);bind(6,native);bind(8,ao);
    IDirect3DTexture9*lights=nullptr;CHECK(SUCCEEDED(device->CreateTexture(32,2,1,0,D3DFMT_A32B32G32R32F,D3DPOOL_MANAGED,&lights,nullptr)));
    D3DLOCKED_RECT lock={};lights->LockRect(0,&lock,nullptr,0);std::memset(lock.pBits,0,lock.Pitch*2);lights->UnlockRect(0);bind(5,lights);
    device->SetPixelShaderConstantF(0,zeros,43);constant(1,{0,0,0,-1});constant(3,{0,0,0,1});constant(4,{.25f,1,2,0});
    constant(43,{.4f,.95f,.12f,.04f});constant(44,{0,0,1,1});
    auto before=draw(base,4,1);
    for(int i=0;i<12;++i)WosSkyIBL::colors[i]=1;
    WosSkyIBL::diffuseStrength=1;WosSkyIBL::specularStrength=1;
    CHECK(WosSkyIBL::Render(device,123,0,quad));auto after=read();
    for(int i=0;i<4;++i){for(int c=0;c<3;++c)CHECK(std::abs(after[i*4+c]-before[i*4+c]-(.5-.2*c))<.002);CHECK(after[i*4+3]==before[i*4+3]);}
    // OFF, diagnostic view and stale scene are exact no-ops.
    WosSkyIBL::enabled=false;CHECK(WosSkyIBL::Render(device,123,0,quad));CHECK(read()==after);
    WosSkyIBL::enabled=true;CHECK(WosSkyIBL::Render(device,123,2,quad));CHECK(read()==after);
    CHECK(WosSkyIBL::Render(device,456,0,quad));CHECK(read()==after);
    // AO and native fog suppress sky light; covered and guard pixels remain intact.
    for(int mode=0;mode<3;++mode){
        auto rf=solid(.5f,.3f,.1f,mode==0?0:1),normal=solid(.5f,1,1,mode==2?-1:1),zeroAO=solid(0,0,0,0);
        bind(1,normal);bind(2,rf);bind(8,zeroAO);constant(28,{mode==1?1.f:0.f,0,0,0});
        CHECK(WosSkyIBL::Render(device,123,0,quad));CHECK(read()==after);rf->Release();normal->Release();zeroAO->Release();++scenarios;
    }
    CHECK(WosSkyIBL::Bind(device,123));constant(43,{.12f,.95f,.02f,.04f});constant(44,{0,0,1,1});
    constant(45,{.6f,.8f,1,0});constant(46,{.2f,.45f,.8f,0});constant(47,{.03f,.12f,.45f,0});
    auto preview=load("preview.cso");constant(48,{1,1,1,1});ppm("preview_on.ppm",draw(preview,800,200),800,200);
    constant(48,{0,1,1,1});ppm("preview_off.ppm",draw(preview,800,200),800,200);
    WosSkyIBL::Reset();CHECK(!WosSkyIBL::valid&&WosSkyIBL::shader==nullptr);
    printf("PASS checks=%d scenarios=%d diffuse_reference_error=%.9g alpha_preserved=1 off_exact=1 scene_guard=1\n",checks,scenarios,error);
    return 0;
}

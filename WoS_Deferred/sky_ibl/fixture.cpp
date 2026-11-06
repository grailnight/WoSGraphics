#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <random>
#include <vector>

static int checks=0;
#define CHECK(x) do { ++checks; if(!(x)){printf("FAIL line %d: %s\n",__LINE__,#x);exit(1);} } while(0)
using V = std::array<double,3>;
static double dot(V a,V b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
static V unit(V v){double d=sqrt(dot(v,v));for(auto&x:v)x/=d;return v;}
// Independent double precision reference, not a translation of the shader's
// factored D and visibility expressions. Inputs here are already in range.
static V reference(V n,V v,V l,V color,double rough,double metal,double f0){
    double nv=std::max(0.,dot(n,v)),nl=std::max(0.,dot(n,l));
    V h={v[0]+l[0],v[1]+l[1],v[2]+l[2]};
    if(nv<=0||nl<=0||dot(h,h)<=1e-12)return {0,0,0};
    h=unit(h);double nh=std::clamp(dot(n,h),0.,1.),vh=std::clamp(dot(v,h),0.,1.);
    double a2=pow(rough,4),pi=acos(-1.);
    double D=a2/(pi*pow(1+(a2-1)*nh*nh,2));
    double Vg=.5/std::max(nl*sqrt(a2+(1-a2)*nv*nv)+nv*sqrt(a2+(1-a2)*nl*nl),7.7e-6);
    V out{};
    for(int c=0;c<3;++c){double f=f0*(1-metal)+color[c]*metal;f+=(1-f)*pow(1-vh,5);
        double entry=(1-f0)*(1-pow(1-nl,5)),exit=(1-f0)*(1-pow(1-nv,5));
        out[c]=entry*exit*(1-metal)*color[c]/pi+D*Vg*f;}
    return out;
}

static IDirect3DDevice9* device=nullptr;
static IDirect3DPixelShader9* load(const char* file){
    std::ifstream in(file,std::ios::binary);CHECK(in.good());
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)),{});
    IDirect3DPixelShader9* shader=nullptr;
    CHECK(SUCCEEDED(device->CreatePixelShader(reinterpret_cast<const DWORD*>(bytes.data()),&shader)));
    return shader;
}
static void constants(V n,V v,V l,V color,float rough,float metal,float f0,float mode){
    float c[20]={float(n[0]),float(n[1]),float(n[2]),0,float(v[0]),float(v[1]),float(v[2]),0,
        float(l[0]),float(l[1]),float(l[2]),0,float(color[0]),float(color[1]),float(color[2]),rough,metal,f0,mode,0};
    CHECK(SUCCEEDED(device->SetPixelShaderConstantF(0,c,5)));
}
static std::vector<float> draw(IDirect3DPixelShader9* shader,unsigned width,unsigned height){
    IDirect3DSurface9 *rt=nullptr,*read=nullptr;
    CHECK(SUCCEEDED(device->CreateRenderTarget(width,height,D3DFMT_A32B32G32R32F,D3DMULTISAMPLE_NONE,0,FALSE,&rt,nullptr)));
    CHECK(SUCCEEDED(device->CreateOffscreenPlainSurface(width,height,D3DFMT_A32B32G32R32F,D3DPOOL_SYSTEMMEM,&read,nullptr)));
    CHECK(SUCCEEDED(device->SetRenderTarget(0,rt)));
    D3DVIEWPORT9 viewport={0,0,width,height,0,1};CHECK(SUCCEEDED(device->SetViewport(&viewport)));
    CHECK(SUCCEEDED(device->SetPixelShader(shader)));
    struct Vertex{float x,y,z,w,u,v;};
    Vertex quad[]={{-.5f,-.5f,0,1,0,0},{width-.5f,-.5f,0,1,1,0},
                   {-.5f,height-.5f,0,1,0,1},{width-.5f,height-.5f,0,1,1,1}};
    CHECK(SUCCEEDED(device->BeginScene()));
    CHECK(SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(Vertex))));
    CHECK(SUCCEEDED(device->EndScene()));
    CHECK(SUCCEEDED(device->GetRenderTargetData(rt,read)));
    D3DLOCKED_RECT locked{};CHECK(SUCCEEDED(read->LockRect(&locked,nullptr,D3DLOCK_READONLY)));
    std::vector<float> pixels(width*height*4);
    for(unsigned y=0;y<height;++y)std::copy_n(reinterpret_cast<float*>(static_cast<char*>(locked.pBits)+y*locked.Pitch),width*4,pixels.data()+y*width*4);
    CHECK(SUCCEEDED(read->UnlockRect()));read->Release();rt->Release();return pixels;
}

int main(){
    HWND window=CreateWindowExW(0,L"STATIC",L"WoS PBR material laboratory",WS_OVERLAPPEDWINDOW,
                                0,0,800,200,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    CHECK(window);
    IDirect3D9* api=Direct3DCreate9(D3D_SDK_VERSION);CHECK(api);
    D3DADAPTER_IDENTIFIER9 adapter{};CHECK(SUCCEEDED(api->GetAdapterIdentifier(0,0,&adapter)));
    printf("HAL adapter: %s\n",adapter.Description);
    D3DPRESENT_PARAMETERS pp={};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow=window;pp.BackBufferWidth=800;pp.BackBufferHeight=200;pp.BackBufferFormat=D3DFMT_A8R8G8B8;
    CHECK(SUCCEEDED(api->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&device)));
    CHECK(SUCCEEDED(device->SetDepthStencilSurface(nullptr)));
    CHECK(SUCCEEDED(device->SetRenderState(D3DRS_ZENABLE,FALSE)));
    CHECK(SUCCEEDED(device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE)));
    CHECK(SUCCEEDED(device->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE)));
    CHECK(SUCCEEDED(device->SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE)));
    CHECK(SUCCEEDED(device->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1)));
    auto probe=load("probe.cso"),preview=load("preview.cso");
    // Validate actual hardware accepts every full resolve candidate, too.
    for(auto name:{"resolve_red_fabric.cso","resolve_black_suit.cso","resolve_metal.cso","resolve_glass_surface.cso","resolve_concrete.cso"}){
        auto shader=load(name);shader->Release();
    }
    V n={0,0,1};std::mt19937 rng(14321);std::uniform_real_distribution<double> uniform(0,1);
    double maxError=0,maxReciprocity=0;
    for(int i=0;i<240;++i){
        V v=unit({uniform(rng)*2-1,uniform(rng)*2-1,.01+uniform(rng)});
        V l=unit({uniform(rng)*2-1,uniform(rng)*2-1,.01+uniform(rng)});
        V color={uniform(rng),uniform(rng),uniform(rng)};
        float rough=float(.1+.9*uniform(rng)),metal=float(i%3==0?1:i%3==1?0:uniform(rng)),f0=.04f;
        constants(n,v,l,color,rough,metal,f0,0);auto out=draw(probe,1,1);
        auto ref=reference(n,v,l,color,rough,metal,f0);
        constants(n,l,v,color,rough,metal,f0,0);auto reciprocal=draw(probe,1,1);
        for(int c=0;c<3;++c){
            CHECK(std::isfinite(out[c])&&out[c]>=0);
            double err=std::abs(out[c]-ref[c])/std::max(.01,std::abs(ref[c]));maxError=std::max(err,maxError);CHECK(err<.002);
            double diff=std::abs(out[c]-reciprocal[c])/std::max(.01,double(out[c]));maxReciprocity=std::max(diff,maxReciprocity);CHECK(diff<.002);
        }
        CHECK(out[3]==1);
    }
    // Back faces and opposite directions must not generate NaN or light.
    for(V v: {V{0,0,-1},V{0,0,1},V{1,0,0}}){
        constants(n,v,{0,0,-1},{1,1,1},.1f,1,.04f,0);auto out=draw(probe,1,1);
        CHECK(out[0]==0&&out[1]==0&&out[2]==0);
    }
    // At n=v=l, a black metal has neither diffuse nor a normal-incidence lobe.
    constants(n,n,n,{0,0,0},.5f,1,.04f,0);auto blackMetal=draw(probe,1,1);
    CHECK(blackMetal[0]==0&&blackMetal[1]==0&&blackMetal[2]==0);
    // Check finite-precision endpoint and clamping behavior directly on GPU.
    constants(n,n,n,{2,-1,.5},0,2,.9f,0);auto clamped=draw(probe,1,1);
    constants(n,n,n,{1,0,.5},.1f,1,.16f,0);auto bounded=draw(probe,1,1);
    for(int c=0;c<3;++c)CHECK(clamped[c]==bounded[c]&&std::isfinite(clamped[c]));
    double maxEnergy=0,maxIntegralError=0;
    const int size=192;
    for(float rough:{.3f,.6f,.9f})for(float nv:{1.f,.5f,.1f})for(float metal:{0.f,1.f}){
        V v={sqrt(1.-nv*nv),0,nv},color={1,1,1};
        constants(n,v,n,color,rough,metal,.04f,1);auto out=draw(probe,size,size);
        double sum=0,expected=0,pi=acos(-1.);
        for(int y=0;y<size;++y)for(int x=0;x<size;++x){
            double u=(x+.5)/size,phi=(y+.5)/size*2*pi;
            V l={sqrt(u)*cos(phi),sqrt(u)*sin(phi),sqrt(1-u)};
            expected+=pi*reference(n,v,l,color,rough,metal,.04f)[0];
            float value=out[(y*size+x)*4];CHECK(std::isfinite(value)&&value>=0);sum+=value;
        }
        sum/=size*size;expected/=size*size;
        maxIntegralError=std::max(maxIntegralError,std::abs(sum-expected));
        maxEnergy=std::max(maxEnergy,sum);
        CHECK(std::abs(sum-expected)<.003);CHECK(sum<=1.005);
        printf("hemisphere rough %.2f NoV %.2f metal %.0f integral %.7f reference %.7f\n",rough,nv,metal,sum,expected);
    }
    auto pixels=draw(preview,1000,200);
    FILE* file=nullptr;CHECK(fopen_s(&file,"preview.ppm","wb")==0);fprintf(file,"P6\n1000 200\n255\n");
    for(size_t p=0;p<pixels.size();p+=4){unsigned char rgb[3];for(int c=0;c<3;++c){CHECK(std::isfinite(pixels[p+c]));rgb[c]=(unsigned char)std::lround(std::clamp(pixels[p+c],0.f,1.f)*255);}CHECK(fwrite(rgb,1,3,file)==3);}
    fclose(file);
    printf("relative_error %.9g reciprocity_error %.9g integral_error %.9g max_sampled_energy %.9g\n",maxError,maxReciprocity,maxIntegralError,maxEnergy);
    printf("PASS %d checks; 240 reference cases; 18 hemisphere cases\n",checks);
    preview->Release();probe->Release();device->Release();api->Release();DestroyWindow(window);
    return 0;
}

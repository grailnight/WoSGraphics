#pragma once
// One-shot G-buffer dump for material diagnostics (control.ini GbufferDump=1).
// Called by the resolve right before it draws, so it reads exactly what the
// resolve reads: s1 normal/shadow/light-set id, s2 rho/fog, s3 specular/power,
// the residual (native colour without direct light), the AO visibility and the
// CPU copy of the light-set rows. Per material class (texel 31.y of the
// pixel's light set) it writes pixel counts and mean values; batches that hold
// symbiote pixels also get a class map and a specular map as BMP. Read-only:
// it changes no device state except the system-memory copies it creates.
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace WosGbufferDump {
constexpr unsigned MaxClasses=16,Symbiote=13;

static float Half(uint16_t h){
    const uint32_t sign=uint32_t(h>>15)<<31,exponent=(h>>10)&31,mantissa=h&1023u;
    if(exponent==0){const float v=std::ldexp(float(mantissa),-24);return (h>>15)?-v:v;}
    const uint32_t bits=exponent==31?(sign|0x7f800000u|(mantissa<<13)):(sign|((exponent+112)<<23)|(mantissa<<13));
    float f;std::memcpy(&f,&bits,4);return f;
}
// Reads a render target into floats, four per pixel (R16F/R32F fill only the first).
static bool Read(IDirect3DDevice9*d,IDirect3DSurface9*s,std::vector<float>&out,UINT&w,UINT&h){
    out.clear();if(!s)return false;
    D3DSURFACE_DESC desc={};if(FAILED(s->GetDesc(&desc))||desc.MultiSampleType!=D3DMULTISAMPLE_NONE)return false;
    if(desc.Format!=D3DFMT_A16B16G16R16F&&desc.Format!=D3DFMT_R32F&&desc.Format!=D3DFMT_R16F)return false;
    IDirect3DSurface9*cpu=nullptr;
    if(FAILED(d->CreateOffscreenPlainSurface(desc.Width,desc.Height,desc.Format,D3DPOOL_SYSTEMMEM,&cpu,nullptr)))return false;
    bool ok=SUCCEEDED(d->GetRenderTargetData(s,cpu));D3DLOCKED_RECT lr={};
    if(ok)ok=SUCCEEDED(cpu->LockRect(&lr,nullptr,D3DLOCK_READONLY));
    if(ok){
        w=desc.Width;h=desc.Height;out.assign(size_t(w)*h*4,0.f);
        for(UINT y=0;y<h;++y){
            const unsigned char*row=static_cast<const unsigned char*>(lr.pBits)+size_t(y)*lr.Pitch;
            float*dst=&out[size_t(y)*w*4];
            if(desc.Format==D3DFMT_A16B16G16R16F){const uint16_t*p=reinterpret_cast<const uint16_t*>(row);for(UINT x=0;x<w*4;++x)dst[x]=Half(p[x]);}
            else if(desc.Format==D3DFMT_R32F){const float*p=reinterpret_cast<const float*>(row);for(UINT x=0;x<w;++x)dst[x*4]=p[x];}
            else{const uint16_t*p=reinterpret_cast<const uint16_t*>(row);for(UINT x=0;x<w;++x)dst[x*4]=Half(p[x]);}
        }
        cpu->UnlockRect();
    }
    cpu->Release();return ok;
}
static bool WriteBmp(const std::string&path,UINT w,UINT h,const std::vector<unsigned char>&bgr){
    const uint32_t stride=(w*3+3)&~3u,size=stride*h;
    BITMAPFILEHEADER file={};BITMAPINFOHEADER info={};
    file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(info);file.bfSize=file.bfOffBits+size;
    info.biSize=sizeof(info);info.biWidth=LONG(w);info.biHeight=-LONG(h);info.biPlanes=1;info.biBitCount=24;info.biCompression=BI_RGB;info.biSizeImage=size;
    FILE*f=nullptr;if(fopen_s(&f,path.c_str(),"wb")||!f)return false;
    bool ok=fwrite(&file,sizeof(file),1,f)==1&&fwrite(&info,sizeof(info),1,f)==1;
    std::vector<unsigned char> row(stride,0);
    for(UINT y=0;ok&&y<h;++y){std::memcpy(row.data(),&bgr[size_t(y)*w*3],size_t(w)*3);ok=fwrite(row.data(),stride,1,f)==1;}
    return fclose(f)==0&&ok;
}
struct Inputs{
    IDirect3DSurface9*normalShadowSet=nullptr,*rho=nullptr,*specular=nullptr,*residual=nullptr,*visibility=nullptr;
    const std::vector<float>*rows=nullptr;unsigned rowFloats=128;
    float strength=0,table[MaxClasses][4]={},intensity=0,specularGain=0,ambient=0,skyDiffuse=0,skySpecular=0;
    bool skyEnabled=false;int view=0;
};
struct Acc{double n=0,spec[4]={},rho[4]={},residual[3]={},visibility=0,shadow=0,bright=0;float specMax=0;unsigned hist[200]={},power[257]={};};
static float Percentile(const unsigned*hist,unsigned bins,double n,double q,float scale){
    double want=n*q,sum=0;for(unsigned i=0;i<bins;++i){sum+=hist[i];if(sum>=want)return (i+.5f)*scale;}
    return bins*scale;
}
// Returns false when a required target could not be read; the text then says why.
static bool Dump(IDirect3DDevice9*d,const Inputs&in,const std::string&folder,const std::string&stem,unsigned batch,unsigned&symbiotePixels){
    symbiotePixels=0;
    const std::string text=folder+"\\"+stem+"_b"+std::to_string(batch)+".txt";
    FILE*f=nullptr;if(fopen_s(&f,text.c_str(),"w")||!f)return false;
    std::vector<float> ns,rho,sp,res,vis;UINT w=0,h=0,w2=0,h2=0,w3=0,h3=0,w4=0,h4=0,w5=0,h5=0;
    const bool okNs=Read(d,in.normalShadowSet,ns,w,h),okRho=Read(d,in.rho,rho,w2,h2),okSp=Read(d,in.specular,sp,w3,h3);
    const bool okRes=Read(d,in.residual,res,w4,h4)&&w4==w&&h4==h;
    const bool okVis=in.visibility&&Read(d,in.visibility,vis,w5,h5)&&w5==w&&h5==h;
    fprintf(f,"WoS G-buffer dump v1  batch %u  %ux%u  read: ns %d rho %d spec %d residual %d visibility %d\n",batch,w,h,okNs,okRho,okSp,okRes,okVis);
    fprintf(f,"settings: intensity %.4f specular %.4f view %d  class strength %.4f  AO strength %.4f  sky IBL %d diffuse %.3f specular %.3f\n",
            in.intensity,in.specularGain,in.view,in.strength,in.ambient,int(in.skyEnabled),in.skyDiffuse,in.skySpecular);
    fprintf(f,"symbiote row (floor ceiling metal reflectance): %.3f %.3f %.3f %.3f\n",in.table[Symbiote][0],in.table[Symbiote][1],in.table[Symbiote][2],in.table[Symbiote][3]);
    const unsigned sets=in.rows?unsigned(in.rows->size()/in.rowFloats):0;
    if(!okNs||!okRho||!okSp||w!=w2||w!=w3||h!=h2||h!=h3||!sets){fprintf(f,"incomplete inputs, light sets %u\n",sets);fclose(f);return false;}
    const std::vector<float>&rows=*in.rows;
    auto classOf=[&](unsigned id){int c=int(std::lround(rows[size_t(id)*in.rowFloats+31*4+1]));return unsigned(c<0||c>=int(MaxClasses)?MaxClasses-1:c);};
    unsigned perClassSets[MaxClasses]={};unsigned firstSymbiote=0;
    for(unsigned id=1;id<sets;++id){const unsigned c=classOf(id);++perClassSets[c];if(c==Symbiote&&!firstSymbiote)firstSymbiote=id;}
    fprintf(f,"light sets %u, per class:",sets);for(unsigned c=0;c<MaxClasses;++c)if(perClassSets[c])fprintf(f," %u:%u",c,perClassSets[c]);fputc('\n',f);
    if(firstSymbiote){
        const float*r=&rows[size_t(firstSymbiote)*in.rowFloats];
        fprintf(f,"first symbiote set %u: sun dir %.3f %.3f %.3f (points %.0f)  sun colour %.3f %.3f %.3f (baked %.0f)  texel31 %.3f %.3f\n",
                firstSymbiote,r[0],r[1],r[2],r[3],r[4],r[5],r[6],r[7],r[31*4],r[31*4+1]);
    }
    static Acc acc[MaxClasses];for(auto&a:acc)a=Acc{};
    std::vector<unsigned char> classMap(size_t(w)*h*3,0),specMap(size_t(w)*h*3,0);
    for(size_t p=0;p<size_t(w)*h;++p){
        const float*n=&ns[p*4];const int id=int(std::floor(n[3]+.5f));
        const float*s=&sp[p*4];
        for(int k=0;k<3;++k)specMap[p*3+2-k]=static_cast<unsigned char>((std::min)(255.f,(std::max)(0.f,s[k]*255.f)));
        if(id<1||unsigned(id)>=sets)continue;
        const unsigned c=classOf(unsigned(id));Acc&a=acc[c];a.n+=1;
        const float mx=(std::max)(s[0],(std::max)(s[1],s[2]));
        for(int k=0;k<4;++k){a.spec[k]+=s[k];a.rho[k]+=rho[p*4+k];}
        if(okRes)for(int k=0;k<3;++k)a.residual[k]+=res[p*4+k];
        if(okVis)a.visibility+=vis[p*4];
        a.shadow+=n[2];a.specMax=(std::max)(a.specMax,mx);if(mx>.3f)a.bright+=1;
        ++a.hist[(std::min)(199u,unsigned((std::max)(0.f,mx)*100.f))];
        ++a.power[(std::min)(256u,unsigned((std::max)(0.f,s[3])))];
        unsigned char*m=&classMap[p*3];
        if(c==Symbiote){const float k=.35f+.65f*(std::min)(1.f,mx*2.f);m[0]=static_cast<unsigned char>(255*k);m[1]=0;m[2]=static_cast<unsigned char>(255*k);}
        else if(c==12){m[0]=20;m[1]=40;m[2]=110;}
        else if(c==0){m[0]=m[1]=m[2]=70;}
        else{static const unsigned char hue[12][3]={{0,0,0},{200,200,60},{150,150,150},{60,60,200},{40,40,40},{230,200,90},{40,120,200},{60,140,60},{30,30,90},{180,180,220},{40,200,40},{200,120,60}};
             m[0]=hue[c%12][0];m[1]=hue[c%12][1];m[2]=hue[c%12][2];}
    }
    fprintf(f,"\nper class: pixels | mean spec rgb, power | spec max p50 p90 max, share > .3 | power p50 p90 | mean rho rgb, rho.a | mean residual rgb | mean AO texture (1 open) | mean native shadow\n");
    for(unsigned c=0;c<MaxClasses;++c){
        const Acc&a=acc[c];if(!a.n)continue;const double n=a.n;
        fprintf(f,"class %2u %8.0f | %.4f %.4f %.4f %6.1f | %.3f %.3f %.3f %.3f | %5.1f %5.1f | %.4f %.4f %.4f %.3f | %.4f %.4f %.4f | %.3f | %.3f\n",c,n,
                a.spec[0]/n,a.spec[1]/n,a.spec[2]/n,a.spec[3]/n,
                Percentile(a.hist,200,n,.5,.01f),Percentile(a.hist,200,n,.9,.01f),a.specMax,a.bright/n,
                Percentile(a.power,257,n,.5,1.f),Percentile(a.power,257,n,.9,1.f),
                a.rho[0]/n,a.rho[1]/n,a.rho[2]/n,a.rho[3]/n,a.residual[0]/n,a.residual[1]/n,a.residual[2]/n,okVis?a.visibility/n:-1.,a.shadow/n);
    }
    symbiotePixels=unsigned(acc[Symbiote].n);
    if(symbiotePixels){
        const std::string base=folder+"\\"+stem+"_b"+std::to_string(batch);
        fprintf(f,"\nclass map: %s (magenta = symbiote, brighter = brighter specular; grey = no class; brown = facade)\n",(base+"_classes.bmp").c_str());
        fprintf(f,"specular map: %s (G-buffer specular rgb, 1.0 = white)\n",(base+"_specular.bmp").c_str());
        WriteBmp(base+"_classes.bmp",w,h,classMap);WriteBmp(base+"_specular.bmp",w,h,specMap);
    }
    fclose(f);return true;
}
}

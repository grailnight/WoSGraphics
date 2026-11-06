#pragma once
// One-frame partial normal G-buffer diagnostic. This is not a lighting replacement.
namespace WosNormals {
static bool active=false,loaded=false;
static unsigned draws=0,occlusionDraws=0;
static const char *status="F9: capture partial world-normal buffer";
struct Variant{std::vector<unsigned char> input;IDirect3DPixelShader9 *shader=nullptr;bool normal=true;};
static std::vector<Variant> variants;
static std::unordered_map<IDirect3DPixelShader9 *,int> cache;
static IDirect3DTexture9 *texture=nullptr;
static IDirect3DSurface9 *surface=nullptr,*sceneColor=nullptr,*sceneDepth=nullptr;
static UINT width=0,height=0;
static void ReleaseBuffer(){if(surface)surface->Release();if(texture)texture->Release();if(sceneColor)sceneColor->Release();if(sceneDepth)sceneDepth->Release();surface=sceneColor=sceneDepth=nullptr;texture=nullptr;width=height=0;}
static void Reset(){active=false;ReleaseBuffer();for(auto &e:cache)e.first->Release();cache.clear();for(auto &v:variants)if(v.shader)v.shader->Release();variants.clear();loaded=false;}
static bool LoadOne(IDirect3DDevice9 *d,const std::string &base,bool normal){
 auto input=WosPCSS::Read(base+".input.cso"),output=WosPCSS::Read(base+".mrt.cso");
 if(input.empty()||output.empty())return false;IDirect3DPixelShader9 *ps=nullptr;
 if(FAILED(d->CreatePixelShader(reinterpret_cast<const DWORD *>(output.data()),&ps)))return false;
 variants.push_back({std::move(input),ps,normal});return true;
}
static void Load(IDirect3DDevice9 *d){
 if(loaded)return;loaded=true;unsigned failures=0;
 for(int id:{9,11,12,13,17,95,97})for(const char *mode:{"original","pcss"}){
  char n[64];std::snprintf(n,sizeof(n),"\\normals\\ps_%04d.%s",id,mode);
  if(!LoadOne(d,WosProbe::Root()+n,true))++failures;
 }
 std::string dir=WosProbe::Root()+"\\normals\\occlusion\\";
 WIN32_FIND_DATAA info={};HANDLE search=FindFirstFileA((dir+"*.input.cso").c_str(),&info);
 if(search!=INVALID_HANDLE_VALUE){
  do{if(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)continue;std::string name(info.cFileName);
   if(name.size()<10||variants.size()>=512)continue;name.resize(name.size()-10);
   if(!LoadOne(d,dir+name,false))++failures;
  }while(FindNextFileA(search,&info));FindClose(search);
 }else ++failures;
 if(failures)status="Normal capture: some variants failed to load; coverage is incomplete";
}
static IDirect3DPixelShader9 *Find(IDirect3DDevice9 *d,IDirect3DPixelShader9 *ps,bool &normal){
 Load(d);auto it=cache.find(ps);int index=-1;
 if(it!=cache.end())index=it->second;
 else{
  UINT size=0;if(SUCCEEDED(ps->GetFunction(nullptr,&size))){bool candidate=false;for(auto &v:variants)if(v.input.size()==size)candidate=true;
   if(candidate){std::vector<unsigned char>b(size);if(SUCCEEDED(ps->GetFunction(b.data(),&size)))for(size_t i=0;i<variants.size();++i)if(b==variants[i].input){index=static_cast<int>(i);break;}}
  }
  ps->AddRef();cache.emplace(ps,index);
 }
 if(index>=0)normal=variants[index].normal;
 return index<0?nullptr:variants[index].shader;
}
static bool Ensure(IDirect3DDevice9 *d,IDirect3DSurface9 *color,IDirect3DSurface9 *depth,const D3DSURFACE_DESC &desc){
 if(surface)return color==sceneColor&&depth==sceneDepth;
 D3DCAPS9 caps={};if(FAILED(d->GetDeviceCaps(&caps))||caps.NumSimultaneousRTs<2)return false;
 if(FAILED(d->CreateTexture(desc.Width,desc.Height,1,D3DUSAGE_RENDERTARGET,D3DFMT_A16B16G16R16F,D3DPOOL_DEFAULT,&texture,nullptr)))return false;
 if(FAILED(texture->GetSurfaceLevel(0,&surface))){ReleaseBuffer();return false;}
 // Clear only our target. Save/restore bindings and viewport explicitly: stateblocks do not save targets.
 D3DVIEWPORT9 vp={};if(FAILED(d->GetViewport(&vp))){ReleaseBuffer();return false;}
 bool ok=SUCCEEDED(d->SetDepthStencilSurface(nullptr));
 if(ok)ok=SUCCEEDED(d->SetRenderTarget(0,surface));
 D3DVIEWPORT9 full={0,0,desc.Width,desc.Height,0,1};
 if(ok)ok=SUCCEEDED(d->SetViewport(&full));
 if(ok)ok=SUCCEEDED(d->Clear(0,nullptr,D3DCLEAR_TARGET,0,1,0));
 HRESULT a=d->SetRenderTarget(0,color),b=d->SetDepthStencilSurface(depth),c=d->SetViewport(&vp);
 if(!ok||FAILED(a)||FAILED(b)||FAILED(c)){ReleaseBuffer();return false;}
 color->AddRef();depth->AddRef();sceneColor=color;sceneDepth=depth;width=desc.Width;height=desc.Height;return true;
}
struct DrawScope{
 IDirect3DDevice9 *device=nullptr;IDirect3DPixelShader9 *original=nullptr;DWORD writeMask=0;bool changed=false;
 DrawScope(IDirect3DDevice9 *d){
  if(!active||WosProbe::suppress)return;
  DWORD blend=0,zwrite=0,ztest=0,srgb=0;
  if(FAILED(d->GetRenderState(D3DRS_ALPHABLENDENABLE,&blend))||blend||FAILED(d->GetRenderState(D3DRS_ZWRITEENABLE,&zwrite))||!zwrite||FAILED(d->GetRenderState(D3DRS_ZENABLE,&ztest))||!ztest||FAILED(d->GetRenderState(D3DRS_SRGBWRITEENABLE,&srgb))||srgb)return;
  for(unsigned i=1;i<4;++i){IDirect3DSurface9 *rt=nullptr;HRESULT hr=d->GetRenderTarget(i,&rt);if(rt){rt->Release();return;}if(FAILED(hr)&&hr!=D3DERR_NOTFOUND)return;}
  if(FAILED(d->GetPixelShader(&original))||!original)return;bool normal=false;auto replacement=Find(d,original,normal);if(!replacement)return;
  IDirect3DSurface9 *color=nullptr,*depth=nullptr;
  if(FAILED(d->GetRenderTarget(0,&color))||!color)return;
  if(FAILED(d->GetDepthStencilSurface(&depth))||!depth){color->Release();return;}
  D3DSURFACE_DESC desc={};D3DVIEWPORT9 vp={};
  bool ok=SUCCEEDED(color->GetDesc(&desc))&&desc.Format==D3DFMT_A16B16G16R16F&&desc.MultiSampleType==D3DMULTISAMPLE_NONE&&desc.Width>=640&&SUCCEEDED(d->GetViewport(&vp))&&vp.X==0&&vp.Y==0&&vp.Width==desc.Width&&vp.Height==desc.Height;
  if(ok)ok=Ensure(d,color,depth,desc);color->Release();depth->Release();if(!ok)return;
  if(FAILED(d->GetRenderState(D3DRS_COLORWRITEENABLE1,&writeMask)))return;
  device=d;changed=true;
  if(FAILED(d->SetRenderTarget(1,surface))||FAILED(d->SetRenderState(D3DRS_COLORWRITEENABLE1,15))||FAILED(g_original_set_pixel_shader(d,replacement))){Restore();return;}
  if(normal)++draws;else ++occlusionDraws;
 }
 void Restore(){if(changed){g_original_set_pixel_shader(device,original);device->SetRenderTarget(1,nullptr);device->SetRenderState(D3DRS_COLORWRITEENABLE1,writeMask);changed=false;}}
 ~DrawScope(){Restore();if(original)original->Release();}
};
static float Half(USHORT h){int e=(h>>10)&31;float x=e==0?std::ldexp(float(h&1023),-24):(e==31?0:std::ldexp(float(1024+(h&1023)),e-25));return h&32768?-x:x;}
static bool Save(){
 if(!surface||!draws)return false;
 IDirect3DDevice9 *d=nullptr;if(FAILED(surface->GetDevice(&d)))return false;
 IDirect3DSurface9 *cpu=nullptr;HRESULT hr=d->CreateOffscreenPlainSurface(width,height,D3DFMT_A16B16G16R16F,D3DPOOL_SYSTEMMEM,&cpu,nullptr);
 if(SUCCEEDED(hr))hr=d->GetRenderTargetData(surface,cpu);d->Release();if(FAILED(hr)){if(cpu)cpu->Release();return false;}
 D3DLOCKED_RECT locked={};if(FAILED(cpu->LockRect(&locked,nullptr,D3DLOCK_READONLY))){cpu->Release();return false;}
 std::string root=WosProbe::Root()+"\\gbuffer";CreateDirectoryA(root.c_str(),nullptr);
 SYSTEMTIME t;GetLocalTime(&t);char name[96];std::snprintf(name,sizeof(name),"\\%04u%02u%02u_%02u%02u%02u_%llu",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,GetTickCount64());std::string base=root+name;
 FILE *raw=std::fopen((base+".rgba16f").c_str(),"wb"),*bmp=std::fopen((base+".bmp").c_str(),"wb");bool ok=raw&&bmp;
 const DWORD stride=(width*3+3)&~3u;
 BITMAPFILEHEADER fh={};fh.bfType=0x4d42;fh.bfOffBits=sizeof(fh)+sizeof(BITMAPINFOHEADER);fh.bfSize=fh.bfOffBits+stride*height;
 BITMAPINFOHEADER ih={};ih.biSize=sizeof(ih);ih.biWidth=width;ih.biHeight=-static_cast<LONG>(height);ih.biPlanes=1;ih.biBitCount=24;ih.biCompression=BI_RGB;
 if(bmp){ok=(std::fwrite(&fh,1,sizeof(fh),bmp)==sizeof(fh))&&ok;ok=(std::fwrite(&ih,1,sizeof(ih),bmp)==sizeof(ih))&&ok;}
 std::vector<unsigned char> row(stride,0);unsigned valid=0;
 for(UINT y=0;y<height;++y){auto p=reinterpret_cast<const USHORT *>(static_cast<const unsigned char *>(locked.pBits)+y*locked.Pitch);
  if(raw)ok=(std::fwrite(p,1,width*8,raw)==width*8)&&ok;
  for(UINT x=0;x<width;++x){bool covered=Half(p[x*4+3])>.5f;if(covered)++valid;
   for(unsigned c=0;c<3;++c){float v=covered?Half(p[x*4+2-c]):0;row[x*3+c]=static_cast<unsigned char>((std::max)(0.f,(std::min)(1.f,v))*255.f+.5f);}
  }
  if(bmp)ok=(std::fwrite(row.data(),1,row.size(),bmp)==row.size())&&ok;
 }
 if(raw)ok=(std::fclose(raw)==0)&&ok;if(bmp)ok=(std::fclose(bmp)==0)&&ok;
 cpu->UnlockRect();cpu->Release();FILE *meta=std::fopen((base+".json").c_str(),"wb");
 if(meta){std::fprintf(meta,"{\"width\":%u,\"height\":%u,\"draws\":%u,\"invalidating_draws\":%u,\"valid_pixels\":%u,\"format\":\"RGBA16F little endian, tightly packed\",\"encoding\":\"RGB=world normal*0.5+0.5; A=material coverage\",\"partial\":true,\"limitation\":\"Captured opaque depth-writing occluders clear coverage. Unknown shaders and other passes may still leave incomplete visibility. Not ready for relighting.\"}\n",width,height,draws,occlusionDraws,valid);ok=(std::fclose(meta)==0)&&ok;}else ok=false;
 return ok;
}
static void FrameEnd(){
 if(active){active=false;status=Save()?"Normal buffer saved in WoS_Deferred/gbuffer":"Normal capture empty or failed (no supported opaque facade)";ReleaseBuffer();}
 if(GetForegroundWindow()==g_window&&(GetAsyncKeyState(VK_F9)&1)){draws=0;occlusionDraws=0;active=true;status="Capturing partial world normals for one frame...";}
}
}
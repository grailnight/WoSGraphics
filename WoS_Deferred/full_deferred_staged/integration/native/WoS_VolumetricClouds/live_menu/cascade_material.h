#pragma once
// Verified opaque main-scene materials only. Own maps never bind during replay.
namespace WosCascadeMaterial {
struct Variant {std::vector<unsigned char> original,pcss;IDirect3DPixelShader9*shader=nullptr;};
static std::vector<Variant> variants;
static std::unordered_map<IDirect3DPixelShader9*,int> cache;
static bool loaded=false;
static unsigned hits=0,lastHits=0;
static const char*status="F11: cascade material sampler OFF";
static void Reset(){for(auto&v:variants)if(v.shader)v.shader->Release();variants.clear();for(auto&e:cache)e.first->Release();cache.clear();loaded=false;hits=lastHits=0;}
static bool reportWritten=false;
static unsigned reportFrames=0,fullFrames=0,refreshCounts[5]={};
static unsigned long long replaySum=0;
static double prepareSum=0,replayCpuSum=0;
static void FrameEnd(){
 lastHits=hits;hits=0;
 if(!WosCascadeProbe::continuous){reportWritten=false;reportFrames=fullFrames=0;replaySum=0;prepareSum=replayCpuSum=0;for(auto&n:refreshCounts)n=0;return;}
 if(!lastHits)return;
 ++reportFrames;prepareSum+=WosCascadeProbe::prepareCpuMs;replayCpuSum+=WosCascadeProbe::replayCpuMs;replaySum+=WosCascadeProbe::lastGpuDraws;if(WosCascadeProbe::updateMask==15)++fullFrames;
 if(WosCascadeProbe::refreshReason<5)++refreshCounts[WosCascadeProbe::refreshReason];
 if(reportWritten&&reportFrames<120)return;
 FILE*f=std::fopen((WosProbe::Root()+"\\cascade_live_status.json").c_str(),"wb");
 if(f){std::fprintf(f,"{\"revision\":5,\"skipped_animated_draws\":%u,\"mean_prepare_cpu_ms\":%.4f,\"mean_replay_cpu_ms\":%.4f,\"sample_frames\":%u,\"full_refresh_frames\":%u,\"mean_gpu_replays\":%.2f,\"refresh_reason_counts\":[%u,%u,%u,%u,%u],\"gpu_replay_draws\":%u,\"update_mask\":%u,\"resolution\":1024,\"partial\":true,\"material_draws\":%u,\"failed_replays\":%u}\n",WosCascadeProbe::skippedAnimated,prepareSum/reportFrames,replayCpuSum/reportFrames,reportFrames,fullFrames,double(replaySum)/reportFrames,refreshCounts[0],refreshCounts[1],refreshCounts[2],refreshCounts[3],refreshCounts[4],WosCascadeProbe::lastGpuDraws,WosCascadeProbe::updateMask,lastHits,WosCascadeProbe::failed);std::fclose(f);reportWritten=true;reportFrames=fullFrames=0;replaySum=0;prepareSum=replayCpuSum=0;for(auto&n:refreshCounts)n=0;}
}
static void Load(IDirect3DDevice9*d){
 if(loaded)return;loaded=true;
 for(int id:{9,11,12,95,97}){char name[80];std::snprintf(name,sizeof(name),"\\cascade_materials\\ps_%04d",id);std::string base=WosProbe::Root()+name;Variant v;
  v.original=WosCascadeProbe::ReadFile(base+".original.cso");auto bytes=WosCascadeProbe::ReadFile(base+".cascade.cso");
  std::snprintf(name,sizeof(name),"\\pcss\\ps_%04d.pcss.cso",id);v.pcss=WosCascadeProbe::ReadFile(WosProbe::Root()+name);
  if(v.original.empty()||bytes.empty()||FAILED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(bytes.data()),&v.shader)))continue;variants.push_back(std::move(v));
 }
 status=variants.size()==5?"Cascade materials: five variants ready, native shadows retained":"Cascade materials: incomplete shader load";
}
static IDirect3DPixelShader9*Replacement(IDirect3DDevice9*d,IDirect3DPixelShader9*ps){
 Load(d);auto it=cache.find(ps);if(it!=cache.end())return it->second<0?nullptr:variants[it->second].shader;
 int found=-1;UINT n=0;if(SUCCEEDED(ps->GetFunction(nullptr,&n))&&n&&n<1048576){std::vector<unsigned char>b(n);if(SUCCEEDED(ps->GetFunction(b.data(),&n)))for(size_t i=0;i<variants.size();++i)if(b==variants[i].original||b==variants[i].pcss){found=static_cast<int>(i);break;}}
 ps->AddRef();cache.emplace(ps,found);return found<0?nullptr:variants[found].shader;
}
struct DrawScope {
 IDirect3DDevice9*d=nullptr;IDirect3DPixelShader9*original=nullptr;IDirect3DBaseTexture9*textures[4]={};float constants[72]={};DWORD states[4][6]={};bool changed=false;
 static constexpr D3DSAMPLERSTATETYPE types[6]={D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER,D3DSAMP_MIPFILTER,D3DSAMP_ADDRESSU,D3DSAMP_ADDRESSV,D3DSAMP_SRGBTEXTURE};
 DrawScope(IDirect3DDevice9*dev){
  using namespace WosCascadeProbe;
  if(!continuous||!ready||busy||!replayed||failed||WosProbe::suppress)return;
  auto g=WosEngineProbe::ReadGlobals();if(g.valid!=7||g.current!=mainScene||g.shadowActive)return;
  DWORD blend=0,zwrite=0;if(FAILED(dev->GetRenderState(D3DRS_ALPHABLENDENABLE,&blend))||blend||FAILED(dev->GetRenderState(D3DRS_ZWRITEENABLE,&zwrite))||!zwrite)return;
  if(FAILED(dev->GetPixelShader(&original))||!original)return;auto ps=Replacement(dev,original);if(!ps)return;
  if(FAILED(dev->GetPixelShaderConstantF(180,constants,18)))return;
  for(unsigned i=0;i<4;++i){if(FAILED(dev->GetTexture(12+i,&textures[i])))return;for(unsigned j=0;j<6;++j)if(FAILED(dev->GetSamplerState(12+i,types[j],&states[i][j])))return;}
  d=dev;changed=true;float upload[72]={};
  for(unsigned i=0;i<4;++i)for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)upload[i*16+r*4+c]=plan.cascades[i].vp.v[c*4+r];
  memcpy(upload+64,cameraDepthPlane,16);for(unsigned i=0;i<4;++i)upload[68+i]=static_cast<float>(plan.cascades[i].farDepth);
  bool ok=SUCCEEDED(d->SetPixelShaderConstantF(180,upload,18));const DWORD values[6]={D3DTEXF_POINT,D3DTEXF_POINT,D3DTEXF_NONE,D3DTADDRESS_CLAMP,D3DTADDRESS_CLAMP,0};
  for(unsigned i=0;i<4;++i){ok=SUCCEEDED(d->SetTexture(12+i,targets.textures[i]))&&ok;for(unsigned j=0;j<6;++j)ok=SUCCEEDED(d->SetSamplerState(12+i,types[j],values[j]))&&ok;}
  if(ok&&SUCCEEDED(g_original_set_pixel_shader(d,ps)))++hits;else Restore();
 }
 void Restore(){if(!changed)return;g_original_set_pixel_shader(d,original);d->SetPixelShaderConstantF(180,constants,18);for(unsigned i=0;i<4;++i){d->SetTexture(12+i,textures[i]);for(unsigned j=0;j<6;++j)d->SetSamplerState(12+i,types[j],states[i][j]);}changed=false;}
 ~DrawScope(){Restore();for(auto*t:textures)if(t)t->Release();if(original)original->Release();}
};
}

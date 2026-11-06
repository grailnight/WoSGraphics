#pragma once
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/shadow_cascades.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/indirect_fixed_globals.h"

// Scene layout established in Ghidra (009E97E0 / 009DB830), then compared
// bit-for-bit with the GPU constants in real F7 captures. No shader CTAB needed.
namespace WosIndirectCamera {
using WosShadowTransform::Matrix4;
struct Camera { float inverse[16]={},position[3]={},relativeInverse[16]={};double worldOrigin[3]={};uint32_t scene=0; };
inline bool Decode(const Matrix4&vp,Camera&out){
 double inv[16];if(!WosCascades::Inverse(vp,inv))return false;
 double directionLength=std::sqrt(double(vp.v[3])*vp.v[3]+double(vp.v[7])*vp.v[7]+double(vp.v[11])*vp.v[11]);
 if(!std::isfinite(directionLength)||directionLength<.5||directionLength>2||std::abs(inv[11])<1e-10)return false;
 // For perspective VP, inverse * clip(0,0,1,0) is the eye in homogeneous
 // coordinates. This also works for the game's off-centre projection.
 Camera c;double eye[3];
 for(unsigned i=0;i<3;++i){eye[i]=inv[8+i]/inv[11];if(!std::isfinite(eye[i])||std::abs(eye[i])>1e7)return false;c.position[i]=float(eye[i]);c.worldOrigin[i]=eye[i];}
 WosCascades::V3 nearPoint,midPoint;
 if(!WosCascades::Unproject(inv,0,0,0,nearPoint)||!WosCascades::Unproject(inv,0,0,.5,midPoint))return false;
 auto clipW=[&](WosCascades::V3 p){return p.x*vp.v[3]+p.y*vp.v[7]+p.z*vp.v[11]+vp.v[15];};
 if(clipW(nearPoint)<=0||clipW(midPoint)<=clipW(nearPoint))return false;
 // Engine VP uses row vectors; HLSL reconstruction uses dot-product rows.
 // Rebase in double before packing float constants: subtracting the rounded
 // float eye leaves large cancellation in depth-derived surface normals.
 // Only the origin changes; native world-axis normals keep their orientation.
 for(unsigned r=0;r<4;++r)for(unsigned col=0;col<4;++col){
  double v=inv[col*4+r],relative=r<3?v-eye[r]*inv[col*4+3]:v;
  if(!std::isfinite(v)||std::abs(v)>1e10||!std::isfinite(relative)||std::abs(relative)>1e10)return false;
  c.inverse[r*4+col]=float(v);c.relativeInverse[r*4+col]=float(relative);
 }
 out=c;return true;
}
struct Entry { IUnknown*depth=nullptr;Camera camera;unsigned width=0,height=0;bool ambiguous=false; };
static std::vector<Entry> entries;
static bool verified=false,verificationTried=false,frameReady=false;
static uint32_t arenaBegin=0,arenaEnd=0,lastScene=0;
// Armed only by Begin after exact image/signature and arena validation.
static uintptr_t fixedGlobalsBase=0;
static unsigned captures=0,rejected=0;
// Moves whenever the camera table or frameReady may change; the Deferred
// verdict cache keys on it instead of repeating Find.
static unsigned serial=1;
static const char*status="engine camera inactive";
static void Clear(){for(auto&e:entries)if(e.depth)e.depth->Release();entries.clear();lastScene=0;frameReady=false;fixedGlobalsBase=0;++serial;}
static void Reset(){Clear();captures=rejected=0;}
static bool Record(IDirect3DBaseTexture9*texture,uint32_t scene,const Matrix4&vp,unsigned w,unsigned h){
 Camera camera;if(!frameReady||!texture||!Decode(vp,camera))return false;camera.scene=scene;
 IUnknown*identity=nullptr;if(FAILED(texture->QueryInterface(IID_IUnknown,reinterpret_cast<void**>(&identity))))return false;
 for(auto&e:entries)if(e.depth==identity&&e.camera.scene==scene){
  // Distinct engine scenes may reuse one depth texture with different projections.
  // A changing matrix within the same scene is ambiguous and rejected.
  if(e.width!=w||e.height!=h||std::memcmp(e.camera.inverse,camera.inverse,sizeof(camera.inverse))||std::memcmp(e.camera.relativeInverse,camera.relativeInverse,sizeof(camera.relativeInverse))||e.camera.worldOrigin[0]!=camera.worldOrigin[0]||e.camera.worldOrigin[1]!=camera.worldOrigin[1]||e.camera.worldOrigin[2]!=camera.worldOrigin[2]){e.ambiguous=true;++serial;}
  identity->Release();return !e.ambiguous;
 }
 if(entries.size()>=16){identity->Release();return false;}
 entries.push_back({identity,camera,w,h,false});++captures;++serial;return true;
}
static bool Find(IDirect3DBaseTexture9*texture,unsigned w,unsigned h,Camera&out,uint32_t scene){
 if(!frameReady||!texture||!scene)return false;IUnknown*identity=nullptr;
 if(FAILED(texture->QueryInterface(IID_IUnknown,reinterpret_cast<void**>(&identity))))return false;
 bool found=false;for(const auto&e:entries)if(e.depth==identity&&e.camera.scene==scene&&e.width==w&&e.height==h&&!e.ambiguous){out=e.camera;found=true;break;}
 identity->Release();return found;
}
// Own the INTZ obtained from this frame's bound scene depth surface. No legacy
// post-process shader, external texture pointer, or depth-capture flag needed.
static bool Acquire(unsigned w,unsigned h,Camera&out,IDirect3DTexture9*&depth,uint32_t scene){
 depth=nullptr;if(!frameReady||!scene)return false;
 const Entry*match=nullptr;
 for(const auto&e:entries)if(e.camera.scene==scene&&e.width==w&&e.height==h){
  if(e.ambiguous||match)return false;match=&e;
 }
 if(!match||FAILED(match->depth->QueryInterface(IID_IDirect3DTexture9,reinterpret_cast<void**>(&depth))))return false;
 D3DSURFACE_DESC desc={};
 if(FAILED(depth->GetLevelDesc(0,&desc))||desc.Width!=w||desc.Height!=h){depth->Release();depth=nullptr;return false;}
 out=match->camera;return true;
}
static bool FixedGlobalsReady(){
 return frameReady&&verified&&fixedGlobalsBase==0x400000&&WosEngineProbe::imageBase==fixedGlobalsBase;
}
static WosIndirectFixedGlobals::Snapshot ReadFrameGlobals(){
 if(FixedGlobalsReady())return WosIndirectFixedGlobals::ReadGlobals(fixedGlobalsBase,true);
 // Unarmed diagnostic/synthetic callers retain the conservative existing path.
 auto g=WosEngineProbe::ReadGlobals();
 return {g.current,g.root,g.nearScene,g.farScene,g.shadowActive,g.valid};
}
static bool ReadFrameCurrentScene(uint32_t&scene){
 if(FixedGlobalsReady())return WosIndirectFixedGlobals::ReadCurrentScene(fixedGlobalsBase,true,scene);
 return WosEngineProbe::Read(WosEngineProbe::imageBase+0xd15c54,&scene,4);
}
static uint32_t CurrentScene(){
 if(!frameReady)return 0;auto g=ReadFrameGlobals();
 if(g.valid!=7||g.shadowActive||g.current<arenaBegin||uint64_t(g.current)+0x4e0>arenaEnd||g.current==g.nearScene||g.current==g.farScene)return 0;
 return g.current;
}
static void Begin(uintptr_t caller){
 if(caller!=0x009dcf3a)return;
 Clear();
 if(!verificationTried){verificationTried=true;std::string reason;verified=WosEngineProbe::Verify(reason);}
 if(!verified){status="engine camera executable verification failed";return;}
 for(const auto&s:WosEngineProbe::signatures){unsigned char b[32];if(!WosEngineProbe::Read(WosEngineProbe::imageBase+s.rva,b,32)||memcmp(b,s.bytes,32)){verified=false;status="engine camera code signature changed";return;}}
 uint32_t bounds[3]={};size_t used=0;
 if(!WosEngineProbe::Read(WosEngineProbe::imageBase+0xd15c64,bounds,sizeof(bounds))||!WosEngineProbe::ArenaBounds(bounds,used)){status="engine camera arena unavailable";return;}
 arenaBegin=bounds[0];arenaEnd=bounds[2];
 if(verified&&WosEngineProbe::imageBase==0x400000)fixedGlobalsBase=WosEngineProbe::imageBase;
 frameReady=true;++serial;status="engine camera waiting for HDR geometry";
}
static void Observe(IDirect3DDevice9*d,UINT primitives){
 // Roof/ground tiles are legitimate two-triangle draws and can be first in a scene.
 // The HDR/INTZ/depth-write and perspective checks below reject screen passes.
 if(!frameReady||primitives==0||WosProbe::suppress)return;
 uint32_t scene=0;if(!ReadFrameCurrentScene(scene)||scene==lastScene)return;
 if(scene<arenaBegin||uint64_t(scene)+0x4e0>arenaEnd)return;
 auto g=ReadFrameGlobals();
 if(g.valid!=7||g.current!=scene)return;
 if(g.shadowActive||scene==g.nearScene||scene==g.farScene){lastScene=scene;return;}
 DWORD zEnabled=0,zWrite=0;
 if(FAILED(d->GetRenderState(D3DRS_ZENABLE,&zEnabled))||FAILED(d->GetRenderState(D3DRS_ZWRITEENABLE,&zWrite))||!zEnabled||!zWrite)return;
 IDirect3DSurface9*rt=nullptr,*ds=nullptr;IDirect3DTexture9*texture=nullptr;
 D3DSURFACE_DESC color={},depth={};D3DVIEWPORT9 viewport={};Matrix4 vp={};
 bool ok=SUCCEEDED(d->GetRenderTarget(0,&rt))&&rt&&SUCCEEDED(rt->GetDesc(&color))&&color.Format==D3DFMT_A16B16G16R16F;
 if(ok)ok=SUCCEEDED(d->GetViewport(&viewport))&&viewport.X==0&&viewport.Y==0&&viewport.Width==color.Width&&viewport.Height==color.Height&&color.Width>=640;
 if(ok)ok=SUCCEEDED(d->GetDepthStencilSurface(&ds))&&ds&&SUCCEEDED(ds->GetDesc(&depth))&&depth.Format==D3DFORMAT(MAKEFOURCC('I','N','T','Z'))&&depth.Width==color.Width&&depth.Height==color.Height;
 if(ok)ok=SUCCEEDED(ds->GetContainer(IID_IDirect3DTexture9,reinterpret_cast<void**>(&texture)))&&WosEngineProbe::Read(uintptr_t(scene)+0x190,&vp,sizeof(vp));
 if(ok){ok=Record(texture,scene,vp,color.Width,color.Height);lastScene=scene;if(!ok)++rejected;else status="engine camera matched to HDR depth";}
 if(texture)texture->Release();if(ds)ds->Release();if(rt)rt->Release();
}
}

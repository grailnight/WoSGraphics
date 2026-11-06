#pragma once
// Staged shared proof for a trusted D3D9 call facade and its resource owner.
// All inputs must be typed objects returned by successful D3D calls. This is
// render-thread-only; it never unwraps, casts, or calls inferred engine pointers.
#include <d3d9.h>
#include <cstdint>
namespace WosRsmDeviceOwnership {
enum class Stage {
 Inactive,Ready,FacadeIdentity,FacadeTarget,TargetOwner,OwnerIdentity,
 OwnerTarget,TargetMismatch,FacadeDepth,OwnerDepth,DepthMismatch,
 FacadeChanged,OwnerChanged,BindingChanged,ResourceOwner,ResourceOwnerMismatch
};
struct Evidence {
 Stage stage=Stage::Inactive;HRESULT hr=S_OK;
 uint64_t facadeRaw=0,facadeIdentity=0,ownerRaw=0,ownerIdentity=0;
 uint64_t facadeTarget=0,ownerTarget=0,facadeDepth=0,ownerDepth=0,resourceOwner=0;
};
struct D3D9Ops {
 using Device=IDirect3DDevice9;using Surface=IDirect3DSurface9;using Identity=IUnknown;
 template<class T>static void Retain(T*p){p->AddRef();}
 template<class T>static void Release(T*p){p->Release();}
 template<class T>static HRESULT Canonical(T*p,Identity**out){return p->QueryInterface(IID_IUnknown,reinterpret_cast<void**>(out));}
 static HRESULT Target(Device*p,Surface**out){return p->GetRenderTarget(0,out);}
 static HRESULT Depth(Device*p,Surface**out){return p->GetDepthStencilSurface(out);}
 template<class T>static HRESULT Owner(T*p,Device**out){return p->GetDevice(out);}
};
template<class Ops>class BasicProof {
public:
 using Device=typename Ops::Device;using Surface=typename Ops::Surface;using Identity=typename Ops::Identity;
private:
 template<class T>struct Ref {
  T*p=nullptr;Ref()=default;Ref(const Ref&)=delete;Ref&operator=(const Ref&)=delete;
  ~Ref(){if(p)Ops::Release(p);}T*Take(){T*q=p;p=nullptr;return q;}
 };
 Device*facade_=nullptr,*owner_=nullptr;Identity*facadeIdentity_=nullptr,*ownerIdentity_=nullptr;
 bool valid_=false;
 template<class T>static uint64_t Key(T*p){return uint64_t(reinterpret_cast<uintptr_t>(p));}
 void Drop(){valid_=false;if(ownerIdentity_)Ops::Release(ownerIdentity_);if(facadeIdentity_)Ops::Release(facadeIdentity_);if(owner_)Ops::Release(owner_);if(facade_)Ops::Release(facade_);ownerIdentity_=facadeIdentity_=nullptr;owner_=facade_=nullptr;}
 bool Fail(Stage stage,HRESULT hr=E_FAIL){evidence.stage=stage;evidence.hr=hr;Drop();return false;}
 bool SurfaceKey(Device*d,bool depth,Ref<Surface>&surface,Ref<Identity>&identity,uint64_t&key,Stage stage){
  const HRESULT hr=depth?Ops::Depth(d,&surface.p):Ops::Target(d,&surface.p);
  if(depth&&hr==D3DERR_NOTFOUND&&!surface.p){key=0;return true;}
  if(FAILED(hr)||!surface.p)return Fail(stage,FAILED(hr)?hr:E_UNEXPECTED);
  const HRESULT qi=Ops::Canonical(surface.p,&identity.p);
  if(FAILED(qi)||!identity.p)return Fail(stage,FAILED(qi)?qi:E_UNEXPECTED);
  key=Key(identity.p);return true;
 }
 // Pin candidate owner only through a facade-returned RT0. Comparing both
 // devices' actual bindings proves the relation without a proxy-specific GUID.
 bool Capture(Device*facade,bool establishing){
  Ref<Device> face,owner;Ref<Identity> faceId,ownerId;
  Ref<Surface> faceRT,nativeRT,faceDS,nativeDS;Ref<Identity> faceRTId,nativeRTId,faceDSId,nativeDSId;
  Ops::Retain(facade);face.p=facade;evidence={};evidence.facadeRaw=Key(facade);
  HRESULT hr=Ops::Canonical(facade,&faceId.p);
  if(FAILED(hr)||!faceId.p)return Fail(Stage::FacadeIdentity,FAILED(hr)?hr:E_UNEXPECTED);
  evidence.facadeIdentity=Key(faceId.p);
  if(!establishing&&faceId.p!=facadeIdentity_)return Fail(Stage::FacadeChanged);
  if(!SurfaceKey(facade,false,faceRT,faceRTId,evidence.facadeTarget,Stage::FacadeTarget))return false;
  hr=Ops::Owner(faceRT.p,&owner.p);if(FAILED(hr)||!owner.p)return Fail(Stage::TargetOwner,FAILED(hr)?hr:E_UNEXPECTED);
  evidence.ownerRaw=Key(owner.p);hr=Ops::Canonical(owner.p,&ownerId.p);
  if(FAILED(hr)||!ownerId.p)return Fail(Stage::OwnerIdentity,FAILED(hr)?hr:E_UNEXPECTED);
  evidence.ownerIdentity=Key(ownerId.p);
  if(!establishing&&ownerId.p!=ownerIdentity_)return Fail(Stage::OwnerChanged);
  if(!SurfaceKey(owner.p,false,nativeRT,nativeRTId,evidence.ownerTarget,Stage::OwnerTarget))return false;
  if(faceRTId.p!=nativeRTId.p)return Fail(Stage::TargetMismatch);
  if(!SurfaceKey(facade,true,faceDS,faceDSId,evidence.facadeDepth,Stage::FacadeDepth)||
     !SurfaceKey(owner.p,true,nativeDS,nativeDSId,evidence.ownerDepth,Stage::OwnerDepth))return false;
  if(faceDSId.p!=nativeDSId.p)return Fail(Stage::DepthMismatch);
  // Recheck the same relation after all queries. The adapter must bracket
  // native calls on one render thread; a moving binding is never cached.
  for(unsigned i=0;i<4;++i){Ref<Surface>s;Ref<Identity>id;uint64_t key=0;const bool depth=i>=2;Device*d=(i&1)?owner.p:facade;
   if(!SurfaceKey(d,depth,s,id,key,Stage::BindingChanged))return false;
   const uint64_t expected=depth?evidence.facadeDepth:evidence.facadeTarget;
   if(key!=expected)return Fail(Stage::BindingChanged);
  }
  if(establishing){Drop();facade_=face.Take();owner_=owner.Take();facadeIdentity_=faceId.Take();ownerIdentity_=ownerId.Take();}
  valid_=true;evidence.stage=Stage::Ready;evidence.hr=S_OK;return true;
 }
public:
 Evidence evidence{};
 BasicProof()=default;BasicProof(const BasicProof&)=delete;BasicProof&operator=(const BasicProof&)=delete;
 ~BasicProof(){Drop();}
 void Reset(){Drop();evidence={};}
 bool Establish(Device*trustedFacade){Ref<Device>keep;if(trustedFacade){Ops::Retain(trustedFacade);keep.p=trustedFacade;}Reset();if(!trustedFacade)return false;return Capture(keep.p,true);}
 bool ValidateBindings(){if(!valid_)return false;return Capture(facade_,false);}
 bool Valid()const{return valid_;}
 Device*Facade()const{return valid_?facade_:nullptr;}Device*ResourceOwner()const{return valid_?owner_:nullptr;}
 uint64_t FacadeKey()const{return valid_?Key(facadeIdentity_):0;}uint64_t ResourceOwnerKey()const{return valid_?Key(ownerIdentity_):0;}
 bool MatchesFacade(Device*trusted){
  if(!valid_||!trusted)return false;Ref<Identity> id;const HRESULT hr=Ops::Canonical(trusted,&id.p);
  return SUCCEEDED(hr)&&id.p&&id.p==facadeIdentity_;
 }
 // Call only for trusted registration / Get outputs within a render-thread
 // frame whose facade/native relation was validated. Ordinary binding changes
 // do not change pinned ownership. Revalidate at frame/reset boundaries; no
 // full binding queries are needed for every resource. Failure rejects the
 // object; it does not replace the owner.
 template<class T>bool MatchesResourceOwner(T*trusted){
  if(!valid_||!trusted)return false;Ref<Device> owner;Ref<Identity> id;
  HRESULT hr=Ops::Owner(trusted,&owner.p);
  if(FAILED(hr)||!owner.p){evidence.stage=Stage::ResourceOwner;evidence.hr=FAILED(hr)?hr:E_UNEXPECTED;return false;}
  hr=Ops::Canonical(owner.p,&id.p);evidence.resourceOwner=Key(id.p);
  if(FAILED(hr)||!id.p){evidence.stage=Stage::ResourceOwner;evidence.hr=FAILED(hr)?hr:E_UNEXPECTED;return false;}
  const bool same=id.p==ownerIdentity_;evidence.stage=same?Stage::Ready:Stage::ResourceOwnerMismatch;evidence.hr=same?S_OK:E_FAIL;return same;
 }
};
using Proof=BasicProof<D3D9Ops>;
}

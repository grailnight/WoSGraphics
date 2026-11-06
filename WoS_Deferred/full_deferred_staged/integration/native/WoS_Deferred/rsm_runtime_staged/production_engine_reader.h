#pragma once
// Include after engine_probe.h. Does not read or modify its F7 enabled flag,
// allocate F7 IDs, open logs or use its Begin/Finish paths.
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_Deferred/rsm_runtime_staged/observation_bridge.h"
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/indirect_fixed_globals.h"
namespace WosRsmRuntime {
struct ProductionEngineReader {
 bool VerifyImage(){std::string reason;return WosEngineProbe::Verify(reason);}
 uint32_t ImageAddress()const{return uint32_t(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)));}
 bool VerifyLoaded(){
  if(ImageAddress()!=ImageBase)return false;
  for(const auto&s:WosEngineProbe::signatures){unsigned char b[32]{};if(!Read(ImageBase+s.rva,b,32)||std::memcmp(b,s.bytes,32))return false;}
  unsigned char clear[32]{};return Read(ShadowClearCaller-32,clear,32)&&!std::memcmp(clear,ClearCode,32);
 }
 bool Read(uintptr_t address,void*out,size_t size){return WosEngineProbe::Read(address,out,size);}
 bool ArenaBounds(const uint32_t(&a)[3],size_t&used){return WosEngineProbe::ArenaBounds(a,used);}
 EngineGlobals ReadGlobals(){auto g=WosIndirectFixedGlobals::ReadGlobals(ImageBase,true);return {g.current,g.root,g.nearScene,g.farScene,g.shadowActive,g.valid};}
};
using ProductionBridge=ObservationBridge<ProductionEngineReader>;
}

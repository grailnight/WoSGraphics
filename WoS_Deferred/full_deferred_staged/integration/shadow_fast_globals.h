#pragma once
#include "native/WoS_VolumetricClouds/live_menu/indirect_fixed_globals.h"
#include <cstdio>
namespace WosShadowFastGlobals {
static bool enabled=true;
static unsigned long long reads=0,fastReads=0,slowReads=0,checks=0,mismatches=0;
using Snapshot=WosIndirectFixedGlobals::Snapshot;
static bool Same(const Snapshot&a,const Snapshot&b){
 return a.current==b.current&&a.root==b.root&&a.nearScene==b.nearScene&&a.farScene==b.farScene&&a.shadowActive==b.shadowActive&&a.valid==b.valid;
}
template<class F> static Snapshot Read(uintptr_t base,bool frameVerified,F fallback){
 auto slow=[&](){++slowReads;auto g=fallback();return Snapshot{g.current,g.root,g.nearScene,g.farScene,g.shadowActive,g.valid};};
 ++reads;
 if(!enabled||!frameVerified)return slow();
 auto g=WosIndirectFixedGlobals::ReadGlobals(base,true);++fastReads;
 // Never use partial direct results. Keep the original guarded read as fallback.
 if(g.valid!=7)return slow();
 // Fresh volatile values, never a cached current scene. Verify periodically
 // against the original reader, failing closed on any discrepancy.
 if((fastReads&255)==1){++checks;auto reference=slow();if(!Same(g,reference)){++mismatches;enabled=false;return reference;}}
 return g;
}
static void WriteJson(FILE*f){
 std::fprintf(f,"\"shadow_globals_cache\":{\"enabled\":%s,\"reads\":%llu,\"fast_reads\":%llu,\"slow_reads\":%llu,\"checks\":%llu,\"mismatches\":%llu},",enabled?"true":"false",reads,fastReads,slowReads,checks,mismatches);
}
}

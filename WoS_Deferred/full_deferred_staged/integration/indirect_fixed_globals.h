#pragma once
#include <windows.h>
#include <cstdint>

// Only the three fixed, verified EXE globals used by the camera hot path.
// This deliberately provides no arbitrary-pointer/length read interface.
// The caller must pass a verification gate established for this render frame.
namespace WosIndirectFixedGlobals {
struct Snapshot { uint32_t current=0,root=0,nearScene=0,farScene=0,shadowActive=0,valid=0; };
static constexpr uintptr_t currentRva=0xd15c54,shadowPairRva=0xc36e98,activeRva=0xc73dfa;
static bool Eligible(uintptr_t base,bool verified) {
    return verified && sizeof(void*)==4 && base>=0x10000 &&
           uint64_t(base)+uint64_t(currentRva)+8<=0x100000000ull;
}
class Reader {
    static int ReadException(DWORD code) {
    return code==EXCEPTION_ACCESS_VIOLATION || code==EXCEPTION_IN_PAGE_ERROR ||
           code==EXCEPTION_GUARD_PAGE ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
// Leaf POD routines keep SEH outside functions that own C++ destructors. Volatile
// loads are fresh on each call; no current-scene value is cached across draws.
__declspec(noinline) static bool Pair(uintptr_t address,uint32_t&first,uint32_t&second) {
    __try {
        auto source=reinterpret_cast<volatile const uint32_t*>(address);
        first=source[0];second=source[1];return true;
    } __except(ReadException(GetExceptionCode())) { first=second=0;return false; }
}
__declspec(noinline) static bool Current(uintptr_t address,uint32_t&out) {
    __try {out=*reinterpret_cast<volatile const uint32_t*>(address);return true;}
    __except(ReadException(GetExceptionCode())) {out=0;return false;}
}
__declspec(noinline) static bool Active(uintptr_t address,uint32_t&out) {
    __try {out=*reinterpret_cast<volatile const unsigned char*>(address);return true;}
    __except(ReadException(GetExceptionCode())) {out=0;return false;}
}
public:
static Snapshot ReadGlobals(uintptr_t base,bool verified) {
    Snapshot out;
    if(!Eligible(base,verified))return out;
    if(Pair(base+currentRva,out.current,out.root))out.valid|=1;
    if(Pair(base+shadowPairRva,out.nearScene,out.farScene))out.valid|=2;
    if(Active(base+activeRva,out.shadowActive))out.valid|=4;
    return out;
}
static bool ReadCurrentScene(uintptr_t base,bool verified,uint32_t&scene) {
    scene=0;
    return Eligible(base,verified) && Current(base+currentRva,scene);
}
 };
static Snapshot ReadGlobals(uintptr_t base,bool verified) {return Reader::ReadGlobals(base,verified);}
static bool ReadCurrentScene(uintptr_t base,bool verified,uint32_t&scene) {return Reader::ReadCurrentScene(base,verified,scene);}
}

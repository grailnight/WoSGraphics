#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include "engine_probe.h"
#include "native_material_context_signatures.h"
#include "native_material_context_compat_signatures.h"

// This is a read-only compatibility gate, never a hook installer. Only this
// reviewed module/route may replace the original binder's first nine bytes.
// Source metadata still does not prove actual texture bindings: the known mod
// can override pixel samplers 1/8 and VS constants/render state after binding.
namespace WosNativeMaterialCompat {
enum class Mode { Rejected, Original, ExWoS };
struct Result {
    bool accepted=false,moduleHashVerified=false;
    Mode mode=Mode::Rejected;
    uint32_t failedVA=0,moduleBase=0,trampoline=0;
    const char* reason="not verified";
};
static const char* ModeName(Mode mode) {
    return mode==Mode::Original?"original_exact":mode==Mode::ExWoS?"ex_wos_exact_detour":"rejected";
}
static uint32_t Word(const void* p) {uint32_t n;std::memcpy(&n,p,4);return n;}
static void PutWord(void* p,uint32_t n) {std::memcpy(p,&n,4);}
static bool Fail(Result&out,const char*reason,uint32_t va) {
    out.accepted=false;out.mode=Mode::Rejected;out.reason=reason;out.failedVA=va;return false;
}
static const WosNativeMaterialContext::CodeRange* OriginalBinder() {
    for(const auto&r:WosNativeMaterialContext::contextCodeRanges)if(r.va==binderVA)return &r;
    return nullptr;
}
static uint32_t RelativeTarget(uint32_t instruction,const unsigned char* bytes) {
    // x86 near rel32 branches use 32-bit modular address arithmetic.
    return instruction+5u+Word(bytes+1);
}
template<class Reader> static bool Exact(Reader&read,uint32_t va,const unsigned char*expected,unsigned size) {
    unsigned char bytes[1536]={};
    return size<=sizeof(bytes)&&uint64_t(va)+size<=0x100000000ull&&read(va,bytes,size)&&!std::memcmp(bytes,expected,size);
}

// Pure read-only route proof for CPU fixtures. It validates loaded PE identity
// and code, not the on-disk file hash; production must use VerifyForCapture.
template<class Reader> static bool VerifyKnownCodeRoute(Reader&read,uint32_t moduleBase,Result&out) {
    out=Result{};out.moduleBase=moduleBase;
    if(moduleBase<0x10000 || uint64_t(moduleBase)+expectedModuleSize>0x100000000ull)
        return Fail(out,"invalid module address",moduleBase);
    // The saved loaded PE records its actual base in OptionalHeader.ImageBase.
    // Expect that exact DWORD; every other header byte remains pinned to disk.
    static_assert(moduleImageBaseOffset+4<=sizeof(expectedModuleHeaders),"ImageBase header bounds");
    unsigned char loadedHeaders[sizeof(expectedModuleHeaders)]={};
    std::memcpy(loadedHeaders,expectedModuleHeaders,sizeof(loadedHeaders));
    PutWord(loadedHeaders+moduleImageBaseOffset,moduleBase);
    if(!Exact(read,moduleBase,loadedHeaders,sizeof(loadedHeaders)))
        return Fail(out,"loaded PE identity mismatch",moduleBase);
    const auto*original=OriginalBinder();
    if(!original||original->size!=121)return Fail(out,"original binder signature unavailable",binderVA);
    // Do not omit other ranges or the node-vtable range when the strict gate
    // stopped at this binder. Its unpatched 112-byte tail remains exact too.
    for(const auto&r:WosNativeMaterialContext::contextCodeRanges) {
        const unsigned skip=r.va==binderVA?9:0;
        if(r.size<skip || !Exact(read,r.va+skip,r.bytes+skip,r.size-skip))
            return Fail(out,"original route or binder tail mismatch",r.va+skip);
    }
    unsigned char entry[9]={};
    if(!read(binderVA,entry,sizeof(entry))||entry[0]!=0xe9||entry[5]!=0xcc||entry[6]!=0xcc||entry[7]!=0xcc||entry[8]!=0xcc||
       RelativeTarget(binderVA,entry)!=moduleBase+wrapperRva)
        return Fail(out,"unrecognized binder entry detour",binderVA);
    for(const auto&r:moduleCodeRanges) {
        unsigned char expected[1536]={};
        if(r.size>sizeof(expected)||uint64_t(r.rva)+r.size>expectedModuleSize)
            return Fail(out,"invalid compiled compatibility range",moduleBase+r.rva);
        std::memcpy(expected,r.bytes,r.size);
        for(unsigned i=0;i<r.relocationCount;++i) {
            const unsigned offset=r.relocations[i];
            if(offset+4>r.size)return Fail(out,"invalid compiled relocation",moduleBase+r.rva);
            PutWord(expected+offset,Word(expected+offset)+(moduleBase-preferredModuleBase));
        }
        if(!Exact(read,moduleBase+r.rva,expected,r.size))
            return Fail(out,"known module code mismatch",moduleBase+r.rva);
    }
    uint32_t trampoline=0;
    if(!read(moduleBase+trampolinePointerRva,&trampoline,4)||trampoline<0x10000||uint64_t(trampoline)+14>0x100000000ull)
        return Fail(out,"invalid original trampoline pointer",moduleBase+trampolinePointerRva);
    out.trampoline=trampoline;unsigned char displaced[14]={};
    if(!read(trampoline,displaced,sizeof(displaced))||std::memcmp(displaced,original->bytes,9)||displaced[9]!=0xe9||
       RelativeTarget(trampoline+9,displaced+9)!=binderVA+9)
        return Fail(out,"original trampoline bytes or return mismatch",trampoline);
    // A hook change during validation must not produce a mixed route proof.
    unsigned char entryAgain[9]={};uint32_t pointerAgain=0;
    if(!read(binderVA,entryAgain,sizeof(entryAgain))||std::memcmp(entry,entryAgain,sizeof(entry))||
       !read(moduleBase+trampolinePointerRva,&pointerAgain,4)||pointerAgain!=trampoline)
        return Fail(out,"route changed during verification",binderVA);
    out.accepted=true;out.mode=Mode::ExWoS;out.reason="exact original trampoline and reviewed post-bind overrides";return true;
}

// Invoke once at F7 Begin. This does not load the mod or call any of its code.
// GetModuleHandle returns only an already loaded module; the file is SHA-pinned.
template<class Reader> static bool VerifyForCapture(Reader&read,Result&out) {
    out=Result{};uint32_t failed=0;
    if(WosNativeMaterialContext::VerifyCodeRanges(read,failed)) {
        out.accepted=true;out.mode=Mode::Original;out.reason="exact original routes";return true;
    }
    if(failed!=binderVA)return Fail(out,"original route mismatch",failed);
    HMODULE module=GetModuleHandleW(L"ex_WoS.dll");
    const uintptr_t base=reinterpret_cast<uintptr_t>(module);
    if(!module||sizeof(void*)!=4||base>0xffffffffu)return Fail(out,"known compatibility module unavailable",binderVA);
    wchar_t path[32768]={};const DWORD n=GetModuleFileNameW(module,path,32768);std::string sha;
    if(!n||n>=32768||!WosEngineProbe::FileSha(path,sha)||sha!=expectedModuleSha)
        return Fail(out,"compatibility module SHA256 mismatch",static_cast<uint32_t>(base));
    const bool ok=VerifyKnownCodeRoute(read,static_cast<uint32_t>(base),out);
    out.moduleHashVerified=true;return ok;
}
}

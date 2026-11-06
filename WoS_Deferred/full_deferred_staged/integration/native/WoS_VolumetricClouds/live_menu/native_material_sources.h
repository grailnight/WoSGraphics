#pragma once
// Included inside WosNativeMaterialContext, after descriptor/base/override helpers.
// CPU reads only. Texture-like source words are followed only after a kind-3 PS binding proof.
struct SourceEffectInfo {
    uint32_t effect=0,pointer=0,count=0,valueBase=0;
    unsigned snapshot=0,firstDraw=0;bool valid=false;
    unsigned char bytes[128*0x2c]={};
};
struct SourceBindingInfo {
    uint32_t state=0,pair=0,descriptor=0,shader=0,pointer=0,count=0;
    unsigned descriptorSnapshot=0,recordsSnapshot=0,firstDraw=0;bool valid=false;
    unsigned char bytes[128*24]={};
};
struct SourceSamplerProof {
    uint32_t shader=0,descriptor=0,stage=0,recordIndex=0,parameterIndex=0,group=0,offset=0;
    uint32_t samplerStateParameterIndex=0,samplerStateParameterAddress=0,samplerStateValuePointer=0;
    unsigned recordsSnapshot=0;
};
struct SourceParameterInfo {
    uint32_t index=0,address=0,hash=0,type=0,ownSize=0,value=0,linked=0,linkedSize=0;
    uint32_t effectIndex=0xffffffffu,effectAddress=0,ngl=0,d3d=0;
    unsigned snapshot=0,valueSnapshot=0,linkedSnapshot=0,capturedBytes=0,nglSnapshot=0,proofCount=0;
    bool samplerParameterCompatible=false;
    SourceSamplerProof proofs[4]={};
};
struct SourceGroupInfo {
    uint32_t descriptor=0,pointer=0,count=0,effect=0,effectTable=0,effectCount=0,valueBase=0;
    unsigned firstDraw=0,descriptorSnapshot=0,captured=0,effectSnapshot=0,effectHeaderSnapshot=0,passIndex=0,passSnapshot=0,bindingCount=0;
    bool passResolved=false,overrideEvidenceValid=false,overridePresent=false,variantPresent=false;
    const char* status="not_attempted";const char* passReason="unresolved";
    SourceBindingInfo bindingRefs[4]={};SourceParameterInfo entries[ContextSourceGroupEntryLimit]={};
};
static std::unordered_map<uint32_t,SourceEffectInfo> sourceEffects;
static std::unordered_map<uint32_t,SourceBindingInfo> sourceBindings;
static std::unordered_map<uint64_t,SourceGroupInfo> sourceGroups;
static std::unordered_map<uint64_t,unsigned> sourceEffectGroups;
static unsigned sourceEntriesByPhase[2]={},sourceGroupsByPhase[2]={};
static void ClearMaterialSources() {
    sourceEffects.clear();sourceBindings.clear();sourceGroups.clear();sourceEffectGroups.clear();
    sourceEntriesByPhase[0]=sourceEntriesByPhase[1]=sourceGroupsByPhase[0]=sourceGroupsByPhase[1]=0;
}
static SourceEffectInfo ReadSourceEffect(const BaseMaterialInfo &base) {
    const auto old=sourceEffects.find(base.pointer);
    if(old!=sourceEffects.end() && old->second.pointer==base.parameters && old->second.count==base.parameterCount && old->second.valueBase==base.valueBase)return old->second;
    SourceEffectInfo e;e.effect=base.pointer;e.pointer=base.parameters;e.count=base.parameterCount;
    e.valueBase=base.valueBase;e.firstDraw=WosProbe::draws;
    if(sourceEffects.size()>=64 || !base.snapshot || !e.count || e.count>128 || !Pointer(e.pointer) ||
        uint64_t(e.pointer)+uint64_t(e.count)*0x2c>0x100000000ull)return e;
    e.snapshot=SnapshotRead("effect_parameter_definitions",e.pointer,e.count*0x2c,e.bytes);
    e.valid=e.snapshot!=0;if(e.valid)sourceEffects[base.pointer]=e;return e;
}
template<class Reader> static bool ResolveSourcePass(Reader &read,const DescriptorInfo &d,const BaseMaterialInfo &b,unsigned &index,const char* &reason) {
    index=0;reason="unresolved";if(!b.passBoundsValid)return false;
    if(!d.passTag){reason="default_zero";return true;}
    for(unsigned i=0;i<b.count;++i)if(b.passHashes[i]==d.passTag){index=i;reason="pass_hash";return true;}
    // 009de080 accepts a numeric name (atoi) when the pass hash is absent.
    if(!d.passName){reason="null_name_hex_fallback_zero";return true;}
    if(d.passName<0x10000)return false;
    uint32_t value=0;bool any=false;
    for(unsigned i=0;i<10;++i) {
        unsigned char ch=0;if(uint64_t(d.passName)+i>0xffffffffull || !read(d.passName+i,&ch,1))return false;
        if(ch<'0' || ch>'9') {
            if(!any)return false;index=value<b.count?value:b.count-1;reason="bounded_numeric_name";return true;
        }
        any=true;if(value>1000)return false;value=value*10+(ch-'0');
    }
    return false; // Never consume an unbounded string or guess a missing terminator.
}
static SourceBindingInfo ReadSourceBinding(uint32_t state) {
    SourceBindingInfo b;b.state=state;LocalReader read;
    if(!Pointer(state) || uint64_t(state)+0xe0>0x100000000ull || !read(state+0xdc,&b.pair,4) ||
        !Pointer(b.pair) || uint64_t(b.pair)+0x30>0x100000000ull)return b;
    b.descriptor=b.pair+0x20;
    const auto old=sourceBindings.find(b.descriptor);
    if(old!=sourceBindings.end()){b=old->second;b.state=state;return b;}
    if(sourceBindings.size()>=64)return b;
    unsigned char descriptor[16]={};b.firstDraw=WosProbe::draws;
    b.descriptorSnapshot=SnapshotRead("pixel_shader_descriptor",b.descriptor,sizeof(descriptor),descriptor);
    if(!b.descriptorSnapshot)return b;
    b.shader=Word(descriptor,4);b.count=Word(descriptor,8);b.pointer=Word(descriptor,12);
    if(!Pointer(b.shader) || !b.count || b.count>128 || !Pointer(b.pointer) ||
        uint64_t(b.pointer)+uint64_t(b.count)*24>0x100000000ull)return b;
    b.recordsSnapshot=SnapshotRead("pixel_shader_binding_records",b.pointer,b.count*24,b.bytes);
    b.valid=b.recordsSnapshot!=0;if(b.valid)sourceBindings[b.descriptor]=b;return b;
}
static unsigned SourceSamplerProofs(const SourceGroupInfo &g,const SourceEffectInfo &effect,unsigned parameterIndex,
    SourceSamplerProof (&proofs)[4]) {
    unsigned count=0;
    if(!effect.valid || !effect.count || effect.count>128 || parameterIndex>=effect.count ||
        g.bindingCount>4 || !Pointer(effect.pointer) || !Pointer(effect.valueBase) ||
        uint64_t(effect.pointer)+uint64_t(effect.count)*0x2c>0x100000000ull)return 0;
    const unsigned char* texture=effect.bytes+parameterIndex*0x2c;
    const uint32_t effectValue=Word(texture,0x1c);
    if(Word(texture,0x18)!=11 || Word(texture,0x20)!=28 || !Pointer(effectValue) ||
        uint64_t(effectValue)+28>0x100000000ull)return 0;
    // 009ea730 case 3 has two separate FX parameters. record[0] supplies sampler
    // address/filter state; context[group]+offset supplies the NGL texture value.
    // The captured PS69 and PS72 families use texture type 11/28 and state 14/40.
    unsigned valueMatches=0;
    for(unsigned j=0;j<effect.count;++j) {
        const unsigned char* e=effect.bytes+j*0x2c;
        if(Word(e,0x18)==11 && Word(e,0x20)==28 && Word(e,0x1c)==effectValue)++valueMatches;
    }
    if(valueMatches!=1)return 0;
    for(unsigned set=0;set<g.bindingCount && count<4;++set) {
        const auto &b=g.bindingRefs[set];if(!b.valid || b.count>128)continue;
        for(unsigned i=0;i<b.count && count<4;++i) {
            const unsigned char* r=b.bytes+i*24;
            const uint32_t stateIndex=Word(r),stage=Word(r,4),kind=Word(r,8),group=Word(r,12),offset=Word(r,16);
            if(stateIndex>=effect.count || kind!=3 || stage>=16 || group!=1 || Word(r,20)!=1 ||
                uint64_t(effect.valueBase)+offset>0xffffffffull || effect.valueBase+offset!=effectValue)continue;
            const unsigned char* state=effect.bytes+stateIndex*0x2c;
            const uint32_t stateValue=Word(state,0x1c);
            if(Word(state,0x18)!=14 || Word(state,0x20)!=40 || !Pointer(stateValue) ||
                uint64_t(stateValue)+40>0x100000000ull)continue;
            auto &p=proofs[count++];p.shader=b.shader;p.descriptor=b.descriptor;p.stage=stage;p.recordIndex=i;
            p.parameterIndex=parameterIndex;p.group=group;p.offset=offset;p.recordsSnapshot=b.recordsSnapshot;
            p.samplerStateParameterIndex=stateIndex;
            p.samplerStateParameterAddress=effect.pointer+stateIndex*0x2c;
            p.samplerStateValuePointer=stateValue;
        }
    }return count;
}
static SourceGroupInfo ReadMaterialSources(const DescriptorInfo &d,const BaseMaterialInfo &base,bool shadow,
    const OverrideSlot &materialOverride,const OverrideSlot &variantOverride) {
    const unsigned phase=shadow?1u:0u;const uint64_t key=uint64_t(d.address)|(uint64_t(phase)<<32);
    const auto old=sourceGroups.find(key);if(old!=sourceGroups.end())return old->second;
    SourceGroupInfo g;g.descriptor=d.address;g.pointer=d.parameters;g.count=d.parameterCount;g.firstDraw=WosProbe::draws;
    g.descriptorSnapshot=d.snapshot;g.effectHeaderSnapshot=base.snapshot;g.effect=base.pointer;g.valueBase=base.valueBase;
    g.overrideEvidenceValid=materialOverride.flagsRead && (!materialOverride.present || materialOverride.valueRead) &&
        variantOverride.flagsRead && (!variantOverride.present || variantOverride.valueRead);
    g.overridePresent=materialOverride.present && materialOverride.valueRead && materialOverride.value!=0;
    g.variantPresent=variantOverride.present;
    const uint64_t effectKey=uint64_t(base.pointer)|(uint64_t(phase)<<32);
    if(sourceGroupsByPhase[phase]>=ContextSourceGroupLimit){g.status="group_limit";return g;}
    if(sourceEffectGroups[effectKey]>=ContextSourceEffectDescriptorLimit){g.status="effect_descriptor_limit";return g;}
    if(sourceEntriesByPhase[phase]>=ContextSourceEntryLimit){g.status="phase_entry_budget";return g;}
    ++sourceGroupsByPhase[phase];++sourceEffectGroups[effectKey];
    if(d.method!=0x009dfe20){g.status="unsupported_submitter";sourceGroups[key]=g;return g;}
    const auto effect=ReadSourceEffect(base);
    g.effectSnapshot=effect.snapshot;g.effectTable=effect.pointer;g.effectCount=effect.count;
    LocalReader read;g.passResolved=ResolveSourcePass(read,d,base,g.passIndex,g.passReason);
    if(g.passResolved) {
        g.passSnapshot=base.passSnapshots[g.passIndex];
        const uint32_t states=base.states[g.passIndex],count=base.subpassCounts[g.passIndex];
        if(Pointer(states) && count>0 && count<=4 && uint64_t(states)+uint64_t(count)*0xec<=0x100000000ull) {
            for(unsigned i=0;i<count;++i)g.bindingRefs[g.bindingCount++]=ReadSourceBinding(states+i*0xec);
        }
    }
    if(!g.count){g.status="empty";sourceGroups[key]=g;return g;}
    if(g.count>ContextSourceDescriptorLimit || !Pointer(g.pointer) || uint64_t(g.pointer)+uint64_t(g.count)*0x2c>0x100000000ull) {
        g.status="invalid_count_or_pointer";sourceGroups[key]=g;return g;
    }
    // Read bounded headers for relevance ordering. Save sampler entries first, then other parameters.
    unsigned char headers[ContextSourceDescriptorLimit*0x2c]={};
    if(!read(g.pointer,headers,g.count*0x2c)){g.status="source_table_unreadable";sourceGroups[key]=g;return g;}
    unsigned indices[ContextSourceDescriptorLimit]={},effectIndices[ContextSourceDescriptorLimit]={},priority[ContextSourceDescriptorLimit]={},n=0;
    for(unsigned i=0;i<g.count;++i) {
        const uint32_t hash=Word(headers,i*0x2c+4);unsigned matched=0xffffffffu,matches=0;
        if(effect.valid && hash)for(unsigned j=0;j<effect.count;++j)if(Word(effect.bytes,j*0x2c+4)==hash){matched=j;++matches;}
        effectIndices[i]=matches==1?matched:0xffffffffu;
        SourceSamplerProof proof[4]={};priority[i]=matches==1 && SourceSamplerProofs(g,effect,matched,proof)>0?1u:0u;
    }
    for(unsigned pass=0;pass<2;++pass)for(unsigned i=0;i<g.count;++i)if(priority[i]==(pass==0?1u:0u))indices[n++]=i;
    g.status=g.count>ContextSourceGroupEntryLimit?"relevant_entries_subset":"captured";
    for(unsigned order=0;order<n && g.captured<ContextSourceGroupEntryLimit;++order) {
        if(sourceEntriesByPhase[phase]>=ContextSourceEntryLimit){g.status="phase_entry_budget";break;}
        const unsigned i=indices[order];++sourceEntriesByPhase[phase];auto &e=g.entries[g.captured++];
        const unsigned char *entry=headers+i*0x2c;e.index=i;e.address=g.pointer+i*0x2c;
        e.snapshot=SaveSnapshot("descriptor_parameter",e.address,entry,0x2c);
        e.hash=Word(entry,4);e.linked=Word(entry,0x14);e.type=Word(entry,0x18);e.value=Word(entry,0x1c);e.ownSize=Word(entry,0x20);
        unsigned char linked[0x2c]={};
        e.linkedSnapshot=SnapshotRead("linked_parameter",e.linked,sizeof(linked),linked);
        if(e.linkedSnapshot)e.linkedSize=Word(linked,0x20);
        unsigned char source[64]={};
        if(e.ownSize>0 && e.ownSize<=65536) {
            e.capturedBytes=e.ownSize<64?e.ownSize:64;
            e.valueSnapshot=SnapshotRead("parameter_source_prefix",e.value,e.capturedBytes,source);
        }
        e.effectIndex=effectIndices[i];
        if(e.effectIndex!=0xffffffffu) {
            e.effectAddress=effect.pointer+e.effectIndex*0x2c;
            const unsigned char* definition=effect.bytes+e.effectIndex*0x2c;
            e.samplerParameterCompatible=e.type==11 && Word(definition,0x18)==11 && e.ownSize==28 && Word(definition,0x20)==28;
            e.proofCount=SourceSamplerProofs(g,effect,e.effectIndex,e.proofs);
        }
        // Separate enum domains: source type is recorded, never equated to shader binding kind.
        if(e.proofCount && e.samplerParameterCompatible && e.valueSnapshot && e.capturedBytes>=28) {
            e.ngl=Word(source);unsigned char texture[24]={};
            e.nglSnapshot=SnapshotRead("typed_ngl_texture",e.ngl,sizeof(texture),texture);
            if(e.nglSnapshot)e.d3d=Word(texture,0x14);
        }
    }
    sourceGroups[key]=g;return g;
}
static void WriteMaterialSources(const SourceGroupInfo &g) {
    std::fprintf(contextLog,"\"descriptor_parameters\":{\"version\":5,\"descriptor\":\"%08x\",\"pointer\":\"%08x\",\"count\":%u,\"first_draw\":%u,\"descriptor_snapshot\":%u,\"status\":\"%s\",\"captured_entries\":%u,\"effect\":\"%08x\",\"effect_table\":\"%08x\",\"effect_count\":%u,\"effect_value_base\":\"%08x\",\"effect_snapshot\":%u,\"effect_header_snapshot\":%u,\"pass_resolved\":%s,\"pass_reason\":\"%s\",\"pass_index\":%u,\"pass_snapshot\":%u,\"override_evidence_valid\":%s,\"material_override_present\":%s,\"variant_override_present\":%s,\"shader_bindings\":[",
        g.descriptor,g.pointer,g.count,g.firstDraw,g.descriptorSnapshot,g.status,g.captured,g.effect,g.effectTable,g.effectCount,g.valueBase,g.effectSnapshot,g.effectHeaderSnapshot,
        g.passResolved?"true":"false",g.passReason,g.passIndex,g.passSnapshot,g.overrideEvidenceValid?"true":"false",g.overridePresent?"true":"false",g.variantPresent?"true":"false");
    for(unsigned i=0;i<g.bindingCount;++i) {
        const auto &b=g.bindingRefs[i];
        std::fprintf(contextLog,"%s{\"state\":\"%08x\",\"pair\":\"%08x\",\"descriptor\":\"%08x\",\"shader\":\"%08x\",\"pointer\":\"%08x\",\"count\":%u,\"descriptor_snapshot\":%u,\"records_snapshot\":%u,\"first_draw\":%u,\"valid\":%s}",
            i?",":"",b.state,b.pair,b.descriptor,b.shader,b.pointer,b.count,b.descriptorSnapshot,b.recordsSnapshot,b.firstDraw,b.valid?"true":"false");
    }
    std::fprintf(contextLog,"],\"entries\":[");
    for(unsigned i=0;i<g.captured;++i) {
        const auto &e=g.entries[i];
        std::fprintf(contextLog,"%s{\"source_index\":%u,\"address\":\"%08x\",\"snapshot\":%u,\"name_hash\":\"%08x\",\"source_type\":%u,\"source_declared_bytes\":%u,\"linked\":\"%08x\",\"linked_snapshot\":%u,\"value_pointer\":\"%08x\",\"declared_bytes\":%u,\"captured_bytes\":%u,\"value_snapshot\":%u,\"value_size_source\":\"source_entry_20\",\"effect_parameter_index\":%u,\"effect_parameter_address\":\"%08x\",\"sampler_parameter_compatible\":%s,\"ngl_pointer\":\"%08x\",\"ngl_snapshot\":%u,\"d3d_pointer\":\"%08x\",\"sampler_proofs\":[",
            i?",":"",e.index,e.address,e.snapshot,e.hash,e.type,e.ownSize,e.linked,e.linkedSnapshot,e.value,e.linkedSize,e.capturedBytes,e.valueSnapshot,
            e.effectIndex,e.effectAddress,e.samplerParameterCompatible?"true":"false",e.ngl,e.nglSnapshot,e.d3d);
        for(unsigned j=0;j<e.proofCount;++j) {
            const auto &p=e.proofs[j];
            std::fprintf(contextLog,"%s{\"proof_version\":2,\"shader\":\"%08x\",\"descriptor\":\"%08x\",\"stage\":%u,\"record_index\":%u,\"parameter_index\":%u,\"sampler_state_parameter_index\":%u,\"sampler_state_parameter_address\":\"%08x\",\"sampler_state_value_pointer\":\"%08x\",\"binding_kind\":3,\"group\":%u,\"offset\":%u,\"records_snapshot\":%u}",
                j?",":"",p.shader,p.descriptor,p.stage,p.recordIndex,p.parameterIndex,p.samplerStateParameterIndex,
                p.samplerStateParameterAddress,p.samplerStateValuePointer,p.group,p.offset,p.recordsSnapshot);
        }
        std::fprintf(contextLog,"]}");
    }
    std::fprintf(contextLog,"]}");
}

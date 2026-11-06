#pragma once
// Included inside WosNativeMaterialContext. F7 diagnostic: exact native input reads,
// no engine calls, COM access, render-state changes or retained engine pointers.
struct CurrentInputRead {
    uint32_t address=0,bytes=0;unsigned snapshot=0;bool ok=false;unsigned char data[16]={};
};
struct CurrentInputCapture {
    uint32_t draw=0,scene=0,instance=0,effect=0,flags=0;unsigned readCount=0;
    const char* status="producer_gate_failed";const char* decodeStatus="not_attempted";
    bool sourceAvailable=false;WosStagedCurrentSun::Inputs inputs;CurrentInputRead reads[32]={};
};
static const char* CurrentInputStatus(WosStagedCurrentSun::Status status) {
    using S=WosStagedCurrentSun::Status;
    switch(status) {
    case S::Accepted:return "accepted";case S::UnsupportedProducer:return "unsupported_producer";
    case S::Context:return "context";case S::PropertyId:return "property_id";case S::Read:return "read";
    case S::Pointer:return "pointer";case S::NonFinite:return "nonfinite";case S::Changed:return "changed";
    }return "invalid_status";
}
template<class Reader> struct CurrentInputReader {
    Reader& read;CurrentInputCapture& capture;
    bool operator()(uint32_t address,void* out,uint32_t bytes) {
        if(capture.readCount>=32 || (bytes!=4 && bytes!=16))return false;
        auto& entry=capture.reads[capture.readCount++];entry.address=address;entry.bytes=bytes;
        entry.ok=read(address,out,bytes);if(entry.ok)std::memcpy(entry.data,out,bytes);return entry.ok;
    }
};
template<class Reader> static CurrentInputCapture ReadCurrentInputs(Reader& read,bool producerGate,
    unsigned draw,uint32_t scene,uint32_t instance,uint32_t effect) {
    CurrentInputCapture c;c.draw=draw;c.scene=scene;c.instance=instance;c.effect=effect;
    if(!producerGate)return c;
    CurrentInputReader<Reader> traced{read,c};
    if(!WosStagedCurrentSun::Bounded(effect,4) || !traced(effect,&c.flags,4)) {
        c.status="flags_read";return c;
    }
    const auto decoded=WosStagedCurrentSun::Decode(traced,scene,instance,c.flags,c.inputs);
    c.status=c.decodeStatus=CurrentInputStatus(decoded);
    // Availability is finalized only after saving all raw reads under the existing F7 budgets.
    return c;
}
static void SaveCurrentInputs(CurrentInputCapture& c) {
    bool saved=true;
    for(unsigned i=0;i<c.readCount;++i) {
        auto& r=c.reads[i];if(r.ok)r.snapshot=SaveSnapshot("current_input_read",r.address,r.data,r.bytes);
        if(!r.ok || !r.snapshot)saved=false;
    }
    if(std::strcmp(c.status,"accepted")==0 && !saved)c.status="snapshot_unavailable";
    c.sourceAvailable=std::strcmp(c.status,"accepted")==0 && c.inputs.sunPresent;
}
static void WriteCurrentFloat4(const char* key,const float* values) {
    std::fprintf(contextLog,"\"%s\":[%.9g,%.9g,%.9g,%.9g]",key,values[0],values[1],values[2],values[3]);
}
static void WriteCurrentInputs(const CurrentInputCapture& c) {
    const auto& r=c.inputs;
    std::fprintf(contextLog,"\"current_inputs\":{\"version\":1,\"draw\":%u,\"scene\":\"%08x\",\"instance\":\"%08x\",\"original_effect\":\"%08x\",\"original_flags\":%u,\"status\":\"%s\",\"decode_status\":\"%s\",\"source_available\":%s,\"reads\":[",
        c.draw,c.scene,c.instance,c.effect,c.flags,c.status,c.decodeStatus,c.sourceAvailable?"true":"false");
    for(unsigned i=0;i<c.readCount;++i) {
        const auto& e=c.reads[i];std::fprintf(contextLog,"%s{\"address\":\"%08x\",\"bytes\":%u,\"snapshot\":%u,\"ok\":%s}",i?",":"",e.address,e.bytes,e.snapshot,e.ok?"true":"false");
    }
    std::fprintf(contextLog,"],\"path\":{\"scene\":\"%08x\",\"instance\":\"%08x\",\"frame\":%u,\"instance_table\":\"%08x\",\"scene_table\":\"%08x\",\"environment_id\":%u,\"scene_environment_id\":%u,\"tint_id\":%u,\"environment\":\"%08x\",\"sun\":\"%08x\",\"tint_pointer\":\"%08x\",\"environment_flags_address\":\"%08x\",\"environment_flags\":%u,\"environment_slot_address\":\"%08x\",\"tint_flags_address\":\"%08x\",\"tint_flags\":%u,\"tint_slot_address\":\"%08x\",\"environment_override\":%s,\"tint_override\":%s,\"sun_present\":%s},",
        r.scene,r.instance,r.frame,r.instanceTable,r.sceneTable,r.environmentId,r.sceneEnvironmentId,r.tintId,r.environment,r.sun,r.tintPointer,
        r.environmentFlagsAddress,r.environmentFlags,r.environmentSlotAddress,r.tintFlagsAddress,r.tintFlags,r.tintSlotAddress,
        r.environmentOverride?"true":"false",r.tintOverride?"true":"false",r.sunPresent?"true":"false");
    // These are finite on Accepted; failed Decode leaves all arrays zero. Raw sun W exists only in raw reads.
    WriteCurrentFloat4("shader_direction",r.shaderDirection);std::fprintf(contextLog,",");
    WriteCurrentFloat4("shader_color",r.shaderColor);std::fprintf(contextLog,",");
    WriteCurrentFloat4("tint",r.tint);std::fprintf(contextLog,"}");
}

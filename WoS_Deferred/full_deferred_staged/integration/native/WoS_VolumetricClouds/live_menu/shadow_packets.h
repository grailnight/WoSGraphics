#pragma once
#include "B:/Games/Spider Man - Web of Shadows/WoS_Deferred/full_deferred_staged/integration/native/WoS_VolumetricClouds/live_menu/shadow_transform.h"
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cstring>
#include <utility>
namespace WosShadowPackets {
using WosShadowTransform::Matrix4;
struct Instance { uint32_t source; Matrix4 world,shaderWVP; };
// Engine resource references are borrowed for the source frame only. These are
// renderer-owned records, NOT ABI-compatible engine nodes or replayable files.
struct Node { uint32_t source,geometry,state,material,pass; size_t instance; };
struct Pass { Matrix4 viewProjection; std::vector<Instance> instances; std::vector<Node> nodes; };
enum class Error { None, Read, UnsupportedNode, Cycle, Limit, InvalidTransform };
// Reader must provide bool(uint32_t address, void* output, size_t bytes).
// Output is committed only after the complete list has passed validation.
template<class Reader> bool Build(Reader read,uint32_t head,const Matrix4 &vp,Pass &out,Error &error) {
    error=Error::None;Pass staging;staging.viewProjection=vp;
    std::unordered_map<uint32_t,size_t> indices;std::unordered_set<uint32_t> visited;
    uint32_t ptr=head;
    while(ptr){
        if(staging.nodes.size()>=100000){error=Error::Limit;return false;}
        if(!visited.insert(ptr).second){error=Error::Cycle;return false;}
        uint32_t n[8]={};if(!read(ptr,n,sizeof(n))){error=Error::Read;return false;}
        if(n[0]!=0x00db7b84){error=Error::UnsupportedNode;return false;}
        size_t index;auto found=indices.find(n[3]);
        if(found==indices.end()){
            Instance inst={};inst.source=n[3];
            if(!read(n[3],&inst.world,sizeof(inst.world))){error=Error::Read;return false;}
            if(!WosShadowTransform::Build(inst.world,vp,inst.shaderWVP)){error=Error::InvalidTransform;return false;}
            index=staging.instances.size();staging.instances.push_back(inst);indices.emplace(n[3],index);
        }else index=found->second;
        staging.nodes.push_back({ptr,n[4],n[5],n[6],n[7],index});ptr=n[1];
    }
    out=std::move(staging);return true;
}
// Reproject an owned pass without touching the source pass, its live engine
// instances, or another cascade. Node/resource identity is retained.
inline bool Reproject(const Pass &source,const Matrix4 &vp,Pass &out) {
    Pass staging=source;staging.viewProjection=vp;
    for(auto &instance:staging.instances)
        if(!WosShadowTransform::Build(instance.world,vp,instance.shaderWVP))return false;
    out=std::move(staging);return true;
}
}

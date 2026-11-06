#pragma once
// After Present, use the same reset path as the existing F5 GI toggle.
namespace WosDeferredGIControl {
static bool SetEnabled(bool requested,std::string&error){
    error.clear();
    if(WosDeferred::pending||WosDeferred::busy||WosDeferred::unsafeDeviceState){error="GI switch requires a safe completed frame";return false;}
    if(WosIndirect::enabled==requested)return true;
    WosIndirect::enabled=requested;WosIndirect::Reset();
    if(requested)WosCascadeProbe::Reset();
    WosIndirect::status=requested?"F5: indirect armed; waiting for current camera and scene depth":"F5: indirect lighting OFF";
    return true;
}
}

#pragma once
// Included INSIDE namespace WosDeferred, after Load. Only the renderer-owned
// material directory is read. The caller runs after a successful Present.
namespace MaterialReloadDetail {
struct Files {
    std::vector<HANDLE> handles;
    ~Files(){for(HANDLE h:handles)CloseHandle(h);}
    bool Read(const std::string&path,std::vector<unsigned char>&bytes,size_t&total,std::string&error){
        HANDLE h=CreateFileA(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(h==INVALID_HANDLE_VALUE){error="material file missing or locked: "+path;return false;}
        try{handles.push_back(h);}catch(...){CloseHandle(h);throw;}
        LARGE_INTEGER size={};
        if(!GetFileSizeEx(h,&size)||size.QuadPart<8||size.QuadPart>1024*1024||size.QuadPart%4){error="invalid material file size: "+path;return false;}
        const size_t length=static_cast<size_t>(size.QuadPart);
        if(total>64*1024*1024-length){error="material reload exceeds 64 MiB limit";return false;}
        bytes.resize(length);DWORD read=0;
        if(!ReadFile(h,bytes.data(),static_cast<DWORD>(length),&read,nullptr)||read!=length){error="material read failed: "+path;return false;}
        total+=length;return true;
    }
};
struct Search {
    HANDLE handle=INVALID_HANDLE_VALUE;
    ~Search(){if(handle!=INVALID_HANDLE_VALUE)FindClose(handle);}
};
struct Pack {
    std::vector<Variant> values;
    ~Pack(){for(auto&v:values){Release(v.shader);Release(v.alphaShader);}}
};
// CreatePixelShader has no byte-count parameter. Reject truncated instructions,
// comments and missing END before handing bounded storage to the driver.
static bool Tokens(const std::vector<unsigned char>&bytes){
    if(bytes.size()<8||bytes.size()%4||bytes.size()>1024*1024)return false;
    const size_t words=bytes.size()/4;
    auto word=[&](size_t i){DWORD value=0;std::memcpy(&value,bytes.data()+4*i,4);return value;};
    if(word(0)!=0xffff0300u)return false;
    for(size_t at=1;at<words;){
        DWORD token=word(at),op=token&0xffffu;
        if(op==0xffffu)return token==0xffffu&&at+1==words;
        if(token&0x80000000u)return false;
        size_t length=op==0xfffeu?(token>>16)&0x7fffu:(token>>24)&15u;
        if(length>words-at-1)return false;
        at+=length+1;
    }
    return false;
}
}
static bool ReloadMaterials(IDirect3DDevice9*d,std::string&error){
    error.clear();
    if(pending){error="material reload rejected: pending batch";return false;}
    if(busy){error="material reload rejected: runtime busy";return false;}
    if(unsafeDeviceState){error="material reload rejected: unsafe device state";return false;}
    if(!d||owner!=d||!resolveShader||!loadAttempted||variants.empty()){
        error="material reload rejected: runtime is not initialized for this device";return false;
    }
    // Setting this only while preparing the transaction prevents reentrant use;
    // no device bindings, user settings, resolve shader, targets or fault change.
    struct BusyScope {BusyScope(){busy=true;}~BusyScope(){busy=false;}} active;
    try{
        MaterialReloadDetail::Files files;MaterialReloadDetail::Pack next;
        next.values.reserve(512);size_t bytesRead=0;
        std::string folder=Directory()+"\\materials\\";WIN32_FIND_DATAA data={};
        MaterialReloadDetail::Search search;
        search.handle=FindFirstFileA((folder+"*.input.cso").c_str(),&data);
        if(search.handle==INVALID_HANDLE_VALUE){error="material reload found no input shaders";return false;}
        for(;;){
            if(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY){error="material input is a directory";return false;}
            std::string name=data.cFileName;
            if(name.size()<=10||next.values.size()>=512){error="material reload exceeds 512 variants or has an invalid name";return false;}
            name.resize(name.size()-10);Variant v;v.rainClass=RainClass(name);std::vector<unsigned char>output;
            if(!files.Read(folder+name+".input.cso",v.input,bytesRead,error)||
               !files.Read(folder+name+".primary.cso",output,bytesRead,error))return false;
            if(!MaterialReloadDetail::Tokens(v.input)||!MaterialReloadDetail::Tokens(output)||
               !WosDeferredLights::Parse(v.input.data(),v.input.size(),v.lights,name.find(".baked_page")!=std::string::npos)){
                error="material reload rejected malformed shader or light binding: "+name;return false;
            }
            for(const auto&old:next.values)if(old.input==v.input){error="material reload has duplicate native input: "+name;return false;}
            const std::string policy=folder+name+".policy.ini";
            DWORD attributes=GetFileAttributesA(policy.c_str());
            if(attributes==INVALID_FILE_ATTRIBUTES){
                DWORD why=GetLastError();if(why!=ERROR_FILE_NOT_FOUND&&why!=ERROR_PATH_NOT_FOUND){error="material policy inaccessible: "+name;return false;}
            }else{
                if(attributes&FILE_ATTRIBUTE_DIRECTORY){error="material policy is a directory: "+name;return false;}
                HANDLE h=CreateFileA(policy.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
                if(h==INVALID_HANDLE_VALUE){error="material policy locked: "+name;return false;}
                try{files.handles.push_back(h);}catch(...){CloseHandle(h);throw;}
                LARGE_INTEGER length={};if(!GetFileSizeEx(h,&length)||length.QuadPart<0||length.QuadPart>4096){error="material policy exceeds 4096 bytes: "+name;return false;}
            }
            char multiple[32]={};GetPrivateProfileStringA("Material","PointCountMultiple","1",multiple,sizeof(multiple),policy.c_str());
            if(std::strcmp(multiple,"1")&&std::strcmp(multiple,"2")){error="material policy PointCountMultiple must be 1 or 2: "+name;return false;}
            v.pointCountMultiple=multiple[0]-'0';
            v.supported=true;next.values.push_back(std::move(v));
            if(FAILED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(output.data()),&next.values.back().shader))||!next.values.back().shader){
                error="material reload CreatePixelShader failed: "+name;return false;
            }
            if(!FindNextFileA(search.handle,&data)){
                if(GetLastError()!=ERROR_NO_MORE_FILES){error="material enumeration failed";return false;}
                break;
            }
        }
        for(const auto&old:variants)if(old.supported){
            bool found=false;for(const auto&v:next.values)if(v.input==old.input){found=true;break;}
            if(!found){error="material reload would remove an existing native material";return false;}
        }
        // No failing operation follows this point. Existing lookup indices and
        // runtime guards belong to the old vector and must be discarded together.
        variants.swap(next.values);++verdictEpoch;
        for(auto&entry:shaderCache)entry.first->Release();shaderCache.clear();
        return true;
    }catch(...){error="material reload allocation or file exception; old pack retained";return false;}
}

#pragma once
// Uses the independently validated native-preserving guard parser. Only its
// appended auxiliary outputs change: -1 marks a written invalid receiver.
namespace WosPrimaryGuard {
static bool Build(const std::vector<unsigned char>&input,std::vector<unsigned char>&output,std::string&reason){
    unsigned zero=0;if(!WosIndirectGuard::Build(input,output,reason,&zero))return false;
    std::vector<uint32_t>w(output.size()/4);std::memcpy(w.data(),output.data(),output.size());
    bool definition=false;
    for(size_t i=1;i<w.size();){
        unsigned op=w[i]&0xffffu;
        if(op==0xfffeu){i+=1+((w[i]>>16)&0x7fffu);continue;}
        if(op==0xffffu)break;
        unsigned n=(w[i]>>24)&15u;
        if(op==81&&n==5&&w[i+1]==(0xa00f0000u|zero)){
            if(w[i+2]||w[i+3]||w[i+4]||w[i+5]){output.clear();reason="guard zero definition changed";return false;}
            w[i+5]=0xbf800000u;definition=true;break;
        }
        i+=n+1;
    }
    if(!definition||w.size()<7||w[w.size()-7]!=0x02000001u||w[w.size()-6]!=0x800f0801u||
       w[w.size()-4]!=0x02000001u||w[w.size()-3]!=0x800f0802u){output.clear();reason="guard suffix mismatch";return false;}
    w[w.size()-2]=0xa0000000u|zero; // oC2 = cZero.xxxx, not sentinel in alpha.
    w.pop_back();w.push_back(0x02000001u);w.push_back(0x800f0803u);w.push_back(0xa0000000u|zero);w.push_back(0xffffu);
    output.resize(w.size()*4);std::memcpy(output.data(),w.data(),output.size());reason="native RGBA with primary invalid-receiver marker";return true;
}
}

#include "parser_under_test.h"
#include <cassert>
int main(){using namespace WosDeferredControl;Request r;std::string e;
assert(Parse("[Control]\nRequestId=1\nSkyIBL=1\nShadowWork=1\nIndirectEnabled=0",r,e)&&r.hasSky&&r.sky&&r.shadowWork&&!r.indirect);
assert(Parse("[Control]\nRequestId=2\nSkyIBL=0",r,e)&&r.hasSky&&!r.sky&&!r.hasIndirect);
assert(Parse("[Control]\nRequestId=3\nCapture=1",r,e)&&!r.hasSky&&r.capture);
for(auto s:{"SkyIBL=2","SkyIBL=nan","SkyIBL=1\nSkyIBL=0","SkyIBL=true","SkyIBL=-1"})assert(!Parse(std::string("[Control]\nRequestId=4\n")+s,r,e));
puts("PASS sky control parser 8 scenarios");}

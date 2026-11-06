from pathlib import Path
import json,hashlib,subprocess,re
M=Path(__file__).resolve().parent;G=M.parents[1];S=G/'WoS_Deferred/full_deferred_staged'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
receipt=json.loads((S/'build/build_receipt.json').read_text())
baseline=json.loads((G/'WoS_Deferred/effects_current/verified_baseline.json').read_text())['source_files']
changes={p.replace('\\','/'):h for p,h in json.loads((M/'verified_changes.json').read_text()).items()}
assert receipt['valid'] and receipt['inputs_unchanged']
for p,h in receipt['input_hashes'].items():assert sha(G/p)==h,p
for p,h in baseline.items():
    if p.endswith('/prepare_integration.py'):continue
    assert sha(G/p)==changes.get(p,h),('Unexpected baseline change',p)
assert sha(S/'build/winmm.dll')==receipt['dll_sha256']
assert sha(G/'WoS_Deferred/full_deferred/pbr_legacy_bridge.hlsl')==sha(M/'package/pbr_legacy_bridge.hlsl')
assert sha(G/'WoS_Deferred/full_deferred/deferred_resolve.cso')==sha(M/'baseline.cso')
source=(S/'integration/deferred_control.h').read_text()
parser=source[:source.index('static std::string Directory()')]+'}\n'
(M/'parser_under_test.h').write_text(parser)
test=r'''#include "parser_under_test.h"
#include <cassert>
int main(){using namespace WosDeferredControl;Request r;std::string e;
assert(Parse("[Control]\nRequestId=1\nSkyIBL=1\nShadowWork=1\nIndirectEnabled=0",r,e)&&r.hasSky&&r.sky&&r.shadowWork&&!r.indirect);
assert(Parse("[Control]\nRequestId=2\nSkyIBL=0",r,e)&&r.hasSky&&!r.sky&&!r.hasIndirect);
assert(Parse("[Control]\nRequestId=3\nCapture=1",r,e)&&!r.hasSky&&r.capture);
for(auto s:{"SkyIBL=2","SkyIBL=nan","SkyIBL=1\nSkyIBL=0","SkyIBL=true","SkyIBL=-1"})assert(!Parse(std::string("[Control]\nRequestId=4\n")+s,r,e));
puts("PASS sky control parser 8 scenarios");}
'''
(M/'parser_test.cpp').write_text(test)
(M/'parser_test.cmd').write_text('@echo off\ncall "C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\VC\\Auxiliary\\Build\\vcvars32.bat"\nif errorlevel 1 exit /b 1\ncd /d "%~dp0"\ncl /nologo /MT /EHsc /std:c++17 parser_test.cpp /Fe:parser_test.exe\nif errorlevel 1 exit /b 1\nparser_test.exe\n')
run=subprocess.run(['cmd','/c',str(M/'parser_test.cmd')],capture_output=True,text=True);assert run.returncode==0,run.stdout+run.stderr
gpu=subprocess.run([str(M/'gpu_test.exe')],cwd=M,capture_output=True,text=True);assert gpu.returncode==0 and 'PASS checks=2423' in gpu.stdout,gpu.stdout
proof={'valid':True,'dll_sha256':receipt['dll_sha256'],'baseline_unchanged_except':list(changes),'input_hashes':receipt['input_hashes'],'gpu':gpu.stdout,'parser':run.stdout,'in_game_verified':False}
(M/'verified_validation.json').write_text(json.dumps(proof,indent=2))
print(json.dumps({k:v for k,v in proof.items() if k!='input_hashes'},indent=2))

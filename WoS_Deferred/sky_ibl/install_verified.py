"""Install ONLY the tested build derived from the byte-proven current baseline."""
from pathlib import Path
import json,hashlib,subprocess,datetime,shutil,os
M=Path(__file__).resolve().parent;G=M.parents[1];S=G/'WoS_Deferred/full_deferred_staged'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
proof=json.loads((M/'verified_validation.json').read_text());assert proof['valid']
for p,h in proof['input_hashes'].items():assert sha(G/p)==h,p
dll=S/'build/winmm.dll';active=G/'winmm.dll'
assert sha(dll)==proof['dll_sha256']
assert sha(active)=='ac2a629d1b274817e187d85fadc4a3b5442797fb9c89c65528347b45456d7c38','Active DLL changed'
run=subprocess.run(['powershell','-NoProfile','-Command',"@(Get-Process | Where-Object {$_.ProcessName -match 'Spider|Play - WoS'}).Count"],capture_output=True,text=True,check=True)
assert int(run.stdout.strip())==0,'Close game before install'
backup=M/'backup'/('verified_'+datetime.datetime.now().strftime('%Y%m%d_%H%M%S'));backup.mkdir(parents=True)
shutil.copy2(active,backup/'winmm.dll');shutil.copy2(G/'WoS_Deferred/full_deferred/settings.ini',backup/'settings.ini')
for p,h in proof['input_hashes'].items():
    target=backup/'source'/p;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(G/p,target);assert sha(target)==h
shutil.copy2(dll,backup/'winmm.new.dll');shutil.copy2(M/'verified_validation.json',backup/'validation.json')
settings=sha(G/'WoS_Deferred/full_deferred/settings.ini')
temporary=G/'winmm.verified_pending';assert not temporary.exists();shutil.copy2(dll,temporary);os.replace(temporary,active)
assert sha(active)==proof['dll_sha256'] and settings==sha(G/'WoS_Deferred/full_deferred/settings.ini')
receipt={'installed':True,'backup':str(backup),'dll_sha256':sha(active),'in_game_verified':False,'settings_unchanged':True}
(M/'verified_install.json').write_text(json.dumps(receipt,indent=2));print(json.dumps(receipt,indent=2))

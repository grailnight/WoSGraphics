"""Install only the isolated DLL, with source/hash guards and reversible backup."""
from pathlib import Path
import argparse,datetime,json,hashlib,subprocess,shutil,os,struct
ROOT=Path(__file__).resolve().parent
GAME=ROOT.parent.parent
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def closed():
    run=subprocess.run(['powershell','-NoProfile','-Command',"@(Get-Process | Where-Object {$_.ProcessName -match 'Spider|Play - WoS'}).Count"],capture_output=True,text=True,check=True)
    if int(run.stdout.strip()):raise RuntimeError('Game must be closed; no process stopped.')
def main():
    ap=argparse.ArgumentParser();ap.add_argument('--apply',action='store_true');ap.add_argument('--rollback',action='store_true');args=ap.parse_args()
    active=GAME/'winmm.dll';receipt=ROOT/'install.json'
    if args.rollback:
        closed();r=json.loads(receipt.read_text());assert sha(active)==r['installed_sha256'],'Installed DLL changed after this install'
        backup=Path(r['backup']).resolve();assert backup.is_relative_to(ROOT/'backup')
        assert sha(backup/'winmm.dll')==r['before_sha256']
        shutil.copy2(backup/'winmm.dll',active);r['rolled_back']=True;receipt.write_text(json.dumps(r,indent=2));print('DLL restored; settings preserved.');return
    blocked=ROOT/'deployment_block.json'
    if blocked.exists():raise RuntimeError('Deployment blocked after in-game regression: '+blocked.read_text())
    plan=json.loads((ROOT/'candidate.json').read_text())
    for name,h in plan['source_inputs'].items():assert sha(GAME/name)==h,('Shared source changed; rebuild from latest first',name)
    for name,h in plan['candidate_inputs'].items():assert sha(ROOT/name)==h,('Candidate source changed',name)
    assert sha(active)==plan['baseline_dll_sha256'],'Active DLL changed; do not overwrite other work'
    for name,h in json.loads((ROOT/'build.json').read_text()).items():assert sha(ROOT/name)==h,('Shader input changed',name)
    proof=json.loads((ROOT/'validation.json').read_text());assert proof['valid']
    for name,h in proof['inputs'].items():assert sha(ROOT/name)==h,('GPU proof stale',name)
    dll=ROOT/'winmm.candidate.dll';data=dll.read_bytes();offset=struct.unpack_from('<I',data,60)[0]
    assert data[offset:offset+4]==b'PE\0\0' and struct.unpack_from('<H',data,offset+4)[0]==0x14c
    print('Preflight passed: tested module, frozen sources, unchanged active DLL, x86 candidate.')
    if not args.apply:return
    closed();backup=ROOT/'backup'/datetime.datetime.now().strftime('%Y%m%d_%H%M%S');backup.mkdir(parents=True)
    shutil.copy2(active,backup/'winmm.dll');shutil.copy2(GAME/'WoS_Deferred/full_deferred/settings.ini',backup/'settings.ini')
    temporary=GAME/'winmm.sky_ibl_pending';assert not temporary.exists();shutil.copy2(dll,temporary);os.replace(temporary,active)
    assert sha(active)==sha(dll)
    r={'installed_at':datetime.datetime.now().astimezone().isoformat(),'backup':str(backup),'before_sha256':plan['baseline_dll_sha256'],'installed_sha256':sha(active),'in_game_verified':False,'source_snapshot':'candidate_source','settings_changed':False}
    receipt.write_text(json.dumps(r,indent=2));print(json.dumps(r,indent=2))
if __name__=='__main__':main()

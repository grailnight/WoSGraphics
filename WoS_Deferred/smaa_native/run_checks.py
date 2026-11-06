from pathlib import Path
import datetime,hashlib,json,re,subprocess
root=Path(__file__).resolve().parent
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def inputs():
    files=[root/n for n in ('smaa_runtime.h','smaa_data.h','smaa.hlsl','prepare_assets.py','integrate.py','test_smaa.cpp','test.ps1','run_checks.py')]
    files+=list((root/'third_party').glob('*.hlsl'))+list((root/'third_party').glob('*.png'))
    return {p.relative_to(root).as_posix():sha(p) for p in files}
before=inputs()
run=subprocess.run(['powershell','-NoProfile','-ExecutionPolicy','Bypass','-File',str(root/'test.ps1')],capture_output=True,text=True)
log=run.stdout+run.stderr
(root/'build/test.log').write_text(log)
match=re.search(r'PASS (\d+) checks; native D3D9 HAL',log)
valid=run.returncode==0 and bool(match) and before==inputs()
receipt={'valid':valid,'inputs_unchanged':before==inputs(),'gpu_hal_executed':True,'checks':int(match[1]) if match else 0,'input_hashes':before,'timestamp':datetime.datetime.now(datetime.timezone.utc).isoformat(),'test_exe_sha256':sha(root/'build/test_smaa.exe'),'log_sha256':sha(root/'build/test.log'),'live_gameplay_verified':False}
(root/'build/test_receipt.json').write_text(json.dumps(receipt,indent=2))
print(log)
if not valid:raise SystemExit(1)

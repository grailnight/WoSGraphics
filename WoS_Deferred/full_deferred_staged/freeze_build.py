"""Freeze project-owned DLL dependencies before/after the production build."""
from pathlib import Path
import hashlib,json,re,sys,struct,datetime
ROOT=Path(__file__).resolve().parent
GAME=ROOT.parent.parent
IMGUI=GAME/'WoS_VolumetricClouds/tools/imgui'

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def snapshot():
    queue=[ROOT/'integration/wos_clouds_menu.cpp']+[IMGUI/p for p in ('imgui.cpp','imgui_draw.cpp','imgui_tables.cpp','imgui_widgets.cpp','backends/imgui_impl_win32.cpp','backends/imgui_impl_dx9.cpp')]
    queue += [ROOT/'build_integration.ps1',ROOT/'prepare_integration.py',ROOT/'build/winmm.generated.def',Path(__file__)]
    found={}
    while queue:
        p=queue.pop().resolve()
        if p in found:continue
        if not p.is_file():raise RuntimeError('Missing dependency '+str(p))
        found[p]=sha(p)
        if p.suffix not in ('.cpp','.h','.hpp'):continue
        for inc in re.findall(r'^\s*#\s*include\s*"([^"]+)"',p.read_text(errors='replace'),re.M):
            options=[p.parent/inc,IMGUI/inc,IMGUI/'backends'/inc]
            match=next((q.resolve() for q in options if q.is_file()),None)
            if match:queue.append(match)
            # Optional/platform-disabled includes are validated by the compiler.
            # A file that appears during compilation is detected by the final scan.
    return {str(p.relative_to(GAME)).replace('\\','/'):h for p,h in sorted(found.items())}

def main():
    mode=sys.argv[1];pending=ROOT/'build/build_inputs.json'
    current=snapshot()
    if mode=='begin':
        pending.write_text(json.dumps({'started_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'input_hashes':current},indent=2));print('Frozen build inputs:',len(current));return
    before=json.loads(pending.read_text());dll=ROOT/'build/winmm.dll';data=dll.read_bytes()
    pe=struct.unpack_from('<I',data,60)[0];machine=struct.unpack_from('<H',data,pe+4)[0];magic=struct.unpack_from('<H',data,pe+24)[0]
    valid=before['input_hashes']==current and data[pe:pe+4]==b'PE\0\0' and machine==0x14c and magic==0x10b
    result={'schema':1,'valid':valid,'inputs_unchanged':before['input_hashes']==current,'pe_machine':hex(machine),'pe_optional_magic':hex(magic),'started_utc':before['started_utc'],'finished_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'dll_sha256':sha(dll),'dll_bytes':len(data),'input_hashes':current}
    (ROOT/'build/build_receipt.json').write_text(json.dumps(result,indent=2));print(json.dumps({'valid':valid,'dependencies':len(current),'dll_sha256':result['dll_sha256']}))
    if not valid:raise RuntimeError('Build input consistency or x86 validation failed')
if __name__=='__main__':main()

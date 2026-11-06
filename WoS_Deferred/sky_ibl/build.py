from pathlib import Path
import json,re,hashlib,ctypes as C,importlib.util
ROOT=Path(__file__).resolve().parent
GAME=ROOT.parent.parent
spec=importlib.util.spec_from_file_location('compiler',ROOT/'compiler.py')
compiler=importlib.util.module_from_spec(spec);spec.loader.exec_module(compiler)
def compile(path,output):
    original=path
    path=ROOT/'package'/original.name
    path.write_text(original.read_text().replace('"package/','"'))
    data=path.read_bytes();code,error=C.c_void_p(),C.c_void_p()
    hr=compiler.compile_shader(data,len(data),str(path).encode(),None,C.c_void_p(1),b'main',b'ps_3_0',1<<15,0,C.byref(code),C.byref(error))
    diagnostics=compiler.blob(error).decode(errors='replace') if error else ''
    if hr<0:raise RuntimeError(diagnostics)
    output.write_bytes(compiler.blob(code))
def main():
    profiles=json.loads((GAME/'WoS_VolumetricClouds/sky_profiles/profiles.json').read_text())
    rows=[]
    for p in profiles:
        values=sum((p[k]+[0] for k in ['horizon','middle','zenith']),[])
        rows.append('{'+','.join(f'{v:.8f}f' for v in values)+'}')
    (ROOT/'sky_palette.generated.h').write_text('#pragma once\nnamespace WosSkyIBLData {\nstatic const float palette[10][12]={\n'+',\n'.join(rows)+'\n};\n}\n')
    # Existing resolve is near the SM3 temporary-register limit. Keep it exact
    # and add IBL as a separate RGB-only ONE/ONE pass before the HDR copy-out.
    path=ROOT/'sky_resolve.hlsl'
    compile(path,path.with_suffix('.cso'))
    compile(ROOT/'probe.hlsl',ROOT/'probe.cso')
    compile(ROOT/'preview.hlsl',ROOT/'preview.cso')
    code=path.with_suffix('.cso').read_bytes()
    import struct
    words=struct.unpack('<'+'I'*(len(code)//4),code)
    (ROOT/'sky_shader.generated.h').write_text('#pragma once\nnamespace WosSkyIBLData { static const DWORD shader[]={'+','.join(hex(v) for v in words)+'}; }\n')
    files=[*sorted((ROOT/'package').glob('*.hlsl')),*sorted((ROOT/'package').glob('*.hlsli')),path,path.with_suffix('.cso'),ROOT/'probe.cso',ROOT/'preview.cso']
    receipt={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
    (ROOT/'build.json').write_text(json.dumps(receipt,indent=2));print('Compiled separate sky resolve, IBL probe, material preview.')
if __name__=='__main__':main()

from pathlib import Path
from PIL import Image
import hashlib,json,subprocess
ROOT=Path(__file__).resolve().parent
GAME=ROOT.parent.parent
third=ROOT/'third_party'
third.mkdir(exist_ok=True)
src=GAME/'reshade-shaders/Shaders/SweetFX/SMAA.fxh'
# Vendor the MIT algorithm, not the ReShade effect wrapper or runtime.
if not (third/'SMAA.hlsl').exists():
    (third/'SMAA.hlsl').write_bytes(src.read_bytes())
    for name in ('AreaTex','SearchTex'):
        (third/(name+'.png')).write_bytes((GAME/('reshade-shaders/Textures/SweetFX/'+name+'.png')).read_bytes())
    (third/'PROVENANCE.json').write_text(json.dumps({'upstream':'https://github.com/iryoku/smaa','source':'Existing local SweetFX distribution; exact upstream revision unknown; original MIT notice retained','sha256':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in third.iterdir() if p.suffix in ('.hlsl','.png')}},indent=2))
fxc=r'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x86\fxc.exe'
out=['#pragma once\nnamespace WosSmaaData {']
for entry in ('Edges','Weights','Blend'):
    dest=ROOT/'build'/(entry+'.cso')
    subprocess.run([fxc,'/nologo','/T','ps_3_0','/E',entry,'/O3','/Fo',str(dest),str(ROOT/'smaa.hlsl')],check=True)
    data=dest.read_bytes()
    out.append('static const unsigned long '+entry+'[] = {'+','.join(hex(int.from_bytes(data[i:i+4],'little')) for i in range(0,len(data),4))+'};')
for name in ('AreaTex','SearchTex'):
    im=Image.open(third/(name+'.png')).convert('RGBA')
    out.append('static const unsigned char '+name+'[] = {'+','.join(str(v) for p in im.getdata() for v in (p[2],p[1],p[0],p[3]))+'};')
    out.append(f'static const unsigned {name}Width={im.width}, {name}Height={im.height};')
out.append('}')
(ROOT/'smaa_data.h').write_text('\n'.join(out))

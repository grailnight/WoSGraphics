"""Verify the built renderer DLL before installation.

The integration/ tree is the source of truth: prepare_integration.py is a guard
stub, and build_integration.ps1 compiles integration/wos_clouds_menu.cpp and
everything it includes. So the checks that decide installability are:

  * the build receipt still describes the sources on disk (nothing edited
    since the build) and the DLL it produced;
  * no renderer feature disappeared: every marker string the currently
    installed DLL carries must also be in the new one (a clean build from the
    old stager once silently dropped AgX and SSGI anti-ghosting);
  * release assets, the game executable and the WinMM forwarder are intact.

The frozen GPU receipts in tests/ pin the root copies of the runtime headers,
which have diverged from the integration/ copies that are actually built. They
are reported, not enforced; pass --strict to require them.
"""
from pathlib import Path
import hashlib,json,sys,struct
ROOT=Path(__file__).resolve().parent;GAME=ROOT.parent.parent

# Menu and status strings that only exist when a feature was compiled in.
FEATURE_MARKERS=('AgX (punchy)','SSGI anti-ghosting','TAA (temporal anti-aliasing)',
    'Water reflections (shaderpack style)','Cloud shadows on the city','Cloud sun blocking',
    'Volumetric sun shafts','Four dynamic cascades (PCSS)','Rain ripples',
    'Sun: synchronized with native directional light','Sky height map (off-screen occluders)','SSGI multi-bounce','Streaming poll','Specular AA strength','Contact max pixels','Material class strength','Цвет от стиля','Cascade stagger','Chimney smoke','SA district shadow casters','Soft particles','Разрешение облаков','Lamp light shafts','Street steam ON','DLAA (DXVK)','symbiote: goo reflects','WoS G-buffer dump v1')

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def safe(base,name):
    p=(base/name).resolve()
    if not p.is_relative_to(GAME):raise RuntimeError('Path outside game workspace: '+str(p))
    return p
def stale(base,items):
    return [n for n,h in items.items() if not safe(base,n).is_file() or sha(safe(base,n))!=h]
def verify_hashes(base,items):
    bad=stale(base,items)
    if bad:raise RuntimeError('Stale/missing verified input: '+str(safe(base,bad[0])))

def gpu_receipts():
    """Name -> list of stale inputs ([] means the receipt still holds)."""
    out={}
    runtime=json.loads((ROOT/'tests/runtime_receipt.json').read_text())
    out['runtime_receipt']=stale(GAME,runtime['sha256']) if runtime['passed'] else ['<failed>']
    for name in ('runtime_inherited_state_receipt','release_loader_receipt','control_receipt','replay_state_receipt','lights_read_receipt'):
        r=json.loads((ROOT/'tests'/(name+'.json')).read_text())
        # The lights runner records paths relative to STAGE; the rest use WoS_Deferred.
        base=ROOT if name=='lights_read_receipt' else ROOT.parent
        out[name]=stale(base,r['input_hashes']) if r['valid'] and r['inputs_unchanged'] else ['<failed>']
    return out

def main():
    strict='--strict' in sys.argv[1:]
    build=json.loads((ROOT/'build/build_receipt.json').read_text())
    if not build['valid'] or not build['inputs_unchanged']:raise RuntimeError('Unverified build')
    bad=stale(GAME,build['input_hashes'])
    if bad:raise RuntimeError('Sources changed since the build; rebuild first: '+', '.join(bad[:5]))
    dll=ROOT/'build/winmm.dll';data=dll.read_bytes()
    if sha(dll)!=build['dll_sha256']:raise RuntimeError('DLL changed')

    missing=[m for m in FEATURE_MARKERS if m.encode() not in data]
    if missing:raise RuntimeError('Built DLL lacks renderer features: '+', '.join(missing))
    installed=GAME/'winmm.dll';lost=[]
    if installed.is_file():
        live=installed.read_bytes()
        lost=[m for m in FEATURE_MARKERS if m.encode() in live and m.encode() not in data]
    if lost:raise RuntimeError('Build would remove installed features: '+', '.join(lost))

    receipts=gpu_receipts();stale_receipts={k:v for k,v in receipts.items() if v}
    if strict and stale_receipts:raise RuntimeError('GPU receipts stale: '+', '.join(stale_receipts))
    diverged=[n for n in ('deferred_runtime.h','deferred_lights.h','deferred_control.h','deferred_replay_state.h')
              if sha(ROOT/n)!=sha(ROOT/'integration'/n)]

    assets=ROOT/'release/assets';release=json.loads((assets/'release_manifest.json').read_text())
    verify_hashes(assets,release['asset_hashes'])
    expected=set(release['asset_hashes'])|{'release_manifest.json'}
    actual={str(p.relative_to(assets)).replace('\\','/') for p in assets.rglob('*') if p.is_file()}
    if {x.replace('\\','/') for x in expected}!=actual:raise RuntimeError('Unexpected release files')
    exe=GAME/'Spider-Man Web of Shadows.exe'
    if sha(exe)!='75231c20ea5521e1ec714ef7cb50469bf3d5169c63d1f489fa7fdf3c01b6d345':raise RuntimeError('Unverified game executable')
    # The DLL forwards WinMM to the system copy itself (winmm_forward.h);
    # a build that still names winmm_original.dll would need the renamed copy.
    if b'winmm_original' in data:raise RuntimeError('DLL still forwards to winmm_original.dll')

    result={'valid':True,'schema':2,'variants':len(release['variants']),'dll_sha256':build['dll_sha256'],
        'installed_dll_sha256':sha(installed) if installed.is_file() else None,
        'already_installed':installed.is_file() and sha(installed)==build['dll_sha256'],
        'feature_markers':len(FEATURE_MARKERS),'release_manifest_sha256':sha(assets/'release_manifest.json'),
        'gpu_receipts_current':not stale_receipts,'stale_gpu_receipts':stale_receipts,
        'root_headers_diverged_from_integration':diverged,'installed':False,
        'full_material_coverage':release['full_material_coverage']}
    (ROOT/'release/install_validation.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
if __name__=='__main__':main()

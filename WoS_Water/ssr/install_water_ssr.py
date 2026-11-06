"""Install shaderpack-style water reflections: DLL + two loose shaders, with backup and receipt."""
from pathlib import Path
import hashlib, json, shutil, subprocess, time, datetime

ROOT = Path(r'B:\Games\Spider Man - Web of Shadows')
HERE = Path(__file__).resolve().parent
EXPECTED_PREVIOUS = 'cc2ee119bdab1f8e36d72ac6c354dfeb36bfff3e409e19e42b2d68ab7471861a'
BUILD = ROOT / 'WoS_Deferred/full_deferred_staged/build/winmm.dll'
LIVE = ROOT / 'winmm.dll'
SHADERS = ROOT / 'WoS_Deferred/water_ssr'


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


running = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq Spider-Man Web of Shadows.exe'], capture_output=True, text=True).stdout
assert 'Spider-Man Web of Shadows.exe' not in running, 'close the game first'
assert sha(LIVE) == EXPECTED_PREVIOUS, 'live DLL is not the expected TAA build; re-check before installing'
log = (HERE / 'gpu_test.log').read_text(errors='replace')
assert log.strip().endswith('PASS'), 'GPU test has not passed'

stamp = time.strftime('%Y%m%d_%H%M%S')
backup = HERE / f'install_backup_{stamp}'
backup.mkdir()
shutil.copy2(LIVE, backup / 'winmm.dll')
SHADERS.mkdir(exist_ok=True)
for name in ('water_ssr.cso', 'water_mask.cso'):
    if (SHADERS / name).exists():
        shutil.copy2(SHADERS / name, backup / name)
    shutil.copy2(HERE / name, SHADERS / name)
shutil.copy2(BUILD, LIVE)
assert sha(LIVE) == sha(BUILD)

receipt = dict(
    installed=True,
    scope='Shaderpack-style water: mask replay of sm_water_mesh + SSR/Fresnel/sun glint at the bloom composite; [WaterSSR] in clouds_menu.ini',
    installed_at=datetime.datetime.now().astimezone().isoformat(),
    previous_dll_sha256=EXPECTED_PREVIOUS,
    dll_sha256=sha(LIVE),
    shaders={n: sha(SHADERS / n) for n in ('water_ssr.cso', 'water_mask.cso')},
    backup=str(backup),
    gpu_test='PASS, WoS_Water/ssr/gpu_test.log',
    live_gameplay_verified=False,
)
(HERE / 'install.json').write_text(json.dumps(receipt, indent=2))
print(json.dumps(receipt, indent=2))



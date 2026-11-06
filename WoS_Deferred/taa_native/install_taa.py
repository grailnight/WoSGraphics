"""Native TAA install over the current hook. Run only with the game closed."""
from pathlib import Path
import datetime, hashlib, json, shutil, subprocess

HERE = Path(__file__).resolve().parent
GAME = HERE.parents[1]
STAGE = GAME / 'WoS_Deferred/full_deferred_staged'
LIVE_SHADERS = GAME / 'WoS_Deferred/taa'
OLD_DLL = '043e34c9ea11af54f56dc59a646f2c8dbb978f6f6369ffbcce2a7d41fc00e7a6'


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def closed():
    r = subprocess.run(['powershell', '-NoProfile', '-Command',
                        "if (Get-Process -Name 'Spider-Man Web of Shadows' -ErrorAction SilentlyContinue) { exit 1 }"],
                       capture_output=True)
    if r.returncode:
        raise RuntimeError('Save and close the game before installing.')


def main():
    closed()
    build = json.loads((STAGE / 'build/build_receipt.json').read_text())
    assert build['valid'] and sha(STAGE / 'build/winmm.dll') == build['dll_sha256']
    for name, h in build['input_hashes'].items():
        assert sha(GAME / name) == h, name
    assert any(n.endswith('taa_native/taa_runtime.h') for n in build['input_hashes']), 'build does not contain TAA'
    test = json.loads((HERE / 'receipt.json').read_text())
    assert test['valid'] and test['shaders_reproducible_from_source']
    for name, h in test['input_hashes'].items():
        assert sha(GAME / name) == h, name
    for name in ('taa_vs_patch.h', 'taa_runtime.h'):
        key = 'WoS_Deferred/taa_native/' + name
        assert build['input_hashes'][key] == test['input_hashes'][key], 'DLL and test built from different ' + name
    assert sha(GAME / 'winmm.dll') == OLD_DLL, 'Installed DLL is not the expected previous build'

    changes = {GAME / 'winmm.dll': (STAGE / 'build/winmm.dll').read_bytes()}
    for name in ('taa_resolve.cso', 'taa_present.cso', 'taa_resolve.hlsl', 'taa_present.hlsl'):
        changes[LIVE_SHADERS / name] = (HERE / name).read_bytes()
    backup = HERE / ('install_backup_' + datetime.datetime.now().strftime('%Y%m%d_%H%M%S'))
    backup.mkdir()
    originals = {}
    for path in changes:
        originals[path] = path.read_bytes() if path.exists() else None
        if path.exists():
            copy = backup / path.relative_to(GAME)
            copy.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, copy)
    closed()
    LIVE_SHADERS.mkdir(exist_ok=True)
    try:
        for path, content in changes.items():
            path.write_bytes(content)
        for path, content in changes.items():
            assert path.read_bytes() == content, path
    except BaseException:
        for path, content in originals.items():
            if content is None:
                path.unlink(missing_ok=True)
            else:
                path.write_bytes(content)
        raise
    receipt = {
        'installed': True,
        'scope': 'Native TAA: Halton jitter through patched vertex shader twins, depth/camera reprojection, YCoCg variance clip, after SMAA at the bloom composite; [TAA] in clouds_menu.ini',
        'installed_at': datetime.datetime.now().astimezone().isoformat(),
        'previous_dll_sha256': OLD_DLL,
        'dll_sha256': sha(GAME / 'winmm.dll'),
        'shaders': {name: sha(LIVE_SHADERS / name) for name in ('taa_resolve.cso', 'taa_present.cso')},
        'backup': str(backup),
        'gpu_test': 'PASS, taa_native/receipt.json',
        'live_gameplay_verified': False,
    }
    (HERE / 'install.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(receipt, indent=2))


if __name__ == '__main__':
    main()

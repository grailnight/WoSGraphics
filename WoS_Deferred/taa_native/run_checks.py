"""Freeze native TAA against a real HAL device.

Shaders are recompiled from HLSL and must match the shipped .cso bytes; the test
is rebuilt and run; inputs are hashed before and after so an edit during the
run invalidates the receipt.
"""
from pathlib import Path
import datetime, hashlib, json, os, re, subprocess, sys

HERE = Path(__file__).resolve().parent
GAME = HERE.parents[1]
FXC = r'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x86\fxc.exe'
SHADERS = {'taa_resolve': 'ps_3_0', 'taa_present': 'ps_3_0'}
TEST_SHADERS = {'vs_basic3': ('vs_basic.hlsl', 'vs_3_0'), 'vs_basic2': ('vs_basic.hlsl', 'vs_2_0'),
                'vs_skin3': ('vs_skin.hlsl', 'vs_3_0'), 'vs_skin2': ('vs_skin.hlsl', 'vs_2_0'),
                'ps_scene': ('ps_scene.hlsl', 'ps_3_0'), 'ps_copy': ('ps_copy.hlsl', 'ps_3_0')}


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def inventory():
    paths = [HERE / 'taa_vs_patch.h', HERE / 'taa_runtime.h', HERE / 'test_taa.cpp', HERE / 'build_test.cmd', Path(__file__)]
    paths += [HERE / (n + ext) for n in SHADERS for ext in ('.hlsl', '.cso')]
    paths += sorted((HERE / 'test_shaders').glob('*.hlsl')) + sorted((HERE / 'test_shaders').glob('*.cso'))
    paths += sorted((HERE / 'test_corpus').glob('vs_*.cso'))
    return {os.path.relpath(p, GAME).replace('\\', '/'): sha(p) for p in paths}


def compile_matches(source, profile, expected):
    out = HERE / 'rebuilt.tmp'
    r = subprocess.run([FXC, '/nologo', '/T', profile, '/E', 'main', '/O3', '/Fo', str(out), str(source)], capture_output=True, text=True)
    same = r.returncode == 0 and out.read_bytes() == expected.read_bytes()
    out.unlink(missing_ok=True)
    return same


def main():
    started = datetime.datetime.now(datetime.timezone.utc)
    before = inventory()
    reproducible = all(compile_matches(HERE / (n + '.hlsl'), p, HERE / (n + '.cso')) for n, p in SHADERS.items())
    reproducible = reproducible and all(compile_matches(HERE / 'test_shaders' / src, p, HERE / 'test_shaders' / (n + '.cso'))
                                        for n, (src, p) in TEST_SHADERS.items())
    build = subprocess.run(['cmd', '/c', str(HERE / 'build_test.cmd')], cwd=GAME, capture_output=True, text=True)
    (HERE / 'build.log').write_text(build.stdout + build.stderr)
    if build.returncode:
        print(build.stdout + build.stderr)
        return 1
    run = subprocess.run([str(HERE / 'test_taa.exe')], cwd=GAME, capture_output=True, text=True)
    log = run.stdout + run.stderr
    (HERE / 'gpu_test.log').write_text(log)
    after = inventory()

    def num(pattern, group=1, cast=float):
        m = re.search(pattern, log)
        return cast(m[group]) if m else None

    summary = re.search(r'TAA GPU checks passed: (\d+) checks, corpus (\d+)/(\d+) patched', log)
    valid = run.returncode == 0 and before == after and reproducible and bool(summary)
    receipt = {
        'schema': 1, 'valid': valid, 'gpu_hal_executed': True, 'inputs_unchanged': before == after,
        'shaders_reproducible_from_source': reproducible,
        'started_utc': started.isoformat(), 'finished_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'returncode': run.returncode, 'input_hashes': before,
        'checks': int(summary[1]) if summary else None,
        'corpus_patched': int(summary[2]) if summary else None, 'corpus_total': int(summary[3]) if summary else None,
        'still_without_jitter_changed_channels': num(r'still without jitter: (\d+) channels changed', cast=int),
        'static_edge_error_aliased': num(r'aliased error ([0-9.]+), TAA error'),
        'static_edge_error_taa': num(r'TAA error ([0-9.]+);'),
        'pan_error_aliased': num(r'pan: aliased ([0-9.]+)'),
        'pan_error_taa_reprojected': num(r'TAA reprojected ([0-9.]+)'),
        'pan_error_taa_without_reprojection': num(r'TAA without reprojection ([0-9.]+)'),
        'moving_object_worst_ghost': num(r'worst ghost on clear background ([0-9.]+)'),
        'log_sha256': sha(HERE / 'gpu_test.log'),
        'checks_description': [
            'All unique vertex shaders from the F7 captures are patched, accepted by the HAL, and differ from the original only in redirected position writes plus the 8 appended tokens',
            'Synthetic vs_2_0/vs_3_0 twins (position on o2 written in four parts, relative bone addressing) with zero jitter: bit-identical colour, and a ZFUNC EQUAL replay through the twin over the original covers every pixel',
            'One pixel of jitter moves coverage by exactly one pixel',
            'A still image without jitter passes the resolve with no 8-bit channel changed',
            'Static edges: TAA error against an 8x8 supersampled reference below half of the aliased error; flat interiors unchanged; sharpening bounded',
            'Panning camera: reprojected TAA beats both the aliased frame and TAA without reprojection',
            'Moving object without motion vectors: worst residual on clear background below 0.06',
            'Unsupported composite target (offset viewport) is skipped, image untouched, jitter stops'],
        'limitations': [
            'Synthetic scenes and shaders; the in-game image, particles, alpha-tested foliage and characters are not verified here',
            'Fixed-function and vs_1_x draws in the main view stay unjittered; the menu counts them',
            'Depth-reconstructing passes (deferred resolve, SSGI, fog) use the unjittered camera, a sub-pixel reconstruction offset',
            'No frame-rate or GPU cost measurement'],
    }
    (HERE / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(log)
    print(json.dumps({'valid': valid, 'reproducible': reproducible, 'checks': receipt['checks']}))
    return 0 if valid else 1


if __name__ == '__main__':
    sys.exit(main())

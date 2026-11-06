"""Compile street_steam_vs/ps.hlsl with d3dcompiler_47 (same path as chimney_smoke/compile.py)."""
from pathlib import Path
import ctypes as C
import hashlib
import json

ROOT = Path(__file__).resolve().parent
api = C.WinDLL('d3dcompiler_47.dll')
compile_shader = api.D3DCompile
compile_shader.argtypes = [C.c_void_p, C.c_size_t, C.c_char_p, C.c_void_p, C.c_void_p, C.c_char_p, C.c_char_p,
                           C.c_uint, C.c_uint, C.POINTER(C.c_void_p), C.POINTER(C.c_void_p)]
compile_shader.restype = C.c_long
disassemble = api.D3DDisassemble
disassemble.argtypes = [C.c_void_p, C.c_size_t, C.c_uint, C.c_char_p, C.POINTER(C.c_void_p)]
disassemble.restype = C.c_long


def blob(pointer):
    v = C.cast(pointer, C.POINTER(C.POINTER(C.c_void_p))).contents
    data = C.WINFUNCTYPE(C.c_void_p, C.c_void_p)(v[3])(pointer)
    size = C.WINFUNCTYPE(C.c_size_t, C.c_void_p)(v[4])(pointer)
    result = C.string_at(data, size)
    C.WINFUNCTYPE(C.c_ulong, C.c_void_p)(v[2])(pointer)
    return result


def build(name, profile):
    path = ROOT / (name + '.hlsl')
    source = path.read_bytes()
    code, error = C.c_void_p(), C.c_void_p()
    # 1<<15 = D3DCOMPILE_OPTIMIZATION_LEVEL3
    hr = compile_shader(source, len(source), str(path).encode(), None, None, b'main', profile.encode(), 1 << 15, 0,
                        C.byref(code), C.byref(error))
    diagnostics = blob(error).decode(errors='replace') if error else ''
    if hr < 0:
        raise SystemExit(name + ': ' + diagnostics)
    output = blob(code)
    (ROOT / (name + '.cso')).write_bytes(output)
    text = C.c_void_p()
    if disassemble(output, len(output), 0, None, C.byref(text)) >= 0:
        (ROOT / (name + '.asm')).write_bytes(blob(text))
    return {'name': name, 'profile': profile, 'source_sha256': hashlib.sha256(source).hexdigest(),
            'shader_sha256': hashlib.sha256(output).hexdigest(), 'shader_bytes': len(output),
            'diagnostics': diagnostics}


if __name__ == '__main__':
    receipt = {'compiler': 'd3dcompiler_47.dll',
               'shaders': [build('street_steam_vs', 'vs_3_0'), build('street_steam_ps', 'ps_3_0')]}
    (ROOT / 'compile.json').write_text(json.dumps(receipt, indent=2))
    print(json.dumps(receipt, indent=2))

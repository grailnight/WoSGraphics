"""Compile standalone native D3D9 water passes with the Windows compiler."""
from pathlib import Path
import ctypes as C
import hashlib
import json

ROOT = Path(__file__).resolve().parent
api = C.WinDLL('d3dcompiler_47.dll')
compile_shader = api.D3DCompile
compile_shader.argtypes = [C.c_void_p, C.c_size_t, C.c_char_p, C.c_void_p, C.c_void_p,
                          C.c_char_p, C.c_char_p, C.c_uint, C.c_uint,
                          C.POINTER(C.c_void_p), C.POINTER(C.c_void_p)]
compile_shader.restype = C.c_long

def blob(pointer):
    table = C.cast(pointer, C.POINTER(C.POINTER(C.c_void_p))).contents
    data = C.WINFUNCTYPE(C.c_void_p, C.c_void_p)(table[3])(pointer)
    size = C.WINFUNCTYPE(C.c_size_t, C.c_void_p)(table[4])(pointer)
    result = C.string_at(data, size)
    C.WINFUNCTYPE(C.c_ulong, C.c_void_p)(table[2])(pointer)
    return result

source = (ROOT / 'water.hlsl').read_bytes()
receipt = {'source_sha256': hashlib.sha256(source).hexdigest(),
           'installed': False, 'game_validated': False, 'passes': []}
compiled = {}
for entry, profile in [('WaterVS', 'vs_3_0'), ('WaterSurface', 'ps_3_0'), ('WaterCaustics', 'ps_3_0')]:
    code, error = C.c_void_p(), C.c_void_p()
    hr = compile_shader(source, len(source), b'water.hlsl', None, None,
                        entry.encode(), profile.encode(), (1 << 15), 0,
                        C.byref(code), C.byref(error))
    diagnostics = blob(error).decode(errors='replace') if error else ''
    if hr < 0:
        raise RuntimeError(entry + ': ' + diagnostics)
    output = blob(code)
    compiled[entry] = output
    receipt['passes'].append({'entry': entry, 'profile': profile, 'bytes': len(output),
                             'sha256': hashlib.sha256(output).hexdigest(), 'diagnostics': diagnostics})
for entry, output in compiled.items():
    (ROOT / (entry + '.cso')).write_bytes(output)
(ROOT / 'build_receipt.json').write_text(json.dumps(receipt, indent=2))
print(json.dumps(receipt, indent=2))


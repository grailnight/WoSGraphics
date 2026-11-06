"""Build water_mask.cso from the installed v3 sm_water_mesh replacement.

The mask program is the replacement's own instruction stream up to the point
where it has the rippled world normal in r1.xyz, followed by two outputs:
RGB = normal * 0.5 + 0.5, A = distance from WorldEyePosition (c6) to the
surface (world position v0). Registers, samplers and interpolators are the
native ones, so the replay needs no constants of its own.
"""
from pathlib import Path
import ctypes as C, hashlib, json

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parent / 'sm_water_mesh.native.asm'

lines = SOURCE.read_text().splitlines()
anchor = [i for i, l in enumerate(lines) if l.strip() == 'mad r0.xyz, r0.w, v1, r0']
assert len(anchor) == 1, anchor
cut = anchor[0] + 1
assert lines[cut].strip() == 'nrm r1.xyz, r0', lines[cut]
body = lines[:cut + 1] + [
    '    add r0.xyz, c6, -v0',
    '    dp3 r0.w, r0, r0',
    '    rsq r0.w, r0.w',
    '    rcp oC0.w, r0.w',
    '    mul r1.xyz, r1, c17.y',
    '    add oC0.xyz, r1, c17.y',
]
source = '\n'.join(body) + '\n'

api = C.WinDLL('d3dx9_43.dll')
assemble = api.D3DXAssembleShader
assemble.argtypes = [C.c_char_p, C.c_uint, C.c_void_p, C.c_void_p, C.c_uint, C.POINTER(C.c_void_p), C.POINTER(C.c_void_p)]
assemble.restype = C.c_long


def blob(pointer):
    vtable = C.cast(C.cast(pointer, C.POINTER(C.c_void_p))[0], C.POINTER(C.c_void_p))
    get_pointer = C.WINFUNCTYPE(C.c_void_p, C.c_void_p)(vtable[3])
    get_size = C.WINFUNCTYPE(C.c_size_t, C.c_void_p)(vtable[4])
    return C.string_at(get_pointer(pointer), get_size(pointer))


code, error = C.c_void_p(), C.c_void_p()
encoded = source.encode()
hr = assemble(encoded, len(encoded), None, None, 0, C.byref(code), C.byref(error))
if hr < 0:
    raise SystemExit('assemble failed: ' + (blob(error).decode(errors='replace') if error else hex(hr & 0xffffffff)))
data = blob(code)
(HERE / 'water_mask.asm').write_text(source)
(HERE / 'water_mask.cso').write_bytes(data)
record = dict(source=str(SOURCE.name), source_sha256=hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
              bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
(HERE / 'water_mask.json').write_text(json.dumps(record, indent=2))
print(json.dumps(record, indent=2))

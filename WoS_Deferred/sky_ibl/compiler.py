"""Compile the D3D9 resolve using the installed Microsoft HLSL compiler."""
from pathlib import Path
import ctypes as C
import hashlib, json
ROOT=Path(__file__).resolve().parent
api=C.WinDLL('d3dcompiler_47.dll')
compile_shader=api.D3DCompile
compile_shader.argtypes=[C.c_void_p,C.c_size_t,C.c_char_p,C.c_void_p,C.c_void_p,C.c_char_p,C.c_char_p,C.c_uint,C.c_uint,C.POINTER(C.c_void_p),C.POINTER(C.c_void_p)]
compile_shader.restype=C.c_long
def blob(pointer):
    v=C.cast(pointer,C.POINTER(C.POINTER(C.c_void_p))).contents
    data=C.WINFUNCTYPE(C.c_void_p,C.c_void_p)(v[3])(pointer)
    size=C.WINFUNCTYPE(C.c_size_t,C.c_void_p)(v[4])(pointer)
    result=C.string_at(data,size)
    C.WINFUNCTYPE(C.c_ulong,C.c_void_p)(v[2])(pointer)
    return result
def main():
    source=(ROOT/'deferred_resolve.hlsl').read_bytes()
    code,error=C.c_void_p(),C.c_void_p()
    hr=compile_shader(source,len(source),str(ROOT/'deferred_resolve.hlsl').encode(),None,C.c_void_p(1),b'main',b'ps_3_0',1<<15,0,C.byref(code),C.byref(error))
    diagnostics=blob(error).decode(errors='replace') if error else ''
    if hr<0:raise RuntimeError(diagnostics)
    output=blob(code)
    (ROOT/'deferred_resolve.cso').write_bytes(output)
    receipt={'profile':'ps_3_0','compiler':'d3dcompiler_47.dll','source_sha256':hashlib.sha256(source).hexdigest(),'brdf_sha256':hashlib.sha256((ROOT/'deferred_brdf.hlsl').read_bytes()).hexdigest(),'cloud_shadow_sha256':hashlib.sha256((ROOT/'deferred_cloud_shadow.hlsl').read_bytes()).hexdigest(),'local_shadow_sha256':hashlib.sha256((ROOT/'deferred_local_shadow.hlsl').read_bytes()).hexdigest(),'shader_sha256':hashlib.sha256(output).hexdigest(),'shader_bytes':len(output),'diagnostics':diagnostics}
    (ROOT/'resolve_compile.json').write_text(json.dumps(receipt,indent=2))
    print(json.dumps(receipt))
if __name__=='__main__':main()

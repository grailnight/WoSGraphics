"""Replace LoadStreetSteam() in wos_clouds_menu.cpp and the [StreetSteam] ini section (v2: SM3 camera smoke)."""
from pathlib import Path
import re, shutil, time

HERE = Path(__file__).resolve().parent
CPP = HERE.parent / 'full_deferred_staged' / 'integration' / 'wos_clouds_menu.cpp'
INI = HERE.parent.parent / 'WoS_VolumetricClouds' / 'clouds_menu.ini'

KEYS = [  # key, variable, default, lo, hi
    ('Density', 'density', 1.0, 0.0, 4.0),
    ('Brightness', 'brightness', 1.0, 0.0, 8.0),
    ('Ambient', 'ambientShare', 1.0, 0.0, 4.0),
    ('Sun', 'sunShare', 0.35, 0.0, 4.0),
    ('HighAltitude', 'highAltitude', 40.0, 0.0, 1000.0),
    ('FunnelScale', 'funnelScale', 4.0, 0.1, 100.0),
    ('WindAngle', 'windAngle', 0.0, 0.0, 360.0),
    ('WindScale', 'windScale', 1.0, 0.0, 10.0),
    ('SoftRange', 'softRange', 8.0, 0.5, 50.0),
    ('MaxDistance', 'maxDistance', 300.0, 50.0, 2000.0),
]

src = CPP.read_bytes().decode('utf-8')
nl = '\r\n' if '\r\n' in src else '\n'
start = src.index('static void LoadStreetSteam()')
end = src.index('// Cloud march resolution:', start)
body = ['static void LoadStreetSteam()', '{', '    using namespace WosStreetSteam;',
        '    auto clamp = [](float v, float lo, float hi) { return std::isfinite(v) ? (std::min)(hi, (std::max)(lo, v)) : lo; };',
        '    enabled = ReadFloatIn("StreetSteam", "Enabled", 1.0f) > 0.5f;',
        '    cameraSmoke = ReadFloatIn("StreetSteam", "CameraSmoke", 1.0f) > 0.5f;',
        '    funnelSmoke = ReadFloatIn("StreetSteam", "FunnelSmoke", 1.0f) > 0.5f;']
for key, var, _, lo, hi in KEYS:
    body.append(f'    {var} = clamp(ReadFloatIn("StreetSteam", "{key}", {var}), {lo!r}f, {hi!r}f);')
body.append('    maxParticles = int(clamp(ReadFloatIn("StreetSteam", "MaxParticles", float(maxParticles)), 0.0f, 6000.0f));')
body.append('}')
new = nl.join(body) + nl
shutil.copyfile(CPP, HERE / ('wos_clouds_menu.before_loader_v2_%s.cpp' % time.strftime('%Y%m%d_%H%M%S')))
CPP.write_bytes((src[:start] + new + src[end:]).encode('utf-8'))

ini = INI.read_bytes().decode('utf-8')
inl = '\r\n' if '\r\n' in ini else '\n'
lines = ['[StreetSteam]', 'Enabled=1.000000', 'CameraSmoke=1.000000', 'FunnelSmoke=1.000000']
lines += ['%s=%f' % (k, d) for k, _, d, _, _ in KEYS] + ['MaxParticles=1500.000000']
section = inl.join(lines) + inl
m = re.search(r'\[StreetSteam\][^\[]*', ini)
ini = ini[:m.start()] + section + ini[m.end():] if m else ini + section
INI.write_bytes(ini.encode('utf-8'))
print('loader and ini updated')

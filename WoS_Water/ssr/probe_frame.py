"""List the draw sequence of a frame capture around the water draws (ps whose asm contains sm_water/HeightScale)."""
import sys, re
from pathlib import Path
cap = Path(sys.argv[1])
names = {}
for asm in cap.glob('ps_*.asm'):
    t = asm.read_text(errors='replace')
    idx = int(asm.stem[3:])
    tag = []
    for key in ('HeightScale', 'z_feather_range', 'ZBUFFER', 'gCloudParams', 'environment_Sampler', 'Normal_texture'):
        if key in t: tag.append(key)
    m = re.findall(r'//\s+(\w+)\s+s(\d+)\s+1', t)
    names[idx] = (' '.join(tag), ','.join(f'{n}@s{s}' for n, s in m)[:160])
draws = []
cur = None
for line in (cap / 'frame.tsv').read_text(errors='replace').splitlines():
    p = line.split('\t')
    if p[0] == 'DRAW':
        cur = dict(i=int(p[1]), kind=p[2], info=' '.join(x for x in p[3:] if not x.startswith('constants')), rt=[], ds='', tex=[], state={})
        draws.append(cur)
    elif cur is not None:
        if p[0] == 'RT': cur['rt'].append(f"rt{p[1]}:{p[2]} {p[4]} f{p[5]}")
        elif p[0] == 'DS': cur['ds'] = f"{p[2]} {p[4]}"
        elif p[0] in ('TEX', 'TEXTURE', 'SAMPLER'): cur['tex'].append(' '.join(p[1:4]))
        elif p[0] == 'STATE': cur['state'][int(p[1])] = p[2]
kinds = sorted({l.split('\t')[0] for l in (cap / 'frame.tsv').read_text(errors='replace').splitlines()})
print('record kinds:', kinds)
water = [d for d in draws if 'HeightScale' in names.get(int(re.search(r'ps=(\d+)', d['info']).group(1)), ('', ''))[0]]
print('draws', len(draws), 'water draws', [(d['i'], d['info'][:60], names[int(re.search(r'ps=(\d+)', d['info']).group(1))][0], 'blend', d['state'].get(27), 'zw', d['state'].get(14)) for d in water])
if len(sys.argv) > 2: sys.exit(0)
show = set()
for w in water:
    show.update(range(w['i'] - 3, w['i'] + 4))
prev_ps = None
for d in draws:
    ps = int(re.search(r'ps=(\d+)', d['info']).group(1))
    if d['i'] in show or ps != prev_ps:
        st = d['state']
        print(d['i'], d['info'][:70], '|', ';'.join(d['rt']), '| ds', d['ds'], '| zen', st.get(7), 'zw', st.get(14), 'blend', st.get(27), 'cw', st.get(168), '|', names.get(ps, ('', ''))[0], names.get(ps, ('', ''))[1][:90])
    prev_ps = ps

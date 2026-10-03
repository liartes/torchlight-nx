# Liste les trous entre les fonctions générées (fin calculée = début + 4 x nb d'instructions).
# Usage : python3 tools/find_gaps.py <dossier de travail>  -> écrit <dossier>/gaps.json
import re, glob, json, sys
OUT = sys.argv[1]
funcs = {}
for f in glob.glob('generated/default/torchlight_recomp.*.cpp'):
    cur = None; n = 0
    for line in open(f, errors='ignore'):
        m = re.match(r'DEFINE_REX_FUNC\((?:__imp__)?sub_([0-9A-F]{8})\)', line)
        if m:
            if cur: funcs[cur] = n
            cur = int(m.group(1), 16); n = 0; continue
        if line.startswith('DEFINE_REX_FUNC'):
            if cur: funcs[cur] = n
            cur = None; continue
        if cur is not None and re.match(r'\s*// ', line) and 'ERROR' not in line:
            n += 1
    if cur: funcs[cur] = n
addrs = sorted(funcs)
gaps = []
for a, b in zip(addrs, addrs[1:]):
    end = a + 4*funcs[a]
    if end < b:
        gaps.append((a, end, b))
    elif end > b:
        pass
json.dump(gaps, open(OUT + '/gaps.json', 'w'))
json.dump(addrs, open(OUT + '/known.json', 'w'))
print(len(funcs), 'fonctions', len(gaps), 'trous')
sizes = [b-e for _, e, b in gaps]
import collections
print('trous par taille (octets):', sorted(collections.Counter(min(s, 256) for s in sizes).items())[:20])
print('exemples:', ['%08X-%08X' % (e, b) for _, e, b in gaps[:8]])

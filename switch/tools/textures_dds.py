# Usage : python3 -I switch/tools/textures_dds.py <dossier de capture> assets/pak.zip
# Compare chaque texture DXT capturée (niveau 0 détilé) au niveau 0 du DDS de pak.zip du même nom.
import sys, zipfile, os, struct
cap, pak = sys.argv[1], sys.argv[2]
z = zipfile.ZipFile(pak)
by_name = {n.lower(): n for n in z.namelist()}
seen = set()
ok = bad = 0
for line in open(os.path.join(cap, 'frame.txt')):
    p = line.split()
    if not p or p[0] != 'tex' or len(p) < 12: continue
    f, fmt, w, h, bpb, tiled, packed, name = p[2], int(p[3]), int(p[4]), int(p[5]), int(p[8]), p[9], p[10], ' '.join(p[11:])
    if fmt not in (18, 19, 20) or (f, name) in seen: continue
    seen.add((f, name))
    n = by_name.get(name.lower())
    if not n: continue
    d = z.read(n)
    if d[84:88] not in (b'DXT1', b'DXT3', b'DXT5'): continue
    size = max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * bpb
    lvl0 = d[128:128 + size]
    b = open(os.path.join(cap, f), 'rb').read()
    same = b[:size] == lvl0
    small = min(w, h) <= 16
    if same: ok += 1
    else: bad += 1
    if small or not same:
        print('%-4s %3dx%-3d %s %s %s' % ('OK' if same else 'DIFF', w, h, 'tuilé' if tiled == '1' else 'linéaire', packed, name))
print('identiques', ok, 'différentes', bad)

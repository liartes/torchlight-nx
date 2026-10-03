# profil_noms.py vtables.txt < texte : ajoute à chaque sub_X ses cases de tables virtuelles (Classe[case]), depuis
# /tmp/image.bin (image mémoire du jeu) et la sortie de tools/rtti_vtables.py
import re, struct, sys, collections
img = open('/tmp/image.bin','rb').read(); BASE=0x82000000
names = collections.defaultdict(list)
for line in open(sys.argv[1]):
    m = re.match(r'\.\?A[UV](\S+)\s+\(décalage (0x[0-9a-f]+)\)\s+vtable (0x[0-9a-f]+)\s+(\d+)', line)
    if not m: continue
    cls = m.group(1).split('@@')[0].split('@'); cls = '::'.join(reversed([c for c in cls if c]))
    vt = int(m.group(3),16); n = int(m.group(4))
    for i in range(n):
        f = struct.unpack('>I', img[vt-BASE+4*i: vt-BASE+4*i+4])[0]
        names[f'sub_{f:08X}'].append(f'{cls}[{i}]')
for l in sys.stdin:
    l = l.rstrip('\n'); m = re.search(r'sub_[0-9A-F]{8}', l)
    extra = ' '.join(names.get(m.group(0), [])[:4]) if m else ''
    print(l, ' ', extra)

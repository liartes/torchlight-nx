#!/usr/bin/env python3
# profil_descente.py piles.txt racine [seuil %] [profondeur] : arbre d'appels descendant depuis une fonction
# (temps inclusif en % des échantillons), branches sous le seuil omises.
import sys, re, collections
def norm(s):
    s = s.strip().split(' ', 1)[1] if ' ' in s.strip() else s.strip()
    return re.sub(r'\(.*', '', s.replace('__imp__', '').replace('(anonymous namespace)', 'anon'))
stacks = [[norm(l) for l in b.strip().split('\n')][::-1] for b in open(sys.argv[1]).read().split('\n\n') if b.strip()]
root = sys.argv[2]; seuil = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0
prof = int(sys.argv[4]) if len(sys.argv) > 4 else 6
N = len(stacks)
tree = collections.Counter()
for st in stacks:
    if root not in st: continue
    i = st.index(root)
    path = tuple(st[i:i + prof + 1])
    seen = set()
    for d in range(1, len(path) + 1):
        tree[path[:d]] += 1
def show(prefix):
    kids = sorted([(c, p) for p, c in tree.items() if len(p) == len(prefix) + 1 and p[:len(prefix)] == prefix], reverse=True)
    for c, p in kids:
        if 100 * c / N < seuil: continue
        print('  ' * (len(p) - 1) + f'{100*c/N:5.1f}% {p[-1]}')
        show(p)
print(f'{N} échantillons')
show(())

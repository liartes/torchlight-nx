#!/usr/bin/env python3
# profil_arbre.py piles.txt [N | fonction [profondeur]] : temps inclusif par fonction (thread principal), et sous-arbre d'une fonction.
import sys, collections, re
def norm(s):
    s = s.strip().split(' ', 1)[1] if ' ' in s.strip() else s.strip()
    s = s.replace('__imp__', '')
    s = re.sub(r'\(.*', '', s.replace('(anonymous namespace)', 'anon'))
    return s
stacks = []
cur = []
for line in open(sys.argv[1]):
    if not line.strip():
        if cur: stacks.append(cur)
        cur = []
    else:
        cur.append(norm(line))
if cur: stacks.append(cur)
N = len(stacks)
print(f"{N} échantillons")
incl = collections.Counter(); selfc = collections.Counter()
for st in stacks:
    selfc[st[0]] += 1
    for f in set(st): incl[f] += 1
if len(sys.argv) < 3 or sys.argv[2].isdigit():
    for f, c in incl.most_common(int(sys.argv[2]) if len(sys.argv) > 2 else 60):
        print(f"{100*c/N:6.2f}% incl {100*selfc[f]/N:6.2f}% propre  {f}")
    sys.exit()
target = sys.argv[2]; depth = int(sys.argv[3]) if len(sys.argv) > 3 else 2
# sous-arbre : enfants directs (feuille vers racine : st[i-1] appelé par st[i])
tree = collections.Counter(); tot = 0
for st in stacks:
    if target not in st: continue
    i = max(k for k, f in enumerate(st) if f == target)  # appel le plus externe
    tot += 1
    path = tuple(reversed(st[max(0, i - depth):i]))
    for d in range(1, len(path) + 1): tree[path[:d]] += 1
    if i == 0: tree[('(propre)',)] += 1
print(f"{target} : {100*tot/N:.2f}% du thread")
for path, c in sorted(tree.items(), key=lambda kv: (kv[0][:1] and -tree[kv[0][:1]], kv[0])):
    if 100 * c / N >= 0.3:
        print("  " * len(path) + f"{100*c/N:5.2f}% {path[-1]}")

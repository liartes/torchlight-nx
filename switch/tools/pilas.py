#!/usr/bin/env python3
"""Échantillons du profileur de la console (logs/rex/rex_pilas.txt) -> piles symbolisées, même format que
`perf script -F ip,sym` (un bloc par échantillon, feuille d'abord), pour switch/tools/profil_arbre.py.

Usage : pilas.py rex_pilas.txt <ELF du .nro testé (switch/elf)> sortie.txt [thread [fonction]]
Chaque ligne : « thread|pc retour retour... », adresses relatives à l'image (l'ELF est lié à 0). Sans thread,
liste les threads et leur nombre d'échantillons. Les threads du jeu (XThread) n'ont pas de nom : thread « - », et
fonction pour ne garder que les piles qui la contiennent (thread principal du jeu : sub_821F8818, sa boucle).
"""
import collections, re, subprocess, sys

if len(sys.argv) < 4:
    raise SystemExit(__doc__)
src, elf, out = sys.argv[1:4]
want = sys.argv[4] if len(sys.argv) > 4 else None
keep = sys.argv[5] if len(sys.argv) > 5 else None
if want == '-':
    want = ''
samples = []
for line in open(src, errors='ignore'):
    if '|' not in line:
        continue
    thread, rest = line.rstrip('\n').split('|', 1)
    addrs = [int(a, 16) for a in re.findall(r'(?:0x)?([0-9a-fA-F]+)', rest)]
    if addrs:
        samples.append((thread.strip(), addrs))
counts = collections.Counter(t for t, _ in samples)
if want is None:
    for t, n in counts.most_common():
        print(f'{n:7d}  {t}')
    raise SystemExit(0)
samples = [a for t, a in samples if t == want]
# Adresses de retour : -4 pour tomber dans l'instruction d'appel (sinon la fonction suivante en fin de fonction).
uniq = sorted({a if i == 0 else a - 4 for s in samples for i, a in enumerate(s)})
res = subprocess.run(['llvm-symbolizer-20', '--obj=' + elf, '--functions=linkage', '--no-inlines', '--demangle'],
                     input='\n'.join(hex(a) for a in uniq), capture_output=True, text=True).stdout
names = {}
for a, block in zip(uniq, res.strip('\n').split('\n\n')):
    names[a] = block.split('\n')[0]
kept = 0
with open(out, 'w') as f:
    for s in samples:
        if keep and not any(keep in names.get(a if i == 0 else a - 4, '') for i, a in enumerate(s)):
            continue
        kept += 1
        for i, a in enumerate(s):
            k = a if i == 0 else a - 4
            f.write(f'\t{a:x} {names.get(k, "?")}\n')
        f.write('\n')
print(f'{kept} échantillons gardés sur {len(samples)} du thread « {want} » -> {out}')

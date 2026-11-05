#!/usr/bin/env python3
"""Remet en sub_X les appels figés __imp__sub_X(ctx, base); du code généré, pour toutes les fonctions interceptées dans src/.

appels_directs.py fige les appels des fonctions non interceptées : une native ajoutée ne verrait pas ces appelants. Idempotent. À
lancer après avoir ajouté une native et après toute régénération du code (le code généré n'est pas versionné).

  python3 switch/tools/appels_figes.py [--verifier]
"""
import glob
import os
import re
import sys

ICI = os.path.dirname(os.path.abspath(__file__))
RACINE = os.path.join(ICI, '..', '..')
G = os.path.join(ICI, '..', 'generated', 'default')


def crochets():
    adresses = set()
    for f in glob.glob(os.path.join(RACINE, 'src', '*.cpp')) + glob.glob(os.path.join(RACINE, 'src', 'natif', '*.cpp')):
        t = open(f, encoding='utf-8', errors='replace').read()
        adresses |= set(re.findall(r'REX_HOOK_RAW\(sub_([0-9A-F]{8})\)', t))
        adresses |= set(re.findall(r'^TL_[A-Z_0-9]+\(([0-9A-F]{8})', t, re.M))
    return adresses


def main():
    verifier = '--verifier' in sys.argv[1:]
    adresses = crochets()
    motif = re.compile(r'__imp__sub_([0-9A-F]{8})\(ctx, base\);')
    total = fichiers = 0
    for chemin in sorted(glob.glob(os.path.join(G, 'torchlight_recomp.*.cpp'))):
        texte = open(chemin, encoding='utf-8').read()
        compte = [0]

        def remplacer(m):
            if m.group(1) in adresses:
                compte[0] += 1
                return 'sub_%s(ctx, base);' % m.group(1)
            return m.group(0)

        neuf = motif.sub(remplacer, texte)
        if compte[0]:
            total += compte[0]
            fichiers += 1
            if not verifier:
                with open(chemin, 'w', encoding='utf-8') as f:
                    f.write(neuf)
    print('appels_figes : %d crochets, %d appels %s dans %d fichiers' %
          (len(adresses), total, 'à remettre' if verifier else 'remis', fichiers))
    return 1 if verifier and total else 0


if __name__ == '__main__':
    sys.exit(main())

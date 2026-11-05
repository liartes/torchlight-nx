#!/usr/bin/env python3
"""Temps absolus par scène et par état des lignes « [coeurs] » d'un journal de la console (src/torchlight_coeurs.cpp).

À regarder en plus de ab_regression.py, surtout après un changement de profil PGO : une différence dans la session peut
venir d'un côté qui ralentit (session du 2026-11-03 : le recompilé perdait ~3 % avec le profil de l'alternance). Sert aussi
sans alternance, pour comparer deux builds de session à session, aux mêmes endroits.

Usage : ab_absolu.py <journal> nom=HH:MM:SS-HH:MM:SS [nom=...]
Moyennes pondérées par le nombre d'images ; fenêtres de chargement écartées (écart maximal >= 250 ms). « reste » : écart
entre images moins temps de l'image, soit surtout la logique du jeu.
"""
import re
import sys

LIGNE = re.compile(r"(\d\d):(\d\d):(\d\d)\.\d+\].*\[coeurs\] etat=(\S+) .*images=(\d+) dessins=(\d+) image=([\d.]+) "
                   r"attente=([\d.]+) ecart: moyen=([\d.]+) p50=([\d.]+) p99=([\d.]+) max=([\d.]+) sup33=(\d+)")


def secondes(texte):
    h, m, s = map(int, texte.split(":"))
    return h * 3600 + m * 60 + s


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    fenetres = []
    for ligne in open(sys.argv[1], errors="replace"):
        m = LIGNE.search(ligne)
        if m:
            g = m.groups()
            fenetres.append(dict(t=int(g[0]) * 3600 + int(g[1]) * 60 + int(g[2]), etat=g[3], images=int(g[4]),
                                 dessins=int(g[5]), image=float(g[6]), ecart=float(g[8]), maxi=float(g[11]),
                                 sup33=int(g[12])))
    print(f"{'scène':<9} {'état':<24} {'fen':>4} {'images':>7} {'dessins':>8} {'écart':>7} {'image':>7} {'reste':>6} "
          f"{'images/s':>9} {'>33 ms':>8}")
    for arg in sys.argv[2:]:
        nom, plage = arg.split("=", 1)
        debut, fin = map(secondes, plage.split("-"))
        groupes = {}
        for f in fenetres:
            if debut <= f["t"] <= fin and f["maxi"] < 250:
                groupes.setdefault(f["etat"], []).append(f)
        for etat in sorted(groupes):
            x = groupes[etat]
            n = sum(f["images"] for f in x)
            moy = lambda c: sum(f[c] * f["images"] for f in x) / n
            print(f"{nom:<9} {etat:<24} {len(x):>4} {n:>7} {moy('dessins'):>8.0f} {moy('ecart'):>7.2f} {moy('image'):>7.2f} "
                  f"{moy('ecart') - moy('image'):>6.2f} {1000 / moy('ecart'):>9.1f} {100 * sum(f['sup33'] for f in x) / n:>7.1f}%")


if __name__ == "__main__":
    main()

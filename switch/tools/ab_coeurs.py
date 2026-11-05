#!/usr/bin/env python3
"""Analyse des essais en alternance (src/torchlight_coeurs.cpp) : lignes « [coeurs] » d'un journal de la console.

Usage : ab_coeurs.py <torchlight_001.log> [seuils de dessins, défaut 215 270]

Les fenêtres voisines (7 s) ne diffèrent que d'un réglage : chaque paire compare ce réglage au même endroit, et la
médiane des rapports résiste aux déplacements du personnage. Fenêtres ignorées : chargements (écart maximal >= 250 ms,
mode boost), et paires dont le nombre de dessins diffère de plus de 5 % (changement de scène). Scènes par nombre de
dessins (mine, ville, combat par défaut). Journaux plus anciens (sans « etat= ») : réglage « coeur », d'après reserve=.
"""
import re
import statistics
import sys

MOTIF = re.compile(
    r"\[coeurs\] (?:etat=(\S+) )?(?:masque=(0x[0-9a-f]+) autres=(\d+) )?reserve=(-?\d+) images=(\d+) dessins=(\d+) "
    r"image=([\d.]+) attente=([\d.]+) ecart: moyen=([\d.]+) p50=([\d.]+) p99=([\d.]+) max=([\d.]+) sup33=(\d+) "
    r"sup50=(\d+)"
)
CHAMPS = ["image", "attente", "moyen", "p50", "p99", "max"]


def lire(chemin):
    fenetres = []
    with open(chemin, errors="replace") as f:
        for ligne in f:
            m = MOTIF.search(ligne)
            if not m:
                continue
            v = m.groups()
            etat = {}
            if v[0] and v[0] != "-":
                etat = {k: int(x) for k, x in (e.split(":") for e in v[0].split(","))}
            elif not v[0]:
                etat = {"coeur": 1 if int(v[3]) >= 0 else 0}
            fenetres.append(dict(etat=etat, masque=v[1], autres=int(v[2] or 0), reserve=int(v[3]), images=int(v[4]),
                                 dessins=int(v[5]), image=float(v[6]), attente=float(v[7]), moyen=float(v[8]),
                                 p50=float(v[9]), p99=float(v[10]), max=float(v[11]), sup33=int(v[12]),
                                 sup50=int(v[13]), rang=len(fenetres)))
    return fenetres


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    fenetres = lire(sys.argv[1])
    seuils = [int(x) for x in sys.argv[2:]] or [215, 270]
    noms = ["< %d dessins" % seuils[0]]
    noms += ["%d-%d dessins" % (a, b) for a, b in zip(seuils, seuils[1:])]
    noms.append(">= %d dessins" % seuils[-1])

    masques = {}
    for f in fenetres:
        if f["masque"]:
            cle = ("réservé" if f["reserve"] >= 0 else "sans", f["masque"], f["autres"])
            masques[cle] = masques.get(cle, 0) + 1
    for (mode, masque, autres), n in sorted(masques.items()):
        print("masques, %-8s : thread principal %s, %d autres threads sur ce cœur : %d fenêtres" %
              (mode, masque, autres, n))

    bonnes = [f for f in fenetres if f["max"] < 250]
    print("%d fenêtres, %d hors chargements" % (len(fenetres), len(bonnes)))

    def scene(dessins):
        return sum(dessins >= s for s in seuils)

    paires = {}
    for a, b in zip(bonnes, bonnes[1:]):
        if b["rang"] != a["rang"] + 1 or a["etat"].keys() != b["etat"].keys():
            continue
        changes = [k for k in a["etat"] if a["etat"][k] != b["etat"][k]]
        if len(changes) != 1:
            continue
        if abs(a["dessins"] - b["dessins"]) > 0.05 * max(a["dessins"], b["dessins"]):
            continue
        k = changes[0]
        avec, sans = (a, b) if a["etat"][k] else (b, a)
        paires.setdefault((k, scene((a["dessins"] + b["dessins"]) // 2)), []).append((avec, sans))

    for k, i in sorted(paires):
        p = paires[(k, i)]
        print("\n%s, %s : %d paires" % (k, noms[i], len(p)))
        print("  %-8s %10s %10s %14s" % ("", "sans", "avec", "écart médian"))
        for c in CHAMPS:
            sans = statistics.mean(s[c] for _, s in p)
            avec = statistics.mean(a[c] for a, _ in p)
            rapports = [a[c] / s[c] - 1 for a, s in p if s[c] > 0]
            print("  %-8s %10.2f %10.2f %+13.2f%%" % (c, sans, avec, 100 * statistics.median(rapports)))
        for c in ["sup33", "sup50"]:
            print("  %-8s %10d %10d" % (c, sum(s[c] for _, s in p), sum(a[c] for a, _ in p)))
        rapports = [a["moyen"] / s["moyen"] - 1 for a, s in p]
        print("  paires où le réglage est plus rapide : %d sur %d" % (sum(r < 0 for r in rapports), len(p)))


if __name__ == "__main__":
    main()

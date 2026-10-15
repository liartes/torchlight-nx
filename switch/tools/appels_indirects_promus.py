#!/usr/bin/env python3
"""Écrit switch/appels_indirects_promus.txt (la liste qu'applique switch/tools/appels_indirects.py --promouvoir) à
partir d'un relevé PC des appels indirects. D'après tools/indirect_promotions.py de sa2-nx (GPL-3.0).

Le relevé est le fichier qu'écrit une partie sur PC avec torchlight_releve_appels_indirects = <fichier>
(src/torchlight_appels_indirects.cpp) : une ligne par site, `<fonction> <site> <appels> <cible>:<appels> ...
other:<appels>`. Un site est promu quand
  - il pèse au moins --part-min de tous les appels indirects de la partie (défaut 0,02 % : une garde sur un site
    qui tourne à peine ne rapporte rien et coûte une comparaison) ;
  - ses cibles les plus fréquentes le couvrent : une cible avec au moins --unique-min de ses appels (défaut 99,5 %),
    ou les deux ou trois premières ensemble (chacune au moins --suivante-min du site, défaut 5 %), ou à défaut une
    cible avec au moins --dominante-min (défaut 70 %) ;
  - ces cibles sont des fonctions du code généré (un appel vers un autre module ou un thunk du noyau reste indirect).
La garde compare la cible : un site qui va ailleurs dans une scène absente du relevé reste juste (ancien chemin,
une comparaison ou deux plus tard). La liste doit donc être bonne, pas complète.

  python3 switch/tools/appels_indirects_promus.py --releve <fichier> [<fichier>...] [--sortie <liste>]
Plusieurs relevés s'additionnent (ville, mine, menus...).
"""
import argparse
import os
import re
import sys

RACINE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GEN = os.path.join(RACINE, 'switch', 'generated', 'default')
FUNC = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\) \{$', re.M)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--releve', required=True, nargs='+')
    ap.add_argument('--gen', default=GEN)
    ap.add_argument('--sortie', default=os.path.join(RACINE, 'switch', 'appels_indirects_promus.txt'))
    ap.add_argument('--part-min', type=float, default=0.02, help='pourcentage de tous les appels indirects')
    ap.add_argument('--unique-min', type=float, default=99.5, help='part du site que ses cibles doivent couvrir')
    ap.add_argument('--suivante-min', type=float, default=5.0, help='part du site d\'une 2e ou 3e cible')
    ap.add_argument('--dominante-min', type=float, default=70.0, help='part d\'une cible gardée seule')
    a = ap.parse_args()
    definies = set()
    for f in os.listdir(a.gen):
        if re.match(r'\w+_recomp\.\d+\.cpp$', f):
            definies.update(FUNC.findall(open(os.path.join(a.gen, f), encoding='utf-8').read()))
    sites = {}  # (fonction, site) -> [total, {cible: appels}]
    for chemin in a.releve:
        for ligne in open(chemin, encoding='utf-8'):
            if ligne.startswith('#') or not ligne.strip():
                continue
            p = ligne.split()
            cle = (p[0].upper(), int(p[1]))
            s = sites.setdefault(cle, [0, {}])
            s[0] += int(p[2])
            for x in p[3:]:
                nom, _, n = x.partition(':')
                if nom != 'other':
                    s[1][nom.upper()] = s[1].get(nom.upper(), 0) + int(n)
    total = sum(s[0] for s in sites.values())
    if not total:
        sys.exit('appels_indirects_promus : le relevé ne contient aucun appel')
    lignes = []  # (appels, clé, [cibles], appels pris par les gardes)
    etrangers = froids = melanges = multiples = dominants = 0
    for cle, (appels, cibles) in sites.items():
        rang = sorted(cibles.items(), key=lambda kv: -kv[1])
        cible, haut = rang[0]
        choix, pris = None, 0
        if 100.0 * haut >= a.unique_min * appels:
            choix, pris = [cible], haut
        else:
            for n in (2, 3):
                part = rang[:n]
                if len(part) == n and 100.0 * sum(c for _, c in part) >= a.unique_min * appels and \
                        all(100.0 * c >= a.suivante_min * appels for _, c in part[1:]):
                    choix, pris = [t for t, _ in part], sum(c for _, c in part)
                    break
            if choix is None and 100.0 * haut >= a.dominante_min * appels:
                choix, pris = [cible], haut
        if choix is None:
            melanges += appels
            continue
        if any(t not in definies for t in choix):
            etrangers += appels
            continue
        if 100.0 * appels < a.part_min * total:
            froids += appels
            continue
        multiples += len(choix) > 1
        dominants += len(choix) == 1 and 100.0 * haut < a.unique_min * appels
        lignes.append((appels, cle, choix, pris))
    promus = sum(r[3] for r in lignes)
    texte = ['# Sites d\'appels indirects qui vont presque toujours au même endroit : switch/tools/appels_indirects.py',
             '# --promouvoir en fait des appels directs gardés. Écrit par switch/tools/appels_indirects_promus.py à',
             '# partir d\'un relevé PC (torchlight_releve_appels_indirects) ; la garde compare la cible, la liste',
             '# doit être bonne, pas complète.',
             '# Relevé : %d sites, %d appels. Promus : %d sites dont les gardes prennent %.1f %% des appels (%d à deux'
             % (len(sites), total, len(lignes), 100.0 * promus / total, multiples),
             '# ou trois cibles, %d à cible dominante d\'au moins %.0f %%). Non promus : %.1f %% des appels dans des'
             % (dominants, a.dominante_min, 100.0 * melanges / total),
             '# sites à cibles nombreuses, %.1f %% vers des cibles hors du code généré, %.1f %% dans des sites sous'
             % (100.0 * etrangers / total, 100.0 * froids / total),
             '# %.2f %% des appels.' % a.part_min,
             '# <fonction> <site> <cible> [<cible2> [<cible3>]]   # appels dans le relevé, part prise par les gardes']
    for appels, (fonction, site), choix, pris in sorted(lignes, key=lambda r: r[1]):
        texte.append('%s %d %s   # %d, %.2f %%' % (fonction, site, ' '.join(choix), appels, 100.0 * pris / appels))
    open(a.sortie, 'w', encoding='utf-8', newline='\n').write('\n'.join(texte) + '\n')
    print('appels_indirects_promus : %d sites et %d appels dans le relevé ; %d sites promus (%d à deux ou trois cibles, '
          '%d à cible dominante) dont les gardes prennent %.1f %% des appels -> %s' %
          (len(sites), total, len(lignes), multiples, dominants, 100.0 * promus / total, a.sortie))
    for appels, (fonction, site), choix, pris in sorted(lignes, key=lambda r: -r[0])[:12]:
        print('  %s site %d -> %s : %d appels (%.2f %% du total), %.2f %% pris' %
              (fonction, site, ' '.join(choix), appels, 100.0 * appels / total, 100.0 * pris / appels))


if __name__ == '__main__':
    main()

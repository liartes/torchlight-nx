#!/usr/bin/env python3
"""Range dans switch/ordre_fonctions.ld les natives du thread principal que la liste ne connaît pas.

La liste vient de relevés de piles plus anciens que ces natives (switch/tools/ordre_fonctions.py) : une native absente
reste loin du code chaud (sub_821C0E50 à 42 Mo, essai du 2026-11-03), ce qui fausse un A/B contre le recompilé. Chaque
native absente est ajoutée avec son original, côte à côte, dans un bloc juste après les fonctions de libc en assembleur.

Idempotent : le bloc précédent est remplacé. À relancer après ordre_fonctions.py ou après l'ajout d'une native. Ne touche
qu'à l'édition des liens : le profil PGO reste valable.

  python3 switch/tools/ordre_natives.py
"""
import os
import re

ICI = os.path.dirname(os.path.abspath(__file__))
RACINE = os.path.join(ICI, '..', '..')
ORDRE = os.path.join(ICI, '..', 'ordre_fonctions.ld')
SOURCE = os.path.join(RACINE, 'src', 'torchlight_ogre_natif.cpp')
DEBUT = '  /* Natives absentes de la liste (switch/tools/ordre_natives.py). */'
FIN = '  /* Fin des natives. */'


def main():
    texte = open(SOURCE, encoding='utf-8').read()
    crochets = re.findall(r'REX_HOOK_RAW\((sub_[0-9A-F]+)\)', texte[texte.index('// Top 50 du thread'):])
    lignes = open(ORDRE, encoding='utf-8').read().split('\n')
    if DEBUT in lignes:
        lignes = lignes[:lignes.index(DEBUT)] + lignes[lignes.index(FIN) + 1:]
    connues = set(re.findall(r'\*\(\.text\.(\S+)', '\n'.join(lignes)))
    absentes = list(dict.fromkeys(c for c in crochets if c not in connues))
    bloc = [DEBUT]
    for nom in absentes:
        bloc.append(f'  *(.text.{nom} .text.hot.{nom})')
        bloc.append(f'  *(.text.__imp__{nom} .text.hot.__imp__{nom})')
    bloc.append(FIN)
    # Après la dernière fonction de libc en assembleur : _start doit rester à l'adresse 0.
    apres = max(i for i, l in enumerate(lignes) if l.strip().startswith('*libc.a:'))
    lignes[apres + 1:apres + 1] = bloc
    open(ORDRE, 'w', encoding='utf-8').write('\n'.join(lignes))
    print(f'{len(crochets)} natives, {len(absentes)} ajoutées : ' + ' '.join(absentes))


if __name__ == '__main__':
    main()

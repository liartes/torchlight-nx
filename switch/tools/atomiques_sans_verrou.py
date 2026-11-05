#!/usr/bin/env python3
"""Opérations atomiques du code généré sans le verrou global.

Le jeu coupe les interruptions autour de chaque lwarx/stwcx. :
    mfmsr r10 ; mtmsrd r13,1 ; lwarx ... ; stwcx. ... ; mtmsrd r10,1
Le générateur traduit cela par un verrou global de l'hôte (std::recursive_mutex) : mfmsr le prend et le rend une fois
(REX_CHECK_GLOBAL_LOCK), mtmsrd r13 le prend (REX_ENTER_GLOBAL_LOCK), l'autre mtmsrd le rend, avec deux barrières et deux
compteurs atomiques en plus. Or stwcx. est déjà un vrai compare-and-swap de l'hôte, et le verrou ne protège rien d'autre :
ce script vérifie que chaque section ne contient que lwarx/ldarx, stwcx./stdcx. et des opérations sur les registres (ni
autre accès à la mémoire, ni appel), et refuse de toucher au code sinon.

Relevé sur la console (2026-11-04) : 2,2 % du thread principal dans ce verrou en moyenne, 6,4 % des chargements de données
rien que sous le malloc du jeu, et les threads de travail s'y disputent avec le thread principal.

Les lignes sont vidées sur place, sans en retirer : les numéros de ligne ne bougent pas, et le profil PGO reste valable pour
toutes les autres fonctions du fichier (les fonctions touchées perdent le leur : leur graphe change). mfmsr rend la valeur
que REX_CHECK_GLOBAL_LOCK rendait hors de toute section (0x8000).

À refaire après chaque génération du code (build_switch.sh le fait) ; idempotent. Pour revenir au code d'origine : régénérer.
Usage : atomiques_sans_verrou.py [--compter]
"""
import glob
import os
import re
import sys

RACINE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GEN = os.path.join(RACINE, 'switch', 'generated', 'default')

BARRIERE = 'std::atomic_thread_fence(std::memory_order_seq_cst);'
CHECK = re.compile(r'^(\s*)((?:ctx\.|l_|ll_)?r\d+)\.u64 = REX_CHECK_GLOBAL_LOCK\(\);$')
MSR = re.compile(r'^\s*ctx\.msr = \(.*\.u32 & 0x8020\) \| \(ctx\.msr & ~0x8020\);$')
# Ce qu'une section a le droit de contenir.
PURES = {'lwarx', 'ldarx', 'stwcx.', 'stdcx.', 'mr', 'add', 'addi', 'subf', 'and', 'or', 'andc', 'ori', 'xori', 'li',
         'rlwinm', 'clrlwi', 'cmpw', 'cmpwi', 'cmplw', 'cmplwi', 'cmpd', 'cmpdi', 'cmpld', 'cmpldi', 'bne', 'beq', 'nop'}


def traiter(chemin, compter):
    with open(chemin, encoding='utf-8', errors='surrogateescape') as f:
        lignes = f.read().split('\n')
    sections = sorties = lectures = 0
    i = 0
    while i < len(lignes):
        s = lignes[i].strip()
        if s.startswith('// mfmsr '):
            if lignes[i + 1] == '' and lignes[i + 2].endswith('.u64 = 0x8000;'):  # déjà traité
                i += 1
                continue
            if lignes[i + 1].strip() != BARRIERE or not CHECK.match(lignes[i + 2]):
                sys.exit('%s:%d : forme de mfmsr inconnue' % (chemin, i + 1))
            m = CHECK.match(lignes[i + 2])
            lignes[i + 1] = ''
            lignes[i + 2] = '%s%s.u64 = 0x8000;' % (m.group(1), m.group(2))
            lectures += 1
        elif s.startswith('// mtmsrd '):
            if lignes[i + 1] == lignes[i + 2] == lignes[i + 3] == '':  # déjà traité
                i += 4
                continue
            entree = s.startswith('// mtmsrd r13,')
            macro = 'REX_ENTER_GLOBAL_LOCK();' if entree else 'REX_LEAVE_GLOBAL_LOCK();'
            if lignes[i + 1].strip() != BARRIERE or not MSR.match(lignes[i + 2]) or lignes[i + 3].strip() != macro:
                sys.exit('%s:%d : forme de mtmsrd inconnue' % (chemin, i + 1))
            if entree:
                # Section linéaire jusqu'au mtmsrd de sortie : rien d'autre que l'opération atomique.
                j = i + 4
                vus = 0
                while not lignes[j].strip().startswith('// mtmsrd '):
                    t = lignes[j].strip()
                    if t.startswith('// '):
                        op = t[3:].split()[0]
                        if op not in PURES:
                            sys.exit('%s:%d : « %s » dans une section : verrou gardé' % (chemin, j + 1, op))
                        vus += op in ('stwcx.', 'stdcx.')
                    j += 1
                if not vus:
                    sys.exit('%s:%d : section sans stwcx. : verrou gardé' % (chemin, i + 1))
                sections += 1
            else:
                sorties += 1
            lignes[i + 1] = lignes[i + 2] = lignes[i + 3] = ''
            i += 3
        i += 1
    if sections or sorties or lectures:
        if not compter:
            with open(chemin, 'w', encoding='utf-8', errors='surrogateescape') as f:
                f.write('\n'.join(lignes))
    return sections, sorties, lectures


def main():
    compter = '--compter' in sys.argv
    total = [0, 0, 0]
    fichiers = 0
    for chemin in sorted(glob.glob(os.path.join(GEN, 'torchlight_recomp.*.cpp'))):
        r = traiter(chemin, compter)
        if any(r):
            fichiers += 1
            total = [a + b for a, b in zip(total, r)]
    print('atomiques sans verrou : %d sections, %d sorties, %d lectures du MSR, %d fichiers%s' %
          (total[0], total[1], total[2], fichiers, ' (compte seulement)' if compter else ''))


if __name__ == '__main__':
    main()

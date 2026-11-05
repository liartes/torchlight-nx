#!/usr/bin/env python3
"""Fonctions du code généré dont le profil PGO n'est plus valable.

Le contrôle « 0 coverage mismatch » de la sortie d'une build ne prouve rien pour le code généré : DEFINE_REX_FUNC vient d'un
en-tête système du SDK, et GCC masque les avertissements placés dans une macro d'en-tête système. Une fonction dont le graphe
a changé perd son profil sans un mot. Ce script recompile chaque fichier avec -Wsystem-headers et compte les fonctions
signalées. Il réécrit les objets de la build (mêmes options, sans autre effet que les avertissements) : la prochaine build
refait l'édition des liens.

  OUT=switch/out-joueurs python3 switch/tools/audit_profil.py [--jobs 6] [--sortie liste.txt]
"""
import argparse
import concurrent.futures
import os
import re
import shlex
import subprocess
import sys

RACINE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WS = os.path.dirname(RACINE)
RE_FONCTION = re.compile(r"function '(?:__imp__)?(sub_[0-9A-F]{8})'")


def commandes(out):
    cibles = subprocess.run(['ninja', '-t', 'targets', 'all'], cwd=out, capture_output=True, text=True).stdout
    objets = [l.split(':')[0] for l in cibles.split('\n') if re.search(r'torchlight_recomp\.\d+\.cpp\.obj:', l)]
    if not objets:
        sys.exit('aucun objet du code généré dans ' + out)
    res = []
    for o in objets:
        c = subprocess.run(['ninja', '-t', 'commands', o], cwd=out, capture_output=True, text=True).stdout
        ligne = c.strip().split('\n')[-1]
        res.append((o, ligne.replace(' -Wno-missing-profile', ' -Wsystem-headers -Wmissing-profile')))
    return res


def compiler(out, objet, ligne):
    cmd = ['docker', 'run', '--rm', '--user', '%d:%d' % (os.getuid(), os.getgid()), '-e', 'HOME=/tmp', '-v', '%s:%s' % (WS, WS),
           '-w', out, 'devkitpro/devkita64', 'bash', '-c', ligne]
    r = subprocess.run(['sg', 'docker', '-c', shlex.join(cmd)], capture_output=True, text=True, errors='replace')
    texte = r.stdout + r.stderr
    controle, absents = set(), set()
    for l in texte.split('\n'):
        if 'does not match' in l:
            controle.update(RE_FONCTION.findall(l))
        elif 'not found in profile' in l:
            absents.update(RE_FONCTION.findall(l))
    fichier_absent = 'profile count data file not found' in texte
    return objet, r.returncode, controle, absents, fichier_absent


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--jobs', type=int, default=6)
    ap.add_argument('--sortie')
    a = ap.parse_args()
    out = os.path.abspath(os.environ.get('OUT', os.path.join(RACINE, 'switch', 'out-joueurs')))
    travaux = commandes(out)
    print('%d fichiers' % len(travaux), flush=True)
    controle, absents, sans_fichier, echecs = set(), set(), [], []
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        for objet, code, c, ab, fa in ex.map(lambda t: compiler(out, *t), travaux):
            controle |= c
            absents |= ab
            if fa:
                sans_fichier.append(objet)
            if code:
                echecs.append(objet)
    print('profil perdu (graphe changé) : %d fonctions' % len(controle))
    print('absentes du profil : %d fonctions' % len(absents))
    print('fichiers sans .gcda : %d' % len(sans_fichier))
    print('échecs de compilation : %d' % len(echecs))
    for o in echecs[:5]:
        print('  ', o)
    if a.sortie:
        with open(a.sortie, 'w') as f:
            for n in sorted(controle):
                f.write(n + '\n')


if __name__ == '__main__':
    main()

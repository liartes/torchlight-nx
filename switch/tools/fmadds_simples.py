#!/usr/bin/env python3
"""Multiplications-additions simple précision arrondies une seule fois, comme le Xenon (d'après le port 96 de
renut_nx_ultimate_base, tools/fma_single.py de blur-nx, 2026-11-12).

Le générateur écrit fmadds / fmsubs / fnmadds / fnmsubs sous la forme
    X.f64 = double(float(std::fma(A.f64, B.f64, C.f64)));     (fmsubs : -C ; fnmadds / fnmsubs : -std::fma)
soit une fma en double arrondie de nouveau en simple : deux arrondis, là où le Xenon arrondit une fois le résultat
exact (un écart d'une unité du dernier chiffre dans de rares cas, environ une opération sur quelques centaines de
millions pour des entrées au hasard), et chaque chaîne de calcul simple précision passe par des conversions en double
(un fcvt avant, deux après). Cette étape les écrit
    X.f64 = REX_FMADDS(A.f64, B.f64, C.f64);                   (fnmadds / fnmsubs : (-REX_FMADDS(...)))
REX_FMADDS = RexFmaddsSimple (définie dans torchlight_pch.h, identique à TorchlightFmadds de src/torchlight_fmadds.h,
que les natives utilisent pour rester au bit près du code généré) : une seule fma simple précision quand les trois
entrées sont des float exacts (le résultat du Xenon), la forme double sinon. La négation commute avec l'arrondi.
Le mode flottant à chaque site reste celui du générateur (celui de l'unité scalaire, sans mise à zéro).
-DREX_FMADDS_DOUBLE_ARRONDI à la compilation redonne la forme double partout (essai A/B).

Idempotent. Après : nouveau profil PGO (le code des fonctions touchées change).
  python3 switch/tools/fmadds_simples.py [--gen switch/generated/default] [--verifier] [--annuler]
"""
import argparse
import glob
import os
import re
import sys

OPERANDE = r'(-?[A-Za-z_][\w.]*)'
FORME = re.compile(r'double\(float\((-?)std::fma\(' + OPERANDE + r', ' + OPERANDE + r', ' + OPERANDE + r'\)\)\)')
NOUVELLE = re.compile(r'(\(-)?REX_FMADDS\(' + OPERANDE + r', ' + OPERANDE + r', ' + OPERANDE + r'\)(\))?')

MARQUE = '// torchlight : fmadds arrondies une seule fois (switch/tools/fmadds_simples.py)'
MACROS = MARQUE + '''
#ifndef REX_FMADDS
#include <bit>
#include <cmath>
// Copie de TorchlightFmadds (src/torchlight_fmadds.h) : les deux doivent rester identiques.
[[gnu::always_inline]] static inline bool RexEstFloatExact(double x) noexcept {
  return std::bit_cast<uint64_t>(double(float(x))) == std::bit_cast<uint64_t>(x);
}
[[gnu::always_inline]] static inline double RexFmaddsSimple(double a, double b, double c) noexcept {
#if !defined(REX_FMADDS_DOUBLE_ARRONDI)
  if (__builtin_expect(RexEstFloatExact(a) & RexEstFloatExact(b) & RexEstFloatExact(c), 1))
    return double(__builtin_fmaf(float(a), float(b), float(c)));
#endif
  return double(float(__builtin_fma(a, b, c)));
}
#define REX_FMADDS(a, b, c) RexFmaddsSimple((a), (b), (c))
#endif  // REX_FMADDS
// (fin des macros de switch/tools/fmadds_simples.py)
'''


def reecrire(texte):
    n = [0]

    def rempl(m):
        n[0] += 1
        appel = 'REX_FMADDS(%s, %s, %s)' % (m.group(2), m.group(3), m.group(4))
        return '(-%s)' % appel if m.group(1) else appel

    return FORME.sub(rempl, texte), n[0]


def annuler(texte):
    n = [0]

    def rempl(m):
        if bool(m.group(1)) != bool(m.group(5)):
            return m.group(0)
        n[0] += 1
        return 'double(float(%sstd::fma(%s, %s, %s)))' % ('-' if m.group(1) else '', m.group(2), m.group(3),
                                                          m.group(4))

    return NOUVELLE.sub(rempl, texte), n[0]


def main():
    ici = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', default=os.path.join(ici, '..', 'generated', 'default'))
    ap.add_argument('--verifier', action='store_true', help='compter seulement, ne rien écrire')
    ap.add_argument('--annuler', action='store_true', help='remettre la forme double du générateur')
    a = ap.parse_args()
    pchs = glob.glob(os.path.join(a.gen, '*_pch.h'))
    if len(pchs) != 1:
        sys.exit('ERREUR : fmadds_simples : pas un seul *_pch.h dans %s' % a.gen)
    total = fichiers = deja = 0
    for chemin in sorted(glob.glob(os.path.join(a.gen, '*_recomp.*.cpp'))):
        texte = open(chemin, encoding='utf-8').read()
        nouveau, n = (annuler if a.annuler else reecrire)(texte)
        if not a.annuler:
            deja += len(NOUVELLE.findall(texte))
        if n:
            total += n
            fichiers += 1
            if not a.verifier:
                open(chemin, 'w', encoding='utf-8', newline='\n').write(nouveau)
    texte = open(pchs[0], encoding='utf-8').read()
    if not a.verifier and not a.annuler and MARQUE not in texte:
        ancre = '// torchlight : chargements flottants inversés dans l\'unité vectorielle'
        if ancre not in texte:
            sys.exit('ERREUR : fmadds_simples : ancre « %s » absente de %s' % (ancre, pchs[0]))
        open(pchs[0], 'w', encoding='utf-8', newline='\n').write(texte.replace(ancre, MACROS + ancre, 1))
    print('fmadds_simples%s : %d multiplications-additions simples %s dans %d fichiers%s' % (
        ' (vérification)' if a.verifier else '', total,
        'remises en forme double' if a.annuler else 'arrondies une seule fois', fichiers,
        '' if a.annuler else ' (%d déjà sous cette forme)' % deja))


if __name__ == '__main__':
    main()

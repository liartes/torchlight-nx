#!/usr/bin/env python3
"""Chargements flottants simples (lfs) du jeu inversés dans l'unité vectorielle sur AArch64. D'après
tools/float_loads.py de sa2-nx (https://github.com/ChanseyIsTheBest/sa2-nx, GPL-3.0), adapté au code généré de
Torchlight (accès à la mémoire du jeu par des pointeurs volatile, gardés).

La mémoire du jeu est gros-boutiste : un `lfs` est un chargement 32 bits, une inversion des octets et une conversion
en double. Le générateur l'écrit par un entier :
    temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 172);   ldr w0, [..] ; rev w0, w0 ; fmov s0, w0
    ctx.f13.f64 = double(temp.f32);              fcvt d13, s0
et le passage des registres généraux à l'unité vectorielle (`fmov s0, w0`) est l'étape lente du Cortex-A57 (5 cycles
après les 4 du chargement et le 1 de l'inversion). Chargé directement dans l'unité vectorielle et inversé là :
    ldr s0, [..] ; rev32 v0.8b, v0.8b            5 + 3 cycles, une instruction de moins
Les mêmes quatre octets dans le même ordre, aucune conversion de plus (les bits d'un NaN restent ceux de la mémoire).

Cette étape réécrit les deux lignes en
    ctx.f13.f64 = double(REX_LOAD_F32(ctx.r3.u32 + 172));
Le pch généré définit REX_LOAD_F32 en forme vectorielle sur AArch64 et en forme entière ailleurs (la build PC ne
change pas) ou avec REX_FLOAT_LOADS_INTEGER. La lecture reste volatile, comme REX_LOAD_U32.
Non réécrits : les écritures (stfs : autant d'instructions dans les deux formes), les copies (chargement dont la
valeur est aussitôt réécrite par un stfs : la forme entière réécrit les bits sans passer par l'unité vectorielle),
et les chargements déjà repliés en constantes (switch/tools/constantes_lecture_seule.py, à lancer AVANT).

  python3 switch/tools/chargements_flottants.py [--verifier] [--annuler]
Idempotent.
"""
import argparse
import os
import re
import sys

RACINE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GEN = os.path.join(RACINE, 'switch', 'generated', 'default')
CHARGEMENT = re.compile(r'^\t((?:ctx\.)?temp)\.u32 = REX_LOAD_U32\((.*)\);$')
UTILISATION = re.compile(r'^\t((?:ctx\.)?f\d+)\.f64 = double\(((?:ctx\.)?temp)\.f32\);$')
NOUVEAU = re.compile(r'^\t((?:ctx\.)?f\d+)\.f64 = double\(REX_LOAD_F32\((.*)\)\);$')
MARQUE = '// torchlight : chargements flottants inversés dans l\'unité vectorielle (switch/tools/chargements_flottants.py)'
MACROS = MARQUE + '''
#ifndef REX_LOAD_F32
#if (defined(__aarch64__) || defined(_M_ARM64)) && !defined(REX_FLOAT_LOADS_INTEGER)
#include <arm_neon.h>
// ldr s puis rev32 en assembleur : la lecture reste volatile (asm volatile, comme REX_LOAD_U32) et la moitié haute
// du registre n'est pas remise à zéro (vset_lane_f32 sur un vecteur nul ajoutait un fmov s, s).
[[gnu::always_inline]] static inline float RexFloatFromBigEndian(const void* p) noexcept {
  float32x2_t v;
  asm volatile("ldr %s0, %1\\n\\trev32 %0.8b, %0.8b" : "=w"(v) : "m"(*(const uint32_t*)p));
  return vget_lane_f32(v, 0);
}
#define REX_LOAD_F32(x) RexFloatFromBigEndian(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x))
#else
#define REX_LOAD_F32(x) std::bit_cast<float>(u32(REX_LOAD_U32(x)))
#endif
#endif
'''


def copie(lignes, j, reg):
    """Vrai si le prochain usage du registre flottant chargé est l'écriture de sa valeur (stfs) : une copie. La forme
    entière y est meilleure (le compilateur réécrit les bits chargés sans passer par l'unité vectorielle)."""
    mot = re.compile(r'(?<![\w.])' + re.escape(reg) + r'\.')
    for k in range(j, min(j + 8, len(lignes))):
        l = lignes[k]
        if l.startswith('\t//') or 'fpscr' in l:
            continue
        if not l.startswith('\t'):
            return False
        if mot.search(l):
            return bool(re.match(r'^\t(?:ctx\.)?temp\.f32 = float\(' + re.escape(reg) + r'\.f64\);$', l))
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gen', default=GEN)
    ap.add_argument('--verifier', action='store_true', help='compte sans écrire')
    ap.add_argument('--annuler', action='store_true', help='revenir à la forme entière')
    a = ap.parse_args()
    fichiers = sorted(f for f in os.listdir(a.gen) if re.match(r'\w+_recomp\.\d+\.cpp$', f))
    pchs = [f for f in os.listdir(a.gen) if f.endswith('_pch.h')]
    if not fichiers or len(pchs) != 1:
        sys.exit('ERREUR : chargements_flottants : pas de *_recomp.N.cpp ou pas un seul *_pch.h dans %s' % a.gen)
    sites = presents = modifies = copies = 0
    for f in fichiers:
        chemin = os.path.join(a.gen, f)
        lignes = open(chemin, encoding='utf-8').read().split('\n')
        sortie = []
        i = 0
        n = 0
        while i < len(lignes):
            ligne = lignes[i]
            if a.annuler:
                m = NOUVEAU.match(ligne)
                if m:
                    sortie.append('\ttemp.u32 = REX_LOAD_U32(%s);' % m.group(2))
                    sortie.append('\t%s.f64 = double(temp.f32);' % m.group(1))
                    n += 1
                    i += 1
                    continue
            else:
                if NOUVEAU.match(ligne):
                    presents += 1
                m = CHARGEMENT.match(ligne)
                u = UTILISATION.match(lignes[i + 1]) if m and i + 1 < len(lignes) else None
                if m and u and u.group(2) == m.group(1) and copie(lignes, i + 2, u.group(1)):
                    copies += 1
                elif m and u and u.group(2) == m.group(1):
                    sortie.append('\t%s.f64 = double(REX_LOAD_F32(%s));' % (u.group(1), m.group(2)))
                    n += 1
                    i += 2
                    continue
            sortie.append(ligne)
            i += 1
        if n:
            sites += n
            modifies += 1
            if not a.verifier:
                open(chemin, 'w', encoding='utf-8', newline='\n').write('\n'.join(sortie))
    pch = os.path.join(a.gen, pchs[0])
    texte = open(pch, encoding='utf-8').read()
    if not a.annuler and MARQUE not in texte:
        ancre = '#define REX_MEMORY_SIZE'
        if ancre not in texte:
            sys.exit('ERREUR : chargements_flottants : pas de REX_MEMORY_SIZE dans %s' % pch)
        if not a.verifier:
            open(pch, 'w', encoding='utf-8', newline='\n').write(texte.replace(ancre, MACROS + ancre, 1))
    print('chargements_flottants%s : %d chargements flottants %s dans %d fichiers (%d déjà sous cette forme ; %d '
          'copies gardées en forme entière)' %
          (' (vérification)' if a.verifier else '', sites, 'remis en forme entière' if a.annuler else 'réécrits',
           modifies, presents, copies))


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""stvewx / stvehx : la lane écrite lue dans le registre vectoriel, sans passer par la mémoire (d'après le port 85 de
renut_nx_ultimate_base, tools/vec_lane_index.py et rex/ppc/vec_lane.h de blur-nx, 2026-11-11).

Le générateur écrit l'écriture d'un élément d'un registre vectoriel comme l'écriture de la lane que choisit l'adresse :
    REX_STORE_U32(ea, V.u32[3 - ((ea & 0xF) >> 2)]);  ->  REX_STORE_U32(ea, REX_LANE_U32(V, 3 - ((ea & 0xF) >> 2)));
    REX_STORE_U16(ea, V.u16[7 - ((ea & 0xF) >> 1)]);  ->  REX_STORE_U16(ea, REX_LANE_U16(V, 7 - ((ea & 0xF) >> 1)));
Avec un indice connu seulement à l'exécution, le compilateur écrit tout le registre sur la pile et relit la lane :
sur l'A57, cette lecture plus étroite que l'écriture qui la précède n'est pas transmise et attend que l'écriture
atteigne le cache. REX_LANE_U32 / REX_LANE_U16 prennent les deux moitiés de 64 bits du registre (deux umov) et
choisissent par les bits de l'indice : même valeur, sans mémoire. Exact : la lane i de la disposition de l'hôte (petit
boutiste) est la moitié i >> 1 décalée de 32 (i & 1) (16 bits : moitié i >> 2, décalage 16 (i & 3)) ; test de tous
les indices : switch/tools/test_lanes_registre.py (x86 et AArch64). -DREX_VEC_LANE_MEMOIRE : la forme indexée d'origine.

Chez nous (relevé du 2026-11-04) : sub_821D3638 0,74 % du thread principal, sub_821DB498 0,28 %, sub_821B9EC8 0,14 %.
Idempotent. Après : nouveau profil PGO.
  python3 switch/tools/lanes_registre.py [--gen switch/generated/default] [--verifier]
"""
import argparse
import glob
import os
import re
import sys

LANE32 = re.compile(r'([\w.]+)\.u32\[3 - \(\(ea & 0xF\) >> 2\)\]')
LANE16 = re.compile(r'([\w.]+)\.u16\[7 - \(\(ea & 0xF\) >> 1\)\]')
MARQUE = '// torchlight : lanes de stvewx / stvehx lues dans le registre (switch/tools/lanes_registre.py)'
MACROS = MARQUE + '''
#ifndef REX_LANE_U32
#if defined(REX_VEC_LANE_MEMOIRE)
#define REX_LANE_U32(v, i) ((v).u32[(i)])
#define REX_LANE_U16(v, i) ((v).u16[(i)])
#else
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#include <cstring>
// Les deux moitiés de 64 bits du registre (sur l'hôte x86, par memcpy : même logique, testée par la build PC).
[[gnu::always_inline]] static inline void RexMoities(const void* v, uint64_t& lo, uint64_t& hi) noexcept {
#if defined(__aarch64__)
  const uint64x2_t q = vld1q_u64(static_cast<const uint64_t*>(v));
  lo = vgetq_lane_u64(q, 0);
  hi = vgetq_lane_u64(q, 1);
#else
  std::memcpy(&lo, v, 8);
  std::memcpy(&hi, static_cast<const uint8_t*>(v) + 8, 8);
#endif
}
[[gnu::always_inline]] static inline uint32_t RexLaneU32(const uint32_t* u32, uint32_t i) noexcept {
  uint64_t lo, hi;
  RexMoities(u32, lo, hi);
  return uint32_t(((i & 2u) ? hi : lo) >> ((i & 1u) * 32u));
}
[[gnu::always_inline]] static inline uint16_t RexLaneU16(const uint16_t* u16, uint32_t i) noexcept {
  uint64_t lo, hi;
  RexMoities(u16, lo, hi);
  return uint16_t(((i & 4u) ? hi : lo) >> ((i & 3u) * 16u));
}
#define REX_LANE_U32(v, i) RexLaneU32((v).u32, uint32_t(i))
#define REX_LANE_U16(v, i) RexLaneU16((v).u16, uint32_t(i))
#endif
#endif  // REX_LANE_U32
// (fin des macros de switch/tools/lanes_registre.py)
'''


def main():
    ici = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', default=os.path.join(ici, '..', 'generated', 'default'))
    ap.add_argument('--verifier', action='store_true', help='compter seulement, ne rien écrire')
    a = ap.parse_args()
    pchs = glob.glob(os.path.join(a.gen, '*_pch.h'))
    if len(pchs) != 1:
        sys.exit('ERREUR : lanes_registre : pas un seul *_pch.h dans %s' % a.gen)
    n32 = n16 = fichiers = deja = 0
    for chemin in sorted(glob.glob(os.path.join(a.gen, '*_recomp.*.cpp'))):
        texte = open(chemin, encoding='utf-8').read()
        deja += texte.count('REX_LANE_U32(') + texte.count('REX_LANE_U16(')
        nouveau, c32 = LANE32.subn(r'REX_LANE_U32(\1, 3 - ((ea & 0xF) >> 2))', texte)
        nouveau, c16 = LANE16.subn(r'REX_LANE_U16(\1, 7 - ((ea & 0xF) >> 1))', nouveau)
        n32 += c32
        n16 += c16
        if nouveau != texte:
            fichiers += 1
            if not a.verifier:
                open(chemin, 'w', encoding='utf-8', newline='\n').write(nouveau)
    texte = open(pchs[0], encoding='utf-8').read()
    if not a.verifier and MARQUE not in texte:
        ancre = '// torchlight : chargements flottants inversés dans l\'unité vectorielle'
        if ancre not in texte:
            sys.exit('ERREUR : lanes_registre : ancre « %s » absente de %s' % (ancre, pchs[0]))
        open(pchs[0], 'w', encoding='utf-8', newline='\n').write(texte.replace(ancre, MACROS + ancre, 1))
    print('lanes_registre%s : %d lanes de 32 bits et %d de 16 bits lues dans le registre (%d fichiers ; %d déjà sous '
          'cette forme)' % (' (vérification)' if a.verifier else '', n32, n16, fichiers, deja))


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Constantes que le jeu charge depuis ses données en lecture seule, remplacées par leur valeur à la compilation.
D'après tools/ro_constants.py de sa2-nx (https://github.com/ChanseyIsTheBest/sa2-nx, GPL-3.0), adapté aux formes du
code généré de Torchlight (REX_LOAD_U32(ctx.rN.u32 + décalage)) ; chargements vectoriels non traités.

Le compilateur du 360 range chaque nombre flottant littéral dans .rdata et le charge par une adresse construite
avec `lis` :
    lis r11,-32241        ctx.r11.s64 = -2112946176;
    lfs f31,-21952(r11)   temp.u32 = REX_LOAD_U32(ctx.r11.u32 + -21952);  ctx.f31.f64 = double(temp.f32);
Le code recompilé le relit vraiment à chaque fois (chargement, inversion des octets, passage au registre flottant,
conversion), pour un nombre qui ne change jamais : .rdata est en lecture seule (indicateurs de la section).

Cet outil suit, le long de chaque ligne droite d'une fonction, les constantes que `lis` / `addi` / `ori` mettent dans
les registres (une étiquette oublie tout, un appel oublie les registres rangés dans ctx, toute autre écriture d'un
registre l'oublie), et quand l'adresse d'un chargement est alors connue et tombe entièrement dans .rdata, remplace le
chargement par REX_RO_U32(adresse, valeur) / REX_RO_U64 / ... :
  - sur la Switch : la valeur ; le compilateur replie alors la conversion et souvent le calcul qui suit ;
  - sur PC : le chargement comme avant, comparé à la valeur (RexRoMismatch signale une différence, un compteur
    compte les vérifications ; src/torchlight_appels_indirects.cpp) ;
  - avec REX_RO_CONSTANTS_OFF : le chargement simple.
Chargements flottants (`lfs`), doubles (`lfd`) et entiers (`lwz` / `lhz` / `lbz` / `ld`, sauf --sans-entiers) ;
un entier replié dans un registre devient lui-même une constante connue pour la suite.

  python3 switch/tools/constantes_lecture_seule.py [--image switch/out-pc/image_jeu.bin] [--verifier]
L'image est la copie de switch/tools/image_jeu.py ; sa taille doit être le REX_IMAGE_SIZE du pch. Idempotent (un
chargement replié ne correspond plus aux motifs).
"""
import argparse
import os
import re
import struct
import sys

RACINE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GEN = os.path.join(RACINE, 'switch', 'generated', 'default')
REG = r'(?:ctx\.)?r\d+'
CONST = re.compile(r'^\t(' + REG + r')\.s64 = (-?\d+);$')
ADDI = re.compile(r'^\t(' + REG + r')\.s64 = (' + REG + r')\.s64 \+ (-?\d+);$')
ORI = re.compile(r'^\t(' + REG + r')\.u64 = (' + REG + r')\.u64 \| (\d+);$')
FLOAT = re.compile(r'^\t((?:ctx\.)?temp)\.u32 = REX_LOAD_U32\((' + REG + r')\.u32 \+ (-?\d+)\);$')
FLOAT_UTIL = re.compile(r'\.f64 = double\((?:ctx\.)?temp\.f32\);$')
DOUBLE = re.compile(r'^\t((?:ctx\.)?f\d+)\.u64 = REX_LOAD_U64\((' + REG + r')\.u32 \+ (-?\d+)\);$')
ENTIER = re.compile(r'^\t(' + REG + r')\.u64 = REX_LOAD_(U8|U16|U32|U64)\((' + REG + r')\.u32 \+ (-?\d+)\);$')
# Chaque mention d'un registre général, avec ce qui suit : un champ et peut-être un opérateur d'affectation.
MENTION = re.compile(r'(?<![\w.])(' + REG + r')(?!\d)(\.\w+)?(\s*(?:[-+*/|&^%]|<<|>>)?=(?!=))?')
APPEL = re.compile(r'\(ctx, base[,)]|REX_CALL_INDIRECT_|ppc_trap\(')
STRUCTURE = re.compile(r'^\t+(?:switch|for|while|do|else|case|default|try|catch)\b|[{}]\s*$|^\t*\}')
TAILLE = {'U8': 1, 'U16': 2, 'U32': 4, 'U64': 8}
MARQUE = '// torchlight : constantes des données en lecture seule repliées (switch/tools/constantes_lecture_seule.py)'
MACROS = MARQUE + '''
#ifndef REX_RO_U32
#if defined(REX_RO_CONSTANTS_OFF)
#define REX_RO_U8(a, v) REX_LOAD_U8(a)
#define REX_RO_U16(a, v) REX_LOAD_U16(a)
#define REX_RO_U32(a, v) REX_LOAD_U32(a)
#define REX_RO_U64(a, v) REX_LOAD_U64(a)
#elif defined(__SWITCH__)
#define REX_RO_U8(a, v) ((uint8_t)(v))
#define REX_RO_U16(a, v) ((uint16_t)(v))
#define REX_RO_U32(a, v) ((uint32_t)(v))
#define REX_RO_U64(a, v) ((uint64_t)(v))
#else
extern "C" void RexRoMismatch(unsigned address) noexcept;
extern "C" volatile unsigned long long g_rex_ro_checked;
template <typename T>
[[gnu::always_inline]] static inline T RexRoCheck(T memory, T constant, unsigned address) noexcept {
  g_rex_ro_checked = g_rex_ro_checked + 1;
  if (memory != constant) [[unlikely]] RexRoMismatch(address);
  return memory;
}
#define REX_RO_U8(a, v) RexRoCheck<uint8_t>(REX_LOAD_U8(a), (uint8_t)(v), (a))
#define REX_RO_U16(a, v) RexRoCheck<uint16_t>(REX_LOAD_U16(a), (uint16_t)(v), (a))
#define REX_RO_U32(a, v) RexRoCheck<uint32_t>(REX_LOAD_U32(a), (uint32_t)(v), (a))
#define REX_RO_U64(a, v) RexRoCheck<uint64_t>(REX_LOAD_U64(a), (uint64_t)(v), (a))
#endif
#endif
'''


def plages_lecture_seule(image):
    """[(début, fin)] des sections .rdata de l'image (adresses invitées), et la base de l'image."""
    pe = struct.unpack_from('<I', image, 0x3C)[0]
    if image[:2] != b'MZ' or image[pe:pe + 4] != b'PE\0\0':
        sys.exit('ERREUR : constantes_lecture_seule : la copie de l\'image n\'est pas une image PE')
    n, opt = struct.unpack_from('<H', image, pe + 6)[0], struct.unpack_from('<H', image, pe + 20)[0]
    base = struct.unpack_from('<I', image, pe + 24 + 28)[0]
    table = pe + 24 + opt
    plages = []
    for i in range(n):
        nom, vtaille, va, _, _, _, _, _, _, drapeaux = struct.unpack_from('<8sIIIIIIHHI', image, table + 40 * i)
        nom = nom.rstrip(b'\0').decode('latin-1')
        # Lisible, non inscriptible, pas du code ; .rdata seulement.
        if nom == '.rdata' and drapeaux & 0x40000000 and not drapeaux & 0x80000000 and not drapeaux & 0x20000020:
            plages.append((base + va, base + va + vtaille))
    return plages, base


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gen', default=GEN)
    ap.add_argument('--image', default=os.path.join(RACINE, 'switch', 'out-pc', 'image_jeu.bin'))
    ap.add_argument('--verifier', action='store_true', help='compte sans écrire')
    ap.add_argument('--sans-entiers', action='store_true', help='seulement les chargements flottants et doubles')
    a = ap.parse_args()
    fichiers = sorted(f for f in os.listdir(a.gen) if re.match(r'\w+_recomp\.\d+\.cpp$', f))
    pchs = [f for f in os.listdir(a.gen) if f.endswith('_pch.h')]
    if not fichiers or len(pchs) != 1:
        sys.exit('ERREUR : constantes_lecture_seule : pas de *_recomp.N.cpp ou pas un seul *_pch.h dans %s' % a.gen)
    if not os.path.exists(a.image):
        sys.exit('ERREUR : constantes_lecture_seule : %s absent : lancer switch/tools/image_jeu.py' % a.image)
    image = open(a.image, 'rb').read()
    chemin_pch = os.path.join(a.gen, pchs[0])
    pch = open(chemin_pch, encoding='utf-8').read()
    plages, base = plages_lecture_seule(image)
    mb = re.search(r'#define REX_IMAGE_BASE (0x[0-9A-Fa-f]+)', pch)
    mt = re.search(r'#define REX_IMAGE_SIZE (0x[0-9A-Fa-f]+)', pch)
    if not mb or not mt or int(mb.group(1), 16) != base or int(mt.group(1), 16) != len(image) or not plages:
        sys.exit('ERREUR : constantes_lecture_seule : %s n\'est pas l\'image de ce code généré' % a.image)

    def constante(adresse, taille):
        adresse &= 0xFFFFFFFF
        for debut, fin in plages:
            if debut <= adresse and adresse + taille <= fin:
                return adresse, int.from_bytes(image[adresse - base:adresse - base + taille], 'big')
        return None

    stats = dict(flottants=0, doubles=0, entiers=0, deja=0, modifies=0)
    for f in fichiers:
        chemin = os.path.join(a.gen, f)
        lignes = open(chemin, encoding='utf-8').read().split('\n')
        connus = {}
        sale = False
        for i, ligne in enumerate(lignes):
            if not ligne.startswith('\t'):
                connus.clear()  # étiquette, première ou dernière ligne d'une fonction
                continue
            if ligne.startswith('\t//'):
                continue
            if 'REX_RO_U' in ligne:
                stats['deja'] += 1
            m = CONST.match(ligne)
            if m:
                connus[m.group(1)] = int(m.group(2))
                continue
            m = ADDI.match(ligne)
            if m and m.group(2) in connus:
                connus[m.group(1)] = connus[m.group(2)] + int(m.group(3))
                continue
            m = ORI.match(ligne)
            if m and m.group(2) in connus:
                connus[m.group(1)] = connus[m.group(2)] | int(m.group(3))
                continue
            if STRUCTURE.search(ligne):
                connus.clear()
                continue
            m = FLOAT.match(ligne)
            if m and m.group(2) in connus and i + 1 < len(lignes) and FLOAT_UTIL.search(lignes[i + 1]):
                c = constante(connus[m.group(2)] + int(m.group(3)), 4)
                if c:
                    lignes[i] = '\t%s.u32 = REX_RO_U32(0x%08X, 0x%08X);' % (m.group(1), c[0], c[1])
                    stats['flottants'] += 1
                    sale = True
                continue
            m = DOUBLE.match(ligne)
            if m and m.group(2) in connus:
                c = constante(connus[m.group(2)] + int(m.group(3)), 8)
                if c:
                    lignes[i] = '\t%s.u64 = REX_RO_U64(0x%08X, 0x%016Xull);' % (m.group(1), c[0], c[1])
                    stats['doubles'] += 1
                    sale = True
                continue
            m = ENTIER.match(ligne)
            if m and not a.sans_entiers and m.group(3) in connus:
                c = constante(connus[m.group(3)] + int(m.group(4)), TAILLE[m.group(2)])
                if c:
                    lignes[i] = '\t%s.u64 = REX_RO_%s(0x%08X, 0x%X%s);' % (
                        m.group(1), m.group(2), c[0], c[1], 'ull' if m.group(2) == 'U64' else 'u')
                    connus[m.group(1)] = c[1]  # le registre contient maintenant une valeur connue
                    stats['entiers'] += 1
                    sale = True
                    continue
            # Toute autre instruction : oublier chaque registre qu'elle peut écrire (affectation, ou mention du
            # registre entier), et après un appel les registres rangés dans ctx.
            for mm in MENTION.finditer(ligne):
                if mm.group(1) in connus and (mm.group(3) or not mm.group(2)):
                    del connus[mm.group(1)]
            if APPEL.search(ligne):
                for nom in [k for k in connus if k.startswith('ctx.')]:
                    del connus[nom]
        if sale:
            stats['modifies'] += 1
            if not a.verifier:
                open(chemin, 'w', encoding='utf-8', newline='\n').write('\n'.join(lignes))
    ancre = '#define REX_MEMORY_SIZE'
    if MARQUE not in pch:
        if ancre not in pch:
            sys.exit('ERREUR : constantes_lecture_seule : pas de REX_MEMORY_SIZE dans %s' % chemin_pch)
        if not a.verifier:
            open(chemin_pch, 'w', encoding='utf-8', newline='\n').write(pch.replace(ancre, MACROS + ancre, 1))
    print('constantes_lecture_seule%s : %d chargements flottants, %d doubles et %d entiers repliés (%s ; %d déjà '
          'repliés), %d fichiers modifiés' %
          (' (vérification)' if a.verifier else '', stats['flottants'], stats['doubles'], stats['entiers'],
           ', '.join('%08X-%08X' % p for p in plages), stats['deja'], stats['modifies']))


if __name__ == '__main__':
    main()

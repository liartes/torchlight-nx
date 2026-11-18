#!/usr/bin/env python3
"""Boucle du mélange des textures d'équipement (sub_822DC6B0, @0x822DD21C..0x822DD39C) faite par l'hôte
(torchlight_melange, src/torchlight_donnees_natif.cpp ; d'après torchlight-recomp de YaPeL, GPL-3.0).

Insère, dans le code généré de sub_822DC6B0 :
- après « rotlwi r26,r10,0 » (les lignes, déjà non nulles) :
      { uint32_t s[4]; if (torchlight_melange(ctx, base, r26.u32, r29.u32, ..., s)) { r26 = 0; r28 = 0 (si colonnes);
        r29, r31, r30 = s[...]; goto loc_822DD39C; } }
  vrai : la boucle faite par l'hôte ; faux (option à 0, format non natif, validation) : la boucle recompilée ;
- à loc_822DD39C, la sortie de la boucle : torchlight_melange_fin(ctx, base) (comparaison du mode 2).
Les noms des registres sont ceux de la fonction générée (variables locales du générateur ou des passes, sinon ctx.).
Vérifie que la boucle est celle attendue (instructions de bornes, sauts vers sa fin seulement). À lancer après les
autres étapes ; idempotent. Après une régénération : relancer, puis nouveau profil PGO.
  python3 switch/tools/melange_natif.py [--verifier]
"""
import os
import re
import sys

ICI = os.path.dirname(os.path.abspath(__file__))
G = os.path.join(ICI, '..', 'generated', 'default')
MARQUE = '// melange_natif.py'
FONCTION = 'DEFINE_REX_FUNC(sub_822DC6B0)'
ATTENDU = ['rotlwi r26,r10,0', 'lwz r11,140(r1)', 'bl 0x82474560', 'cmplwi cr6,r6,255', 'bl 0x82474560',
           'fmadds f2,f9,f10,f7', 'fmadds f0,f8,f10,f3', 'fmadds f12,f5,f10,f1', 'bl 0x82473fa0', 'addic. r28,r28,-1',
           'addi r31,r31,4', 'addi r29,r29,4', 'bne 0x822dd230', 'addic. r26,r26,-1', 'bne 0x822dd220']


def nom(corps, r):
    for forme in (r, 'll_' + r, 'l_' + r):
        if any(re.match(r'^\tPPCRegister %s\{\};$' % forme, l) or
               re.match(r'^\tREX_(?:LOOP|LEAF)_LOCAL\(PPCRegister, %s,' % forme, l) for l in corps):
            return forme
    return 'ctx.' + r


def transformer(texte):
    debut = texte.index(FONCTION)
    fin = texte.index('\n}\n', debut) + 3
    corps = texte[debut:fin].split('\n')
    k = corps.index('\t// rotlwi r26,r10,0')
    k_fin = corps.index('loc_822DD39C:')
    assert k < k_fin
    instructions = [l[4:] for l in corps[k:k_fin] if l.startswith('\t// ')]
    i = 0
    for a in ATTENDU:  # dans l'ordre, sans être contiguës
        while i < len(instructions) and instructions[i].strip() != a:
            i += 1
        assert i < len(instructions), 'instruction attendue absente : ' + a
        i += 1
    for l in corps[k:k_fin]:
        m = re.search(r'goto (loc_[0-9A-F]+);', l)
        assert not m or (m.group(1) + ':') in corps[k:k_fin + 1], l
        assert 'return;' not in l, l
    # Ligne C++ de l'instruction rotlwi : l'appel vient juste après.
    k_insertion = k + 1
    while not corps[k_insertion].startswith('\t') or corps[k_insertion].startswith('\t//'):
        k_insertion += 1
    n = {r: nom(corps, r) for r in ('r17', 'r19', 'r26', 'r27', 'r28', 'r29', 'r30', 'r31', 'f30', 'f31')}
    appel = ('\t{ uint32_t s[4]; if (torchlight_melange(ctx, base, %(r26)s.u32, %(r29)s.u32, %(r30)s.u32, %(r31)s.u32, '
             '%(r27)s.u32, %(r19)s.u32, %(r17)s.u32, %(f30)s.f64, %(f31)s.f64, s)) { %(r26)s.u64 = 0; '
             'if (s[3]) %(r28)s.u64 = 0; %(r29)s.u64 = s[0]; %(r31)s.u64 = s[1]; %(r30)s.u64 = s[2]; '
             'goto loc_822DD39C; } }  ' % n) + MARQUE
    corps.insert(k_insertion + 1, appel)
    k_fin = corps.index('loc_822DD39C:')
    corps.insert(k_fin + 1, '\ttorchlight_melange_fin(ctx, base);  ' + MARQUE)
    declaration = ('bool torchlight_melange(PPCContext& ctx, uint8_t* base, uint32_t r26, uint32_t r29, uint32_t r30, '
                   'uint32_t r31, uint32_t r27, uint32_t r19, uint32_t r17, double f30, double f31, uint32_t* sortie);'
                   '  %s\nvoid torchlight_melange_fin(PPCContext& ctx, uint8_t* base);  %s\n'
                   % (MARQUE, MARQUE))
    return texte[:debut] + declaration + '\n'.join(corps) + texte[fin:]


def main():
    verifier = '--verifier' in sys.argv[1:]
    for nom_f in sorted(os.listdir(G)):
        if not (nom_f.startswith('torchlight_recomp.') and nom_f.endswith('.cpp')):
            continue
        chemin = os.path.join(G, nom_f)
        texte = open(chemin, encoding='utf-8').read()
        if FONCTION not in texte:
            continue
        if MARQUE in texte:
            print('melange_natif : déjà fait (%s)' % nom_f)
            return 0
        if verifier:
            print('melange_natif : à faire (%s)' % nom_f)
            return 1
        neuf = transformer(texte)
        open(chemin, 'w', encoding='utf-8', newline='\n').write(neuf)
        print('melange_natif : %s réécrit' % nom_f)
        return 0
    print('melange_natif : %s introuvable' % FONCTION, file=sys.stderr)
    return 1


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""Boucle des particules de BillboardRenderer::_updateRenderQueue (sub_821DA090) rendue séparable du reste.

La fonction fait : préparation, beginBillboards (sub_821DDBB0, verrouille le tampon), boucle sur les particules (une
injectBillboard sub_821DA848 chacune), déverrouillage, setVisible, mise en file, statistique. Seule la boucle peut
quitter le thread principal (src/torchlight_particules.cpp). Plutôt qu'une réécriture à la main, cette étape recopie
tel quel le texte généré de la boucle dans une fonction à part, torchlight_billboards_boucle (entrée : r3 le renderer,
r4 la réserve de particules), et ajoute après beginBillboards
    if (torchlight_billboards_differer(ctx, base, r30, r25, r29)) goto loc_821DA774;
Sans option, torchlight_billboards_differer rend faux : la fonction d'origine continue comme avant.

À lancer après les autres étapes du code généré (le texte recopié doit être le texte final) ; idempotent.

  python3 switch/tools/billboards_differes.py [--verifier]
"""
import os
import re
import sys

ICI = os.path.dirname(os.path.abspath(__file__))
G = os.path.join(ICI, '..', 'generated', 'default')
MARQUE = '// billboards_differes.py'
FONCTION = 'DEFINE_REX_FUNC(sub_821DA090)'


def registre(lignes, nom):
    return nom if any(l.strip() == 'PPCRegister %s{};' % nom for l in lignes) else 'ctx.' + nom


def instruction(lignes, commentaire):
    """Lignes C++ de l'instruction après son commentaire, sans le mode flottant (remis au début de la boucle)."""
    i = lignes.index('\t// ' + commentaire)
    sortie = []
    for l in lignes[i + 1:]:
        if l.lstrip().startswith('//') or l.startswith('loc_'):
            break
        if 'disableFlushMode' not in l:
            sortie.append(l)
    assert sortie, commentaire
    return sortie


def transformer(texte):
    debut = texte.index(FONCTION)
    fin = texte.index('\n}\n', debut) + 3
    corps = texte[debut:fin].split('\n')

    k_begin = next(i for i, l in enumerate(corps) if re.fullmatch(r'\t(__imp__)?sub_821DDBB0\(ctx, base\);', l))
    k_tail = corps.index('loc_821DA708:')
    assert 'loc_821DA774:' in corps
    r25, r29, r30 = (registre(corps, n) for n in ('r25', 'r29', 'r30'))
    for nom in (r29, r30, 'r27', 'f29', 'f31'):
        assert not nom.startswith('ctx.'), nom  # registres non volatils en variables locales du générateur

    segment = corps[k_begin + 1:k_tail]
    for l in segment:
        # Sauts dans la boucle ou vers sa fin, rien d'autre.
        m = re.search(r'goto (loc_[0-9A-F]+);', l)
        assert not m or m.group(1) == 'loc_821DA708' or (m.group(1) + ':') in segment, l
        assert 'return;' not in l, l

    # Déclarations de la fonction d'origine : tout ce qui précède la première instruction.
    k_decl = corps.index('\t// mflr r12')
    declarations = corps[1:k_decl]

    r27 = (-32245 << 16) + -23872
    # r27, f31 et f29 : posés avant la boucle dans la fonction d'origine.
    assert '\t// addi r27,r10,-23872' in corps
    entree = [
        '\tr30.u64 = ctx.r3.u64;',
        '\tr29.u64 = ctx.r4.u64;',
        '\tr27.s64 = %d;' % r27,
        '\tctx.fpscr.disableFlushMode();',
    ] + instruction(corps, 'lfs f31,68(r27)') + instruction(corps, 'lfs f29,64(r27)') + [
        '\t// stwu r1,-368(r1)',
        '\tea = -368 + ctx.r1.u32;',
        '\tREX_STORE_U32(ea, ctx.r1.u32);',
        '\tctx.r1.u32 = ea;',
    ]
    boucle = (['%s : boucle de sub_821DA090, texte généré recopié' % MARQUE,
               'extern "C" void torchlight_billboards_boucle(PPCContext& __restrict ctx, uint8_t* base) {'] +
              declarations + entree + segment +
              ['loc_821DA708:', '\tctx.r1.s64 = ctx.r1.s64 + 368;', '\treturn;', '}', ''])

    appel = ('\tif (torchlight_billboards_differer(ctx, base, %s.u32, %s.u32, %s.u32)) goto loc_821DA774;  %s' %
             (r30, r25, r29, MARQUE))
    corps.insert(k_begin + 1, appel)
    declaration = ('extern "C" bool torchlight_billboards_differer(PPCContext& ctx, uint8_t* base, uint32_t renderer, '
                   'uint32_t queue, uint32_t pool);  %s\n' % MARQUE)
    return texte[:debut] + declaration + '\n'.join(corps) + texte[fin:].rstrip('\n') + '\n\n' + '\n'.join(boucle)


def main():
    verifier = '--verifier' in sys.argv[1:]
    fichiers = [n for n in sorted(os.listdir(G)) if n.startswith('torchlight_recomp.') and n.endswith('.cpp')]
    for nom in fichiers:
        chemin = os.path.join(G, nom)
        texte = open(chemin, encoding='utf-8').read()
        if FONCTION not in texte:
            continue
        if MARQUE in texte:
            print('billboards_differes : déjà fait (%s)' % nom)
            return 0
        if verifier:
            print('billboards_differes : à faire (%s)' % nom)
            return 1
        neuf = transformer(texte)
        with open(chemin, 'w', encoding='utf-8') as f:
            f.write(neuf)
        print('billboards_differes : %s réécrit' % nom)
        return 0
    print('billboards_differes : %s introuvable' % FONCTION, file=sys.stderr)
    return 1


if __name__ == '__main__':
    sys.exit(main())

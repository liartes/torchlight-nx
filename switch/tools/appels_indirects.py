#!/usr/bin/env python3
"""Appels indirects : relevé de leurs cibles, puis appel direct gardé là où un site va presque toujours au même
endroit. D'après tools/indirect_calls.py de sa2-nx (https://github.com/ChanseyIsTheBest/sa2-nx, GPL-3.0),
adapté au code généré de Torchlight (la ligne « ctx.lr = ... » entre « // bctrl » et l'appel).

Un `bctrl` (appel de méthode virtuelle d'Ogre, appel par pointeur) est REX_CALL_INDIRECT_FUNC(ctr.u32) : test de
plage, lecture dans la table de répartition, test du pointeur nul, appel par registre, que le compilateur ne peut ni
intégrer ni prévoir. 17 220 sites dans le code de Torchlight.

1. Marquage. Chaque appel `bctrl` devient
       REX_CALL_INDIRECT_FUNC_AT(0x<fonction>, <n>, ctr.u32);
   (adresse de la fonction et numéro du site dans la fonction, stables d'une génération à l'autre). Sur la Switch,
   c'est REX_CALL_INDIRECT_FUNC. Sur PC, avec le relevé actif (src/torchlight_appels_indirects.cpp, option
   torchlight_releve_appels_indirects), chaque site déclare sa cible : nombre d'appels et cibles par site.
2. Promotion. La liste (switch/appels_indirects_promus.txt, lignes `<fonction> <site> <cible> [<cible2> [<cible3>]]`,
   écrite par switch/tools/appels_indirects_promus.py à partir d'un relevé) transforme le site en
       if (REX_PROMOTED_IS(ctr.u32, 0x<cible>u)) [[likely]] { REX_PROMOTED_CALL(0x<cible>, sub_<cible>);
           sub_<cible>(ctx, base); } else { REX_CALL_INDIRECT_FUNC_AT(...); }
   La garde compare la cible elle-même : l'appel promu est celui que la table aurait fait (elle contient sub_<cible>,
   le nom interceptable ; appels_directs.py, À LANCER APRÈS, en fait __imp__sub_ quand rien ne l'intercepte). Toute
   autre cible prend l'ancien chemin. Sur PC, REX_PROMOTED_CALL compte et vérifie que la table contient bien cette
   fonction pour cette adresse (RexPromotedMismatch sinon) : la seule hypothèse, testée à chaque appel promu.

  python3 switch/tools/appels_indirects.py [--promouvoir switch/appels_indirects_promus.txt] [--verifier]
Idempotent ; une liste modifiée s'applique par un nouveau lancement. Sauts indirects (bctr, formes qui sauvent des
registres autour de l'appel) : laissés tels quels.
"""
import argparse
import os
import re
import sys

RACINE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GEN = os.path.join(RACINE, 'switch', 'generated', 'default')
FUNC = re.compile(r'^DEFINE_REX_FUNC\((sub_([0-9A-F]{8}))\) \{$')
SIMPLE = re.compile(r'^\tREX_CALL_INDIRECT_FUNC\(((?:ctx\.)?ctr\.u32)\);$')
SITE = re.compile(r'^\tREX_CALL_INDIRECT_FUNC_AT\(0x[0-9A-F]{8}, \d+, ((?:ctx\.)?ctr\.u32)\);$')
PROMU = re.compile(r'^\tif \(REX_PROMOTED_IS\(((?:ctx\.)?ctr\.u32), 0x[0-9A-F]{8}u\)\) \[\[likely\]\] \{ '
                   r'REX_PROMOTED_CALL\(.*\} else \{ REX_CALL_INDIRECT_FUNC_AT\(.*\); \}$')
MARQUE = '// torchlight : sites d\'appels indirects (switch/tools/appels_indirects.py)'
MACROS = MARQUE + '''
#ifndef REX_PROMOTED_IS
#if defined(REX_PROMOTED_CALLS_OFF)
#define REX_PROMOTED_IS(x, target) false
#else
#define REX_PROMOTED_IS(x, target) ((uint32_t)(x) == (target))
#endif
#endif
#ifndef REX_CALL_INDIRECT_FUNC_AT
#if defined(__SWITCH__) || defined(REX_INDIRECT_CENSUS_OFF)
#define REX_CALL_INDIRECT_FUNC_AT(function, site, x) REX_CALL_INDIRECT_FUNC(x)
#define REX_PROMOTED_CALL(target, function) ((void)0)
#else
extern "C" unsigned char g_rex_indirect_census;
extern "C" volatile unsigned long long g_rex_promoted_calls;
extern "C" void RexIndirectCensus(unsigned function, unsigned site, unsigned target) noexcept;
extern "C" void RexPromotedMismatch(unsigned target) noexcept;
#define REX_CALL_INDIRECT_FUNC_AT(function, site, x)                                              \\
  do {                                                                                            \\
    if (g_rex_indirect_census) [[unlikely]] RexIndirectCensus((function), (site), (uint32_t)(x)); \\
    REX_CALL_INDIRECT_FUNC(x);                                                                    \\
  } while (0)
#define REX_PROMOTED_CALL(target, function)                                                   \\
  do {                                                                                        \\
    g_rex_promoted_calls = g_rex_promoted_calls + 1;                                          \\
    if (REX_LOOKUP_FUNC(base, target) != &function) [[unlikely]] RexPromotedMismatch(target); \\
  } while (0)
#endif
#endif
'''
DECL_DEBUT = '// torchlight : cibles des appels indirects promus (switch/tools/appels_indirects.py)'
DECL_FIN = '// fin des cibles promues'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gen', default=GEN)
    ap.add_argument('--promouvoir', help='liste `<fonction> <site> <cible>...` (hexa, décimal, hexa)')
    ap.add_argument('--verifier', action='store_true', help='compte sans écrire')
    a = ap.parse_args()
    fichiers = sorted(f for f in os.listdir(a.gen) if re.match(r'\w+_recomp\.\d+\.cpp$', f))
    pchs = [f for f in os.listdir(a.gen) if f.endswith('_pch.h')]
    if not fichiers or len(pchs) != 1:
        sys.exit('ERREUR : appels_indirects : pas de *_recomp.N.cpp ou pas un seul *_pch.h dans %s' % a.gen)
    promus = {}
    if a.promouvoir and os.path.exists(a.promouvoir):
        for ligne in open(a.promouvoir, encoding='utf-8'):
            ligne = ligne.split('#')[0].split()
            if len(ligne) >= 3:
                promus[(ligne[0].upper(), int(ligne[1]))] = [t.upper() for t in ligne[2:5]]
    textes = {}
    definies = set()
    for f in fichiers:
        textes[f] = open(os.path.join(a.gen, f), encoding='utf-8').read().split('\n')
        for ligne in textes[f]:
            m = FUNC.match(ligne)
            if m:
                definies.add(m.group(2))
    marques = promotions = multiples = deja = modifies = 0
    utilises = set()
    for f in fichiers:
        lignes = textes[f]
        sortie = list(lignes)
        fonction = None
        numero = 0
        cibles = set()
        for i, ligne in enumerate(lignes):
            m = FUNC.match(ligne)
            if m:
                fonction, numero = m.group(2), 0
                continue
            if fonction is None or not (ligne.startswith('\tREX_CALL_INDIRECT_') or
                                        ligne.startswith('\tif (REX_PROMOTED_IS(')):
                continue
            m = SIMPLE.match(ligne) or SITE.match(ligne) or PROMU.match(ligne)
            # Seulement la forme simple d'un `bctrl` : « // bctrl », « ctx.lr = ... », l'appel.
            if not m or i < 2 or not lignes[i - 2].startswith('\t// bctrl') or not lignes[i - 1].startswith('\tctx.lr = '):
                continue
            site = numero
            numero += 1
            expr = m.group(1)
            marque = 'REX_CALL_INDIRECT_FUNC_AT(0x%s, %d, %s);' % (fonction, site, expr)
            voulu = promus.get((fonction, site))
            if voulu:
                for cible in voulu:
                    if cible not in definies:
                        sys.exit('ERREUR : appels_indirects : %s donne la cible %s pour %s site %d, qui n\'est pas une '
                                 'fonction du code généré' % (a.promouvoir, cible, fonction, site))
                nouveau = '\t'
                for n, cible in enumerate(voulu):
                    nouveau += ('if (REX_PROMOTED_IS(%s, 0x%su)) %s{ REX_PROMOTED_CALL(0x%s, sub_%s); sub_%s(ctx, base); } '
                                'else ' % (expr, cible, '[[likely]] ' if n == 0 else '', cible, cible, cible))
                nouveau += '{ %s }' % marque
                cibles.update(voulu)
                promotions += 1
                multiples += len(voulu) > 1
                utilises.add((fonction, site))
                # Un lancement précédent (puis appels_directs.py) a pu écrire la même ligne avec __imp__.
                if ligne.replace('__imp__sub_', 'sub_') == nouveau:
                    deja += 1
                    continue
            else:
                nouveau = '\t' + marque
                if ligne == nouveau:
                    deja += 1
                    continue
            marques += 1
            sortie[i] = nouveau
        # Déclarations des cibles promues en tête du fichier (le générateur ne déclare que ce qu'il voit appelé).
        if DECL_DEBUT in sortie:
            d, e = sortie.index(DECL_DEBUT), sortie.index(DECL_FIN)
            del sortie[d:e + 1]
        if cibles:
            premier = next(k for k, l in enumerate(sortie) if l.startswith('#include'))
            sortie[premier + 1:premier + 1] = [DECL_DEBUT] + ['DECLARE_REX_FUNC(sub_%s);' % t for t in sorted(cibles)] + \
                [DECL_FIN]
        if sortie != lignes:
            modifies += 1
            if not a.verifier:
                open(os.path.join(a.gen, f), 'w', encoding='utf-8', newline='\n').write('\n'.join(sortie))
    absents = [k for k in promus if k not in utilises]
    if absents:
        sys.exit('ERREUR : appels_indirects : %d sites de %s absents du code généré (premier : %s site %d) : refaire la '
                 'liste à partir d\'un relevé de ce code' % (len(absents), a.promouvoir, absents[0][0], absents[0][1]))
    pch = os.path.join(a.gen, pchs[0])
    texte = open(pch, encoding='utf-8').read()
    if MARQUE not in texte:
        ancre = '//=============================================================================\n// Flush Mode'
        if ancre not in texte:
            sys.exit('ERREUR : appels_indirects : pas de section « Flush Mode » dans %s' % pch)
        if not a.verifier:
            open(pch, 'w', encoding='utf-8', newline='\n').write(texte.replace(ancre, MACROS + '\n' + ancre, 1))
    print('appels_indirects%s : %d sites bctrl écrits (%d promus en appel direct gardé, dont %d à deux ou trois '
          'cibles ; %d dans la liste), %d déjà comme voulu ; %d fichiers modifiés' %
          (' (vérification)' if a.verifier else '', marques, promotions, multiples, len(promus), deja, modifies))


if __name__ == '__main__':
    main()

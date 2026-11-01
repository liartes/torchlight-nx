#!/usr/bin/env python3
"""Plus d'écriture de l'adresse de retour dans ctx.lr avant chaque appel du code généré.

D'après elide_lr_stores de renut-nx / banjo-nx (GPL-3.0, https://github.com/ChanseyIsTheBest/renut-nx,
docs/codegen-enhancements.md ; masseffect-nx : -10 % sur son thread principal avec non_volatile_as_local).

Le générateur écrit « ctx.lr = 0x<adresse de retour>; » avant chaque bl / bctrl : 406 020 écritures (mov, movk,
str) à chaque appel. Dans le code recompilé un retour (blr) est un return du C++ : la valeur n'est jamais utilisée
pour revenir. Elle n'est lue que par mflr, presque toujours dans un prologue qui la range sur la pile pour la remettre
dans ctx.lr à l'épilogue (mtlr), sans autre usage. Audit du 2026-11-01 : 34 757 mflr ; hors prologues, quelques
fonctions de la bibliothèque C du jeu (0x8287C430, 0x8287C570, 0x8287C61C, 0x8287C640 : mflr r31 / r7 puis mtlr,
appel de funclet par blrl dont la cible est mise dans lr juste avant, ce qui reste) et setjmp / longjmp (sub_82864230,
sub_828636E0, déjà inutilisables avec les registres en variables locales). Le déroulement de la pile du jeu n'existe
pas (RtlUnwind, RtlVirtualUnwind : bouchons du SDK). Le runtime ne lit ctx.lr que pour des journaux (appelant de
XexGetProcedureAddress, rapport de plantage) et la sauvegarde de l'état d'un thread ; nos interceptions, pour les
journaux du traceur. Différence avec skip_lr (abandonnée le 2026-10-02 avec non_argument_as_local) : mflr lit
toujours ctx.lr, et les mtlr restent.

Chaque « ctx.lr = 0x...; » devient « REX_LR(0x...); » : rien sur la Switch, l'écriture d'origine avec REX_LR_STORES
(option CMake TORCHLIGHT_RETOUR_LR, OFF par défaut : A/B sans régénérer). Seules les écritures d'une constante sont
touchées (pas ctx.lr = <registre>, qui est un mtlr).
À refaire après chaque génération du code (build_switch.sh le fait) ; idempotent. Usage : retour_sans_lr.py [--annuler]
"""
import os
import re
import sys

RACINE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GEN = os.path.join(RACINE, 'switch', 'generated', 'default')
MARQUE = '// torchlight : adresse de retour non écrite dans ctx.lr (switch/tools/retour_sans_lr.py)'
MACRO = MARQUE + '''
#ifndef REX_LR
#if defined(REX_LR_STORES)
#define REX_LR(x) (ctx.lr = (x))
#else
#define REX_LR(x) ((void)0)
#endif
#endif
'''
ECRIT = re.compile(r'^\tctx\.lr = (0x[0-9A-F]+);$', re.M)
MACRO_LIGNE = re.compile(r'^\tREX_LR\((0x[0-9A-F]+)\);$', re.M)


def main():
    annuler = '--annuler' in sys.argv
    pch = os.path.join(GEN, 'torchlight_pch.h')
    texte = avant = open(pch, encoding='utf-8').read()
    if annuler:
        texte = texte.replace(MACRO, '')
    elif MARQUE not in texte:
        ancre = '#define REX_LOAD_U8(x)'
        if ancre not in texte:
            sys.exit('ERREUR : retour_sans_lr : pas de REX_LOAD_U8 dans %s' % pch)
        texte = texte.replace(ancre, MACRO + ancre, 1)
    if texte != avant:
        open(pch, 'w', encoding='utf-8', newline='\n').write(texte)
    total = fichiers = 0
    for f in sorted(os.listdir(GEN)):
        if not (f.startswith('torchlight_recomp.') and f.endswith('.cpp')):
            continue
        chemin = os.path.join(GEN, f)
        t = open(chemin, encoding='utf-8').read()
        if annuler:
            nouveau, n = MACRO_LIGNE.subn(r'\tctx.lr = \1;', t)
        else:
            nouveau, n = ECRIT.subn(r'\tREX_LR(\1);', t)
            n += len(MACRO_LIGNE.findall(t))
        if nouveau != t:
            open(chemin, 'w', encoding='utf-8', newline='\n').write(nouveau)
            fichiers += 1
        total += n
    print('%s : %d écritures de ctx.lr, %d fichiers modifiés'
          % ('adresse de retour rétablie' if annuler else 'adresse de retour non écrite', total, fichiers))


if __name__ == '__main__':
    main()

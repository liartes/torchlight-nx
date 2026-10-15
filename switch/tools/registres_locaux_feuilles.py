#!/usr/bin/env python3
"""Registres du jeu en variables locales C++ dans les fonctions feuilles (sans aucun appel). D'après
tools/leaf_locals.py de sa2-nx (https://github.com/ChanseyIsTheBest/sa2-nx, GPL-3.0), adapté à Torchlight.

Les registres que le générateur ne met pas déjà en variables locales (arguments r3-r10, f1-f13, v0-v13, et ce qu'une
fonction mentionne comme ctx.<registre>) vivent dans le PPCContext. Chez nous, tous les accès à la mémoire du jeu
passent par des pointeurs volatile dans le tableau mémoire de l'hôte : le compilateur ne peut pas prouver qu'une
écriture dans la mémoire du jeu laisse ctx intact, et relit donc ces registres depuis ctx après chaque écriture
(sa2-nx : la même chose à cause de ses barrières de boucle). Une variable locale dont l'adresse n'est pas prise ne
peut pas être touchée ainsi.

Une fonction feuille (aucun appel, aucun piège) peut garder ces registres en variables locales pendant tout son
corps : personne d'autre ne regarde ctx pendant qu'elle s'exécute. Chargés au début, réécrits avant chaque retour :
    REX_LEAF_LOCAL(PPCRegister, l_r4, r4);         PPCRegister l_r4 = ctx.r4;
    ... l_r4 au lieu de ctx.r4 ...
    REX_LEAF_STORE(l_r4, r4);                      ctx.r4 = l_r4;      avant chaque retour et à la fin
Seulement les feuilles : une variable qui vit à travers un appel demande un registre sauvegardé de l'hôte, qui coûte
plus que les relectures évitées (mesuré par sa2-nx). Par défaut seulement les feuilles qui ont une boucle (un goto
vers une étiquette déjà vue) ; --toutes : toutes les feuilles.

Un registre n'est réécrit que si la fonction peut l'avoir changé : registres généraux et flottants dont chaque
mention est une simple lecture d'un champ : chargés mais pas réécrits ; vecteurs (écrits par pointeur) : toujours.
REX_LEAF_LOCALS_OFF (à la compilation) fait de chaque variable une référence à son registre de ctx : l'ancien code.
Laissées telles quelles : fonctions avec setjmp / longjmp ou sans REX_FUNC_PROLOGUE.

  python3 switch/tools/registres_locaux_feuilles.py [--verifier] [--toutes]
Idempotent (une fonction réécrite ne mentionne plus de registre de ctx). À lancer après les autres étapes qui
lisent ctx.<registre> (constantes, chargements flottants, appels indirects ne touchent pas les feuilles).
"""
import argparse
import os
import re
import sys

RACINE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GEN = os.path.join(RACINE, 'switch', 'generated', 'default')
FUNC = re.compile(r'^DEFINE_REX_FUNC\((\w+)\) \{$')
REG = re.compile(r'\bctx\.(r\d+|f\d+|v\d+|cr\d|ctr|xer|reserved)\b')
APPEL = re.compile(r'\(ctx, base[,)]|REX_CALL_INDIRECT_|ppc_trap\(|\bctx\s*[,)]')
RETOUR = re.compile(r'\breturn;')
MARQUE = '// torchlight : registres en variables locales dans les fonctions feuilles (switch/tools/registres_locaux_feuilles.py)'
MACROS = MARQUE + '''
#ifndef REX_LEAF_LOCAL
#if defined(REX_LEAF_LOCALS_OFF)
#define REX_LEAF_LOCAL(type, name, reg) type& name = ctx.reg
#define REX_LEAF_STORE(name, reg) ((void)0)
#else
#define REX_LEAF_LOCAL(type, name, reg) type name = ctx.reg
#define REX_LEAF_STORE(name, reg) ctx.reg = name
#endif
#endif
'''


def type_de(reg):
    if reg.startswith('cr'):
        return 'PPCCRRegister'
    if reg == 'xer':
        return 'PPCXERRegister'
    if reg.startswith('v'):
        return 'PPCVRegister'
    return 'PPCRegister'


def ordre(reg):
    return (re.sub(r'\d', '', reg), int(re.sub(r'\D', '', reg) or 0))


def a_une_boucle(code):
    vues = set()
    for l in code:
        m = re.match(r'^(loc_[0-9A-Fa-f]+):', l)
        if m:
            vues.add(m.group(1))
        for g in re.findall(r'goto (loc_[0-9A-Fa-f]+);', l):
            if g in vues:
                return True
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gen', default=GEN)
    ap.add_argument('--verifier', action='store_true', help='compte sans écrire')
    ap.add_argument('--toutes', action='store_true', help='toutes les feuilles, pas seulement celles qui bouclent')
    a = ap.parse_args()
    fichiers = sorted(f for f in os.listdir(a.gen) if re.match(r'\w+_recomp\.\d+\.cpp$', f))
    pchs = [f for f in os.listdir(a.gen) if f.endswith('_pch.h')]
    if not fichiers or len(pchs) != 1:
        sys.exit('ERREUR : registres_locaux_feuilles : pas de *_recomp.N.cpp ou pas un seul *_pch.h dans %s' % a.gen)
    faites = deja = laissees = registres = lus = modifies = 0
    for f in fichiers:
        chemin = os.path.join(a.gen, f)
        lignes = open(chemin, encoding='utf-8').read().split('\n')
        sortie = []
        i = 0
        touche = False
        while i < len(lignes):
            ligne = lignes[i]
            sortie.append(ligne)
            if not FUNC.match(ligne):
                i += 1
                continue
            j = i + 1
            while j < len(lignes) and lignes[j] != '}':
                j += 1
            corps = lignes[i + 1:j]
            code = [l for l in corps if not l.lstrip().startswith('//')]
            texte = '\n'.join(code)
            i = j
            if 'REX_LEAF_LOCAL(' in texte:
                deja += 1
                sortie.extend(corps)
                continue
            regs = sorted({m.group(1) for m in REG.finditer(texte)}, key=ordre)
            if not regs or APPEL.search(texte) or (not a.toutes and not a_une_boucle(code)):
                sortie.extend(corps)
                continue
            if 'setjmp' in texte or 'longjmp' in texte or re.search(r'\bl_(?:r|f|v|cr)\d', texte) or \
                    not any(l.strip() == 'REX_FUNC_PROLOGUE();' for l in code):
                laissees += 1
                sortie.extend(corps)
                continue
            # Registres que la fonction peut avoir changés : r et f seulement par une affectation à un champ.
            reecrits = []
            for r in regs:
                if r[0] in 'rf' and r[1:].isdigit():
                    mentions = re.findall(r'\bctx\.' + r + r'\b(\.\w+)?(\s*(?:[-+*/|&^%]|<<|>>)?=(?!=))?', texte)
                    if all(champ and not affect for champ, affect in mentions) and \
                            not re.search(r'&\s*ctx\.' + r + r'\b', texte):
                        lus += 1
                        continue
                reecrits.append(r)
            synchro = ''.join(' REX_LEAF_STORE(l_%s, %s);' % (r, r) for r in reecrits).strip()
            nouveau = []
            for l in corps:
                if l.lstrip().startswith('//'):
                    nouveau.append(l)
                    continue
                l = REG.sub(lambda m: 'l_' + m.group(1), l)
                if synchro:
                    # Accolades : le code généré écrit aussi un retour comme corps d'un `if` sur une ligne.
                    l = RETOUR.sub('{ ' + synchro + ' return; }', l)
                nouveau.append(l)
                if l.strip() == 'REX_FUNC_PROLOGUE();':
                    for r in regs:
                        nouveau.append('\tREX_LEAF_LOCAL(%s, l_%s, %s);' % (type_de(r), r, r))
            if synchro:
                nouveau.append('\t' + synchro)  # sortir par la fin est aussi un retour
            sortie.extend(nouveau)
            faites += 1
            registres += len(regs)
            touche = True
        if touche:
            modifies += 1
            if not a.verifier:
                open(chemin, 'w', encoding='utf-8', newline='\n').write('\n'.join(sortie))
    pch = os.path.join(a.gen, pchs[0])
    texte = open(pch, encoding='utf-8').read()
    if MARQUE not in texte:
        ancre = '#define REX_MEMORY_SIZE'
        if ancre not in texte:
            sys.exit('ERREUR : registres_locaux_feuilles : pas de REX_MEMORY_SIZE dans %s' % pch)
        if not a.verifier:
            open(pch, 'w', encoding='utf-8', newline='\n').write(texte.replace(ancre, MACROS + ancre, 1))
    print('registres_locaux_feuilles%s : %d fonctions feuilles%s, %d registres en variables locales (%d seulement '
          'lus : pas réécrits), %d laissées, %d déjà faites ; %d fichiers modifiés' %
          (' (vérification)' if a.verifier else '', faites, '' if a.toutes else ' avec boucle', registres, lus,
           laissees, deja, modifies))


if __name__ == '__main__':
    main()

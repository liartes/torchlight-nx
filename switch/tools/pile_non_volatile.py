#!/usr/bin/env python3
"""Accès à la pile du thread sans volatile dans le code généré.

Le générateur écrit toutes les lectures et écritures en mémoire du jeu avec volatile (REX_LOAD_Uxx / REX_STORE_Uxx :
*(volatile u32*)(base + ...)) : le compilateur ne peut ni supprimer une relecture, ni garder une valeur en registre,
ni regrouper deux accès. C'est nécessaire pour la mémoire partagée entre threads (boucles d'attente du jeu), pas pour
la pile du thread (adresses relatives à r1 : sauvegardes de registres, variables locales, paramètres).

Cet outil remplace les accès de forme « r1 + déplacement » par REX_LOAD_STACK_Uxx / REX_STORE_STACK_Uxx et ajoute
leurs définitions à l'en-tête précompilé généré :
- avec REX_STACK_NON_VOLATILE (option CMake TORCHLIGHT_PILE_NON_VOLATILE) : accès ordinaires par des types
  may_alias (le PowerPC écrit parfois 8 octets puis en relit 4 : sans may_alias, le compilateur pourrait réordonner
  ces accès de types différents) et aligned(1) ;
- sinon : les macros volatile d'origine (comparaison sans régénérer).
Boucles d'attente : une boucle qui relit une variable de la pile modifiée par un autre thread, sans aucun appel de
fonction dans la boucle, ne relirait plus la mémoire (le compilateur sort la lecture de la boucle). Essai du
2026-10-02 sans précaution : interblocages au démarrage sur PC (sections critiques jamais relâchées, 3 lancements
sur 5 ; 0 sur 6 avec la pile volatile ; ces essais étaient faussés par la course de l'anneau de commandes du SDK,
corrigée le même jour : ensuite 10 lancements sur 10). Par précaution, les fonctions qui contiennent une boucle sans appel qui lit la pile
(416 fonctions, 675 boucles) gardent donc tous leurs accès volatile.

À refaire après chaque génération du code (build_switch.sh le fait ; sans effet si déjà fait).
Usage : pile_non_volatile.py [--annuler]
"""
import os
import re
import sys

RACINE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GEN = os.path.join(RACINE, 'switch', 'generated', 'default')
MARQUE = '// --- torchlight : accès à la pile (switch/tools/pile_non_volatile.py) ---'
DEFINITIONS = MARQUE + r'''
#if defined(REX_STACK_NON_VOLATILE)
typedef u8 __attribute__((may_alias, aligned(1))) rex_pile_u8;
typedef u16 __attribute__((may_alias, aligned(1))) rex_pile_u16;
typedef u32 __attribute__((may_alias, aligned(1))) rex_pile_u32;
typedef u64 __attribute__((may_alias, aligned(1))) rex_pile_u64;
#define REX_LOAD_STACK_U8(x) (*(rex_pile_u8*)(base + (u32)(x)))
#define REX_LOAD_STACK_U16(x) __builtin_bswap16(*(rex_pile_u16*)(base + (u32)(x)))
#define REX_LOAD_STACK_U32(x) __builtin_bswap32(*(rex_pile_u32*)(base + (u32)(x)))
#define REX_LOAD_STACK_U64(x) __builtin_bswap64(*(rex_pile_u64*)(base + (u32)(x)))
#define REX_STORE_STACK_U8(x, y) (*(rex_pile_u8*)(base + (u32)(x)) = (y))
#define REX_STORE_STACK_U16(x, y) (*(rex_pile_u16*)(base + (u32)(x)) = __builtin_bswap16(y))
#define REX_STORE_STACK_U32(x, y) (*(rex_pile_u32*)(base + (u32)(x)) = __builtin_bswap32(y))
#define REX_STORE_STACK_U64(x, y) (*(rex_pile_u64*)(base + (u32)(x)) = __builtin_bswap64(y))
#else
#define REX_LOAD_STACK_U8(x) REX_LOAD_U8(x)
#define REX_LOAD_STACK_U16(x) REX_LOAD_U16(x)
#define REX_LOAD_STACK_U32(x) REX_LOAD_U32(x)
#define REX_LOAD_STACK_U64(x) REX_LOAD_U64(x)
#define REX_STORE_STACK_U8(x, y) REX_STORE_U8(x, y)
#define REX_STORE_STACK_U16(x, y) REX_STORE_U16(x, y)
#define REX_STORE_STACK_U32(x, y) REX_STORE_U32(x, y)
#define REX_STORE_STACK_U64(x, y) REX_STORE_U64(x, y)
#endif
// --- fin des accès à la pile ---
'''

CHARGE = re.compile(r'\bREX_LOAD_U(8|16|32|64)\((ctx\.r1\.u32 \+ -?\d+)\)')
ECRIT = re.compile(r'\bREX_STORE_U(8|16|32|64)\((ctx\.r1\.u32 \+ -?\d+),')
CHARGE_PILE = re.compile(r'\bREX_LOAD_STACK_U(8|16|32|64)\(')
ECRIT_PILE = re.compile(r'\bREX_STORE_STACK_U(8|16|32|64)\(')


FONCTION = re.compile(r'^DEFINE_REX_FUNC\((sub_[0-9A-F]+)\) \{.*?\n\}', re.S | re.M)


def decouper(texte):
    """Découpe un fichier en (morceau, est_une_fonction)."""
    pos = 0
    for m in FONCTION.finditer(texte):
        if m.start() > pos:
            yield texte[pos:m.start()], False
        yield m.group(0), True
        pos = m.end()
    if pos < len(texte):
        yield texte[pos:], False


def boucle_sans_appel(corps):
    """Vrai si la fonction contient une boucle (saut vers une étiquette placée plus haut) sans aucun appel de
    fonction qui lit la pile : boucle d'attente possible sur une variable de la pile."""
    lignes = corps.split('\n')
    etiquettes = {}
    for i, l in enumerate(lignes):
        m = re.match(r'^(loc_[0-9A-F]+):', l)
        if m:
            etiquettes[m.group(1)] = i
    for i, l in enumerate(lignes):
        g = re.search(r'goto (loc_[0-9A-F]+);', l)
        if g and g.group(1) in etiquettes and etiquettes[g.group(1)] < i:
            boucle = '\n'.join(lignes[etiquettes[g.group(1)]:i + 1])
            if (CHARGE.search(boucle) or CHARGE_PILE.search(boucle)) and not re.search(r'\w+\(ctx, base\);', boucle):
                return True
    return False


def main():
    annuler = '--annuler' in sys.argv
    pch = os.path.join(GEN, 'torchlight_pch.h')
    texte = avant = open(pch, encoding='utf-8').read()
    if annuler:
        if MARQUE in texte:
            debut = texte.index(MARQUE)
            fin = texte.index('// --- fin des accès à la pile ---\n') + len('// --- fin des accès à la pile ---\n')
            texte = texte[:debut] + texte[fin:]
    elif MARQUE not in texte:
        ancre = '#define REX_MEMORY_SIZE'
        texte = texte.replace(ancre, DEFINITIONS + '\n' + ancre, 1)
    if texte != avant:  # sinon l'en-tête précompilé change de date et tout le code généré se recompile
        open(pch, 'w', encoding='utf-8', newline='\n').write(texte)
    total = fichiers = gardees = 0
    for f in sorted(os.listdir(GEN)):
        if not (f.startswith('torchlight_recomp.') and f.endswith('.cpp')):
            continue
        chemin = os.path.join(GEN, f)
        t = open(chemin, encoding='utf-8').read()
        if annuler:
            n = t.count('_STACK_U')
            nouveau = ECRIT_PILE.sub(r'REX_STORE_U\1(', CHARGE_PILE.sub(r'REX_LOAD_U\1(', t))
        else:
            morceaux, n = [], 0
            for corps, est_fonction in decouper(t):
                if est_fonction and not boucle_sans_appel(corps):
                    corps, n1 = CHARGE.subn(r'REX_LOAD_STACK_U\1(\2)', corps)
                    corps, n2 = ECRIT.subn(r'REX_STORE_STACK_U\1(\2,', corps)
                    n += n1 + n2
                elif est_fonction:
                    gardees += 1
                morceaux.append(corps)
            nouveau = ''.join(morceaux)
        if nouveau != t:
            open(chemin, 'w', encoding='utf-8', newline='\n').write(nouveau)
            fichiers += 1
        total += n
    print('%s : %d accès à la pile dans %d fichiers ; %d fonctions gardées volatile (boucle sans appel qui lit la '
          'pile)' % ('annulé' if annuler else 'pile', total, fichiers, gardees))


if __name__ == '__main__':
    main()

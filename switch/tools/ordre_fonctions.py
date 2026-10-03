#!/usr/bin/env python3
"""Ordre des fonctions pour l'éditeur de liens (méthode de nfsmw-nx, tools/calientes.py) : les fonctions qui
tournent le plus, d'après les profils des piles de la console, regroupées au début de .text (moins de défauts de
cache d'instructions et de TLB sur ~80 Mo de code).

Usage : ordre_fonctions.py sortie.ld rex_pilas.txt:ELF [rex_pilas.txt:ELF ...]
Poids d'une fonction : nombre d'échantillons (tous threads) où elle apparaît dans la pile. Seules les fonctions
vues dans au moins 2 échantillons sont listées.

Avec le PGO, GCC place les fonctions chaudes en .text.hot.<nom> (et les froides en .text.unlikely.<nom>) : chaque
fonction est listée sous les deux noms (.text.<nom> et .text.hot.<nom>).
Piège (nfsmw-nx, docs/toolchain.md) : le fichier passe avant le script de l'éditeur de liens ; la première ligne
de .text doit être KEEP (*(.crt0)), sinon le code de démarrage de libnx n'est plus à l'adresse 0 et le .nro ne
démarre pas. build_switch.sh vérifie que _start reste à 0.
"""
import collections
import re
import subprocess
import sys

if len(sys.argv) < 3:
    raise SystemExit(__doc__)
sortie = sys.argv[1]
poids = collections.Counter()
for arg in sys.argv[2:]:
    pilas, elf = arg.split(':', 1)
    echantillons = []
    for ligne in open(pilas, errors='ignore'):
        if '|' in ligne:
            echantillons.append([int(a, 16) for a in re.findall(r'[0-9a-fA-F]+', ligne.split('|', 1)[1])])
    adresses = sorted({a if i == 0 else a - 4 for e in echantillons for i, a in enumerate(e)})
    sortie_sym = subprocess.run(['llvm-symbolizer-20', '--obj=' + elf, '--functions=linkage', '--no-inlines',
                                 '--no-demangle'], input='\n'.join(hex(a) for a in adresses),
                                capture_output=True, text=True).stdout
    noms = {a: bloc.split('\n')[0] for a, bloc in zip(adresses, sortie_sym.strip('\n').split('\n\n'))}
    for e in echantillons:
        vus = {noms.get(a if i == 0 else a - 4, '??') for i, a in enumerate(e)}
        for n in vus:
            if n not in ('??', '_start'):
                poids[n] += 1

liste = [n for n, c in poids.most_common() if c >= 2]
lignes = ['/*',
          ' * Fonctions chaudes regroupées au début de .text (%d, les plus chaudes d\'abord), d\'après les profils' % len(liste),
          ' * des piles de la console. Généré par switch/tools/ordre_fonctions.py ; ne pas modifier à la main.',
          ' */',
          '.text : {',
          '  /* Code de démarrage de libnx : doit rester à l\'adresse 0 (sinon le .nro ne démarre pas). */',
          '  KEEP (*(.crt0))',
          '  /* Fonctions en assembleur sans section propre : par fichier. */',
          '  *libc.a:libc_a-memcpy.o(.text .text.*)',
          '  *libc.a:libc_a-memmove.o(.text .text.*)',
          '  *libc.a:libc_a-memset.o(.text .text.*)',
          '  *libc.a:libc_a-memcmp.o(.text .text.*)',
          '  *libc.a:libc_a-strcmp.o(.text .text.*)']
for n in liste:
    m = re.fullmatch(r'(?:__imp__)?(sub_[0-9A-F]{8})', n)
    if m:
        # Fonction recompilée (section .text.__imp__sub_X) ou notre interception (section .text.sub_X).
        lignes.append('  *(.text.__imp__{0} .text.hot.__imp__{0})'.format(m.group(1)))
        lignes.append('  *(.text.{0} .text.hot.{0})'.format(m.group(1)))
    elif n not in ('memcpy', 'memmove', 'memset', 'memcmp', 'strcmp'):
        lignes.append('  *(.text.{0} .text.hot.{0})'.format(n))
lignes.append('}')
open(sortie, 'w').write('\n'.join(lignes) + '\n')
print('%d fonctions chaudes, dont %d du jeu -> %s' % (len(liste), sum(1 for n in liste if 'sub_8' in n), sortie))

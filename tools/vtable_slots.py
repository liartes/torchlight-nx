#!/usr/bin/env python3
"""Affiche, pour chaque case d'une table virtuelle, les premières instructions PPC de la fonction.

Usage : vtable_slots.py <sortie de rtti_vtables.py avec filtre> [dossier generated] [debut] [fin]
"""
import glob, re, sys

vt = sys.argv[1]
gen = sys.argv[2] if len(sys.argv) > 2 else 'switch/generated/default'
lo = int(sys.argv[3]) if len(sys.argv) > 3 else 0
hi = int(sys.argv[4]) if len(sys.argv) > 4 else 10**9

slots = re.findall(r'\[\s*(\d+)\]\s+\+0x[0-9a-f]+\s+(sub_[0-9A-F]+)', open(vt).read())
bodies = {}
for f in glob.glob(gen + '/*_recomp.*.cpp'):
    t = open(f, errors='ignore').read()
    for m in re.finditer(r'DEFINE_REX_FUNC\((sub_[0-9A-F]+)\) \{(.*?)\n\}', t, re.S):
        bodies[m.group(1)] = re.findall(r'\t// ([a-z][a-z0-9.+-]*)( [^\n]*)?', m.group(2))

for i, name in slots:
    i = int(i)
    if not lo <= i <= hi:
        continue
    ins = bodies.get(name, [])
    txt = '; '.join((a + (o or '')).strip() for a, o in ins[:6])
    print(f'{i:3d} {name} [{len(ins):4d}] {txt[:150]}')

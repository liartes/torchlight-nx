#!/usr/bin/env python3
"""Trouve les fonctions recompilées qui chargent l'adresse d'une chaîne (lis rX,hi puis addi rY,rX,lo).

Usage : str_refs.py <texte> [image.bin] [dossier generated]
"""
import glob, re, sys

BASE = 0x82000000
texte = sys.argv[1].encode()
image = sys.argv[2] if len(sys.argv) > 2 else '/tmp/image.bin'
gen = sys.argv[3] if len(sys.argv) > 3 else 'switch/generated/default'
data = open(image, 'rb').read()

cibles = {}
for m in re.finditer(re.escape(texte), data):
    # début de la chaîne C qui contient le texte
    d = m.start()
    while d > 0 and data[d - 1] not in (0,):
        d -= 1
    a = BASE + d
    hi = ((a + 0x8000) >> 16) & 0xFFFF
    hi = hi - 0x10000 if hi >= 0x8000 else hi
    lo = a & 0xFFFF
    lo = lo - 0x10000 if lo >= 0x8000 else lo
    cibles[a] = (hi, lo, data[d:data.index(b'\0', d)][:80].decode(errors='replace'))

for a, (hi, lo, s) in cibles.items():
    print(f'chaîne {a:#010x} (lis {hi}, addi {lo}) : {s!r}')
    pat = re.compile(r'// lis (r\d+),' + str(hi) + r'\n(?:.*\n){0,40}?.*// addi r\d+,\1,' + str(lo) + r'\b')
    for f in glob.glob(gen + '/*_recomp.*.cpp'):
        t = open(f, errors='ignore').read()
        for fm in re.finditer(r'DEFINE_REX_FUNC\((sub_[0-9A-F]+)\) \{(.*?)\n\}', t, re.S):
            if pat.search(fm.group(2)):
                print('   référencée par', fm.group(1))

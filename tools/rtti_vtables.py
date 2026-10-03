#!/usr/bin/env python3
"""Retrouve les tables virtuelles des classes C++ (RTTI MSVC) dans l'image mémoire du jeu.

Usage : rtti_vtables.py [image.bin] [filtre]
  image.bin : image mémoire décompressée, base 0x82000000 (find_candidates.sh -> /tmp/image.bin)
  filtre    : sous-chaîne du nom de classe (ex. D3D9RenderSystem)

MSVC 32 bits big-endian : vtable[-1] -> Complete Object Locator
  COL = {signature, offset, cdOffset, pTypeDescriptor, pClassHierarchyDescriptor}
  TypeDescriptor = {pVFTable, spare, nom ".?AV...@@"}
"""
import re, struct, sys

BASE = 0x82000000
path = sys.argv[1] if len(sys.argv) > 1 else '/tmp/image.bin'
filtre = sys.argv[2] if len(sys.argv) > 2 else ''
data = open(path, 'rb').read()
CODE_LO, CODE_HI = 0x82190000, 0x83027448

def u32(off):
    return struct.unpack_from('>I', data, off)[0]

# Descripteurs de type : le nom commence 8 octets après le début.
tds = {}
for m in re.finditer(rb'\.\?A[VU][A-Za-z0-9_@?$]+@@\x00', data):
    td = m.start() - 8
    if td >= 0:
        tds[BASE + td] = m.group()[:-1].decode()

# Complete Object Locators : signature 0, pTypeDescriptor connu.
cols = {}
for off in range(0, len(data) - 20, 4):
    if u32(off) != 0:
        continue
    ptd = u32(off + 12)
    if ptd in tds:
        cols[BASE + off] = (tds[ptd], u32(off + 4))

# Tables virtuelles : mot précédent = adresse d'un COL, puis des pointeurs vers le code.
res = []
for off in range(4, len(data) - 4, 4):
    col = u32(off - 4)
    if col not in cols:
        continue
    name, this_off = cols[col]
    if filtre and filtre not in name:
        continue
    funcs = []
    o = off
    while o + 4 <= len(data):
        v = u32(o)
        if not (CODE_LO <= v < CODE_HI):
            break
        funcs.append(v)
        o += 4
    if funcs:
        res.append((name, this_off, BASE + off, funcs))

for name, this_off, vt, funcs in sorted(res):
    print(f'{name}  (décalage {this_off:#x})  vtable {vt:#010x}  {len(funcs)} virtuelles')
    if filtre:
        for i, f in enumerate(funcs):
            print(f'  [{i:3d}] +{i*4:#05x}  sub_{f:08X}')

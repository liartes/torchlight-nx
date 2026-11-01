#!/usr/bin/env python3
"""Boucles infinies que GCC aurait faites de boucles d'attente du jeu (ELF de la Switch, AArch64).

D'après find_self_loops.py de renut-nx / sa2-nx / futari-nx (GPL-3.0). Une boucle qui ne fait que relire la mémoire
du jeu (while (mot == 0) {}) attend vraiment sur la Xbox 360 : un autre thread ou le GPU change le mot. Sans volatile
(switch/tools/memoire_non_volatile.py), si une barrière manque, le compilateur sort la lecture de la boucle, qui
devient un saut sur lui-même (b ., b.cond ., cbz/cbnz ., tbz/tbnz .) : le thread tourne à 100 % pour toujours.
Liste chaque saut sur lui-même du code de l'ELF avec sa fonction, pour comparer deux builds.

Usage : boucles_infinies.py <elf> [autre elf...]
"""
import bisect
import struct
import subprocess
import sys

NM = 'llvm-nm-20'


def sections(path):
    data = open(path, 'rb').read()
    assert data[:4] == b'\x7fELF' and data[4] == 2
    shoff, = struct.unpack_from('<Q', data, 0x28)
    shentsize, shnum = struct.unpack_from('<HH', data, 0x3A)
    out = []
    for i in range(shnum):
        name, type_, flags, addr, offset, size = struct.unpack_from('<IIQQQQ', data, shoff + i * shentsize)
        if type_ == 1 and flags & 0x4 and size:  # PROGBITS exécutable
            out.append((addr, data[offset:offset + size]))
    return out


def symbols(path):
    text = subprocess.run([NM, '-n', path], capture_output=True, text=True).stdout
    addrs, names = [], []
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in 'tTwW':
            addrs.append(int(parts[0], 16))
            names.append(parts[2])
    return addrs, names


def self_branches(code):
    for i in range(0, len(code) - 3, 4):
        w, = struct.unpack_from('<I', code, i)
        if w == 0x14000000:
            yield i, 'b .'
        elif (w & 0xFF000010) == 0x54000000 and ((w >> 5) & 0x7FFFF) == 0:
            yield i, 'b.cond .'
        elif (w & 0x7E000000) == 0x34000000 and ((w >> 5) & 0x7FFFF) == 0:
            yield i, 'cbz/cbnz .'
        elif (w & 0x7E000000) == 0x36000000 and ((w >> 5) & 0x3FFF) == 0:
            yield i, 'tbz/tbnz .'


def scan(path):
    addrs, names = symbols(path)
    found = {}
    for base, code in sections(path):
        for off, kind in self_branches(code):
            a = base + off
            k = bisect.bisect_right(addrs, a) - 1
            found.setdefault(names[k] if k >= 0 else '?', []).append((a, kind))
    return found


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for p in sys.argv[1:]:
        found = scan(p)
        guest = {n: v for n, v in found.items() if 'sub_' in n}
        print('== %s : %d sauts sur eux-mêmes, %d dans des fonctions recompilées'
              % (p, sum(len(v) for v in found.values()), len(guest)))
        for n, v in sorted(guest.items()):
            print('   %s : %s' % (n, ', '.join('%#x %s' % (a, k) for a, k in v)))


if __name__ == '__main__':
    main()

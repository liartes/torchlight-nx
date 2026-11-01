#!/usr/bin/env python3
"""Test de switch/tools/decalages_rlwinm.py : pour chaque paire (décalage, masque) du code généré (ou toutes les
paires de masques sans tour si --toutes), compare l'expression d'origine (rotation 64 bits du mot dupliqué, comme le
générateur) et l'expression réécrite, pour rlwinm et rlwimi, sur des entrées 64 bits aléatoires et particulières
(bits du haut quelconques). Compile avec le compilateur de l'hôte et l'exécute.
Usage : test_decalages_rlwinm.py [--toutes]
"""
import os
import random
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import decalages_rlwinm as d  # noqa: E402


def masque(mb, me):
    return sum(1 << (31 - i) for i in range(mb, me + 1))


def main():
    if '--toutes' in sys.argv:
        paires = {(sh, masque(mb, me)) for sh in range(32) for mb in range(32) for me in range(mb, 32)}
    else:
        compte = {'rlwinm': 0, 'rlwimi': 0, 'gardes': 0, 'paires': set()}
        for f in sorted(os.listdir(d.GEN)):
            if f.startswith('torchlight_recomp.') and f.endswith('.cpp'):
                d.reecrire(open(os.path.join(d.GEN, f), encoding='utf-8').read(), compte)
        paires = compte['paires']
        if not paires:
            sys.exit('aucune paire : le code généré est déjà réécrit (lancer avec --toutes)')
    lignes = ['#include <bit>', '#include <cstdint>', '#include <cstdio>',
              '#define __builtin_rotateleft64(x, n) std::rotl(static_cast<uint64_t>(x), static_cast<int>(n))',
              '#ifndef __clang__', '#define __builtin_rotateleft32(x, n) std::rotl(static_cast<uint32_t>(x), '
              'static_cast<int>(n))', '#endif',
              'struct R { union { uint64_t u64; uint32_t u32; }; };',
              'static const uint64_t E[] = {%s};' % ', '.join(
                  '0x%016XULL' % v for v in [0, ~0 & (2**64 - 1), 0x8000000000000001, 0x00000000FFFFFFFF,
                                             0xFFFFFFFF00000000, 0x123456789ABCDEF0] +
                  [random.getrandbits(64) for _ in range(58)]),
              'int main() { long n = 0, faux = 0;',
              ' for (uint64_t a : E) for (uint64_t b : E) { R s, x; s.u64 = a; x.u64 = b; R r1, r2;']
    for sh, m in sorted(paires):
        m2 = (~m) & (2**64 - 1)
        new = d.expression('s', sh, m)
        lignes.append('  r1.u64 = __builtin_rotateleft64(s.u32 | (s.u64 << 32), %d) & 0x%X; r2.u64 = %s; '
                      'n++; if (r1.u64 != r2.u64) { faux++; if (faux < 10) printf("rlwinm %d 0x%X\\n"); }'
                      % (sh, m, new, sh, m))
        lignes.append('  r1.u64 = (__builtin_rotateleft64(s.u32 | (s.u64 << 32), %d) & 0x%X) | (x.u64 & 0x%X); '
                      'r2.u64 = uint64_t%s | (x.u64 & 0x%X); n++; if (r1.u64 != r2.u64) { faux++; '
                      'if (faux < 10) printf("rlwimi %d 0x%X\\n"); }' % (sh, m, m2, new, m2, sh, m))
    lignes += [' }', ' printf("%ld comparaisons, %ld différences\\n", n, faux); return faux != 0; }']
    with tempfile.TemporaryDirectory() as t:
        src, exe = os.path.join(t, 't.cpp'), os.path.join(t, 't')
        open(src, 'w').write('\n'.join(lignes) + '\n')
        subprocess.run(['clang++-20', '-std=c++20', '-O0', '-o', exe, src], check=True)
        r = subprocess.run([exe])
    print('%d paires décalage/masque' % len(paires))
    sys.exit(r.returncode)


if __name__ == '__main__':
    main()

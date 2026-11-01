#!/usr/bin/env python3
"""rlwinm / rlwimi du code généré en décalages et extractions simples (d'après rotate_word_shifts de renut-nx /
banjo-nx, GPL-3.0 : -0,54 % pondéré par leur profil de console).

Le générateur écrit rlwinm rA,rS,SH,MB,ME comme une rotation 64 bits du mot dupliqué :
    rA.u64 = __builtin_rotateleft64(rS.u32 | (rS.u64 << 32), SH) & M;
et rlwimi comme la même rotation insérée sous masque :
    rA.u64 = (__builtin_rotateleft64(rS.u32 | (rS.u64 << 32), SH) & M) | (rA.u64 & M2);
Quand le masque M ne fait pas le tour (M <= 0xFFFFFFFF), seuls les 32 bits du bas comptent, et
rotl64(dup(x), SH) & M == rotl32(x, SH) & M. Selon les bits que M garde :
  - aucun des SH bits du bas (venus du haut du mot) : (uint32_t)(x << SH) & M ;
  - aucun des bits SH et plus (venus du bas du mot) : (x >> (32 - SH)) & M ;
  - sinon : __builtin_rotateleft32(x, SH) & M.
Mêmes valeurs pour toute entrée (switch/tools/test_decalages_rlwinm.py, sur toutes les paires SH/M du code) ; GCC en
fait une instruction (lsl, lsr, ubfx, ubfiz ; bfi pour rlwimi) là où la rotation en prenait deux à quatre.
Les masques qui font le tour (MB > ME : bits du haut du registre aussi) ne sont pas touchés.

À refaire après chaque génération du code (build_switch.sh le fait) ; idempotent (une ligne réécrite ne correspond
plus au motif). Pour revenir au code d'origine : régénérer le code. Usage : decalages_rlwinm.py [--compter]
"""
import os
import re
import sys

RACINE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GEN = os.path.join(RACINE, 'switch', 'generated', 'default')
REG = r'(?:ctx\.|l_|ll_)?r\d+'
ROT = r'__builtin_rotateleft64\((' + REG + r')\.u32 \| \((' + REG + r')\.u64 << 32\), (\d+)\)'
RLWINM = re.compile(r'^(\t+)(' + REG + r')\.u64 = ' + ROT + r' & (0x[0-9A-F]+|\d+);$', re.M)
RLWIMI = re.compile(r'^(\t+)(' + REG + r')\.u64 = \(' + ROT + r' & (0x[0-9A-F]+|\d+)\) \| \((' + REG +
                    r')\.u64 & (0x[0-9A-F]+|\d+)\);$', re.M)


def expression(src, sh, m):
    """Expression 32 bits de rotl32(src, sh) & m (m <= 0xFFFFFFFF), ou None."""
    if m > 0xFFFFFFFF:
        return None
    x = '%s.u32' % src
    if sh == 0:
        return '(%s & 0x%Xu)' % (x, m)
    if m & ((1 << sh) - 1) == 0:
        return '(uint32_t(%s << %d) & 0x%Xu)' % (x, sh, m)
    if m >> sh == 0:
        return '((%s >> %d) & 0x%Xu)' % (x, 32 - sh, m)
    return '(__builtin_rotateleft32(%s, %d) & 0x%Xu)' % (x, sh, m)


def reecrire(texte, compte):
    def rlwinm(mo):
        tab, dst, s1, s2, sh, m = mo.groups()
        e = expression(s1, int(sh), int(m, 0)) if s1 == s2 else None
        if e is None:
            compte['gardes'] += 1
            return mo.group(0)
        compte['rlwinm'] += 1
        compte['paires'].add((int(sh), int(m, 0)))
        return '%s%s.u64 = %s;' % (tab, dst, e)

    def rlwimi(mo):
        tab, dst, s1, s2, sh, m, dst2, m2 = mo.groups()
        e = expression(s1, int(sh), int(m, 0)) if s1 == s2 else None
        if e is None or dst2 != dst:
            compte['gardes'] += 1
            return mo.group(0)
        compte['rlwimi'] += 1
        compte['paires'].add((int(sh), int(m, 0)))
        return '%s%s.u64 = uint64_t%s | (%s.u64 & %s);' % (tab, dst, e, dst, m2)

    return RLWIMI.sub(rlwimi, RLWINM.sub(rlwinm, texte))


def main():
    compter = '--compter' in sys.argv
    compte = {'rlwinm': 0, 'rlwimi': 0, 'gardes': 0, 'paires': set()}
    fichiers = 0
    for f in sorted(os.listdir(GEN)):
        if not (f.startswith('torchlight_recomp.') and f.endswith('.cpp')):
            continue
        chemin = os.path.join(GEN, f)
        t = open(chemin, encoding='utf-8').read()
        nouveau = reecrire(t, compte)
        if nouveau != t:
            fichiers += 1
            if not compter:
                open(chemin, 'w', encoding='utf-8', newline='\n').write(nouveau)
    print('decalages_rlwinm : %d rlwinm et %d rlwimi réécrits (%d paires décalage/masque), %d gardés (masque qui '
          'fait le tour), %d fichiers %s' % (compte['rlwinm'], compte['rlwimi'], len(compte['paires']),
                                              compte['gardes'], fichiers, 'à modifier' if compter else 'modifiés'))
    return compte


if __name__ == '__main__':
    main()

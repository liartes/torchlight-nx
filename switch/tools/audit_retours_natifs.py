#!/usr/bin/env python3
"""Appelants d'une fonction native qui liraient un registre volatil laissé par l'original.

Le compilateur de la Xbox 360 (LTCG) connaît parfois les registres que l'appelé ne touche pas et garde des valeurs dedans à
travers un appel (ex. r10 à travers sub_821AFE28). Une native n'écrit que r3 ou f1 et laisse le reste intact : ce premier cas
reste juste. Le danger inverse est un appelant qui lit après l'appel un registre que l'original écrivait : on le cherche
ici. Pour chaque « bl <native> » du code généré (commentaires), les instructions suivantes sont parcourues en ligne droite
jusqu'à un saut ou une étiquette ; tout registre volatil lu avant d'être écrit est signalé, sauf r3/f1 (résultat) et r1.

  python3 switch/tools/audit_retours_natifs.py sub_821AFE28 sub_821CC700 ...   (sans argument : les REX_HOOK_RAW de la fin de
  src/torchlight_ogre_natif.cpp, depuis le marqueur « Top 50 »)
"""
import os
import re
import sys

ICI = os.path.dirname(os.path.abspath(__file__))
RACINE = os.path.join(ICI, '..', '..')
G = os.path.join(ICI, '..', 'generated', 'default')
VOLATILS = {'r%d' % i for i in [0] + list(range(4, 13))} | {'f%d' % i for i in [0] + list(range(2, 14))} | \
           {'v%d' % i for i in range(14)} | {'cr0', 'cr1', 'cr5', 'cr6', 'cr7', 'ctr'}
REG = re.compile(r'\b(r\d+|f\d+|v\d+|cr\d)\b')


def fonctions_cibles():
    if sys.argv[1:]:
        return set(sys.argv[1:])
    texte = open(os.path.join(RACINE, 'src', 'torchlight_ogre_natif.cpp'), encoding='utf-8').read()
    return set(re.findall(r'REX_HOOK_RAW\((sub_[0-9A-F]+)\)', texte[texte.index('// Top 50 du thread principal'):]))


def lus_ecrits(mn, ops):
    regs = REG.findall(ops)
    if mn.startswith(('st', 'cmp', 'fcmp', 'mt', 'dcb', 'tw', 'td')):
        lus, ecrits = regs, []
        if mn.startswith(('cmp', 'fcmp')) and regs and regs[0].startswith('cr'):
            lus, ecrits = regs[1:], [regs[0]]
        elif mn.startswith(('cmp', 'fcmp')):
            ecrits = ['cr0']
        if mn.startswith('mtctr'):
            ecrits = ['ctr']
        return lus, ecrits
    if mn.startswith('rlwimi') or mn.startswith('insr'):
        return regs, regs[:1]
    if not regs:
        return [], []
    ecrits = regs[:1]
    if mn.endswith('.'):
        ecrits.append('cr0')
    return regs[1:], ecrits


def main():
    cibles = fonctions_cibles()
    adresses = {'0x' + c[4:].lower(): c for c in cibles}
    signales = {}
    sites = 0
    for nom in sorted(os.listdir(G)):
        if not (nom.startswith('torchlight_recomp.') and nom.endswith('.cpp')):
            continue
        lignes = [l.strip() for l in open(os.path.join(G, nom), encoding='utf-8', errors='replace')
                  if l.lstrip().startswith(('//', 'loc_', 'DEFINE_REX_FUNC'))]
        fonction = '?'
        for i, l in enumerate(lignes):
            if l.startswith('DEFINE_REX_FUNC'):
                fonction = l[16:-1].split(')')[0]
                continue
            m = re.fullmatch(r'// bl (0x[0-9a-f]+)', l)
            if not m or m.group(1) not in adresses:
                continue
            sites += 1
            ecrits = set()
            for suite in lignes[i + 1:i + 40]:
                if suite.startswith(('loc_', 'DEFINE_REX_FUNC')) or not suite.startswith('// '):
                    break
                t = suite[3:].split(None, 1)
                mn, ops = t[0], t[1] if len(t) > 1 else ''
                if mn in ('bl', 'bctrl', 'blr', 'b', 'bctr') or re.match(r'b(eq|ne|lt|le|gt|ge|dnz)', mn):
                    # Saut conditionnel : il lit son registre de condition.
                    c = REG.findall(ops)
                    if re.match(r'b(eq|ne|lt|le|gt|ge)', mn):
                        cr = c[0] if c else 'cr0'
                        if cr in VOLATILS and cr not in ecrits:
                            signales.setdefault((adresses[m.group(1)], cr), set()).add(fonction)
                    break
                lus, ecr = lus_ecrits(mn, ops)
                for r in lus:
                    if r in VOLATILS and r not in ecrits:
                        signales.setdefault((adresses[m.group(1)], r), set()).add(fonction)
                ecrits.update(ecr)
    print('%d fonctions, %d sites d\'appel direct' % (len(cibles), sites))
    if not signales:
        print('aucune lecture suspecte')
    for (c, r), fs in sorted(signales.items()):
        print('%s : %s lu après l\'appel par %s' % (c, r, ' '.join(sorted(fs)[:8])))
    return 0


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""fmadds dont les trois entrées sont prouvées simple précision : sans le test de REX_FMADDS (d'après M-24 du port 136
et single_floats.py de renut_nx_ultimate_base, sasrt-nx et blur-nx).

switch/tools/fmadds_simples.py écrit les fmadds / fmsubs / fnmadds / fnmsubs sous la forme REX_FMADDS(a, b, c) : une
fma simple précision si les trois entrées sont des float exacts, la forme double sinon. Là où le compilateur ne voit
pas d'où viennent les valeurs (registres lus dans ctx), le test reste : une quinzaine d'instructions. Cette étape
prouve sur le code du jeu que les trois entrées sont des float exacts et écrit alors REX_FMADDS_KNOWN(a, b, c), la
seule branche vraie du test : même résultat, au bit près, par construction. frsp d'une valeur prouvée simple devient
une copie (REX_FRSP_SIMPLE : double(float(x)) == x pour un float exact).

Analyse « doit » par fonction, vers l'avant (rencontre = intersection), sur les instructions du jeu (commentaires
« // mnémonique opérandes » du code généré) :
- un registre est simple après lfs / lfsx / lfsu / lfsux, fadds / fsubs / fmuls / fdivs / fsqrts / fres, les quatre
  fmadds et frsp, si la ligne qui l'écrit a bien la forme attendue (double(float(...)), double(REX_LOAD_F32(...)),
  double(temp.f32), REX_FMADDS...) ; après fmr / fneg / fabs / fnabs d'un simple ; après fsel de deux simples ;
- rien n'est simple à l'entrée de la fonction ; un appel (toute ligne avec « (ctx, base) ») oublie tout ; toute autre
  écriture d'un registre flottant l'efface ; une fonction avec setjmp est laissée telle quelle ;
- arêtes : chaque goto vers son étiquette, et la chute vers l'instruction suivante sauf après un goto ou un return sans
  condition (une arête de trop ne rend l'analyse que plus prudente) ; code jamais atteint : rien de simple.
Vérification : -DTORCHLIGHT_VERIF_SIMPLES (build PC) fait vérifier chaque REX_FMADDS_KNOWN et REX_FRSP_SIMPLE à
l'exécution (TorchlightPreuveSimpleFausse, src/torchlight_fmadds.cpp : compteur et journal).
-DREX_FMADDS_DOUBLE_ARRONDI : forme double partout, comme fmadds_simples.py.

À lancer après fmadds_simples.py. Idempotent ; --annuler remet REX_FMADDS et double(float(x)).
  python3 switch/tools/fmadds_prouvees.py [--gen switch/generated/default] [--verifier] [--annuler]
"""
import argparse
import glob
import os
import re
import sys

TOUS = (1 << 32) - 1
COMMENTAIRE = re.compile(r'^\t// ([a-z][\w.]*)\s*(.*)$')
ETIQUETTE = re.compile(r'^(loc_[0-9A-F]+):$')
GOTO = re.compile(r'goto (loc_[0-9A-F]+);')
DEBUT = re.compile(r'^(?:DEFINE_REX_FUNC|PPC_FUNC_IMPL)\(')
APPEL = re.compile(r'\(ctx, base\)|longjmp|setjmp')
FREG = re.compile(r'^f(\d+)$')
# Écriture d'un registre flottant (toutes les formes de nom : ctx.fN, l_fN, ll_fN, fN).
ECRITURE = re.compile(r'^\t+(?:ctx\.|ll_|l_)?f(\d+)\.(?:f64|u64|s64|u32|f32)\s*=(?!=)')
VAL = r'(?:ctx\.|ll_|l_)?f(\d+)\.f64'
SIMPLE_RHS = re.compile(r'^\t+(?:ctx\.|ll_|l_)?f(\d+)\.f64 = (?:double\(float\(|double\(REX_LOAD_F32\(|double\(temp\.f32\);|'
                        r'\(?-?REX_FMADDS(?:_KNOWN)?\(|REX_FRSP_SIMPLE\()')
COPIE = re.compile(r'^\t+(?:ctx\.|ll_|l_)?f(\d+)\.f64 = ' + VAL + r';$')
SIGNE = re.compile(r'^\t+(?:ctx\.|ll_|l_)?f(\d+)\.u64 = \(?(?:ctx\.|ll_|l_)?f(\d+)\.u64 (?:\^|\||&) ~?0x8000000000000000(?:ull|ULL)?\)?;$')
FMADDS = re.compile(r'^(\t+(?:ctx\.|ll_|l_)?f\d+\.f64 = \(?-?)REX_FMADDS\((-?' + VAL + r'), (-?' + VAL + r'), (-?' + VAL +
                    r')\)(\)?;)$')
FRSP = re.compile(r'^(\t+(?:ctx\.|ll_|l_)?f\d+\.f64 = )double\(float\(((?:ctx\.|ll_|l_)?f(\d+)\.f64)\)\);$')
UNDO_FMADDS = re.compile(r'REX_FMADDS_KNOWN\(')
UNDO_FRSP = re.compile(r'REX_FRSP_SIMPLE\(([^()]*)\)')

PRODUIT = {'lfs', 'lfsx', 'lfsu', 'lfsux', 'fadds', 'fsubs', 'fmuls', 'fdivs', 'fsqrts', 'fres', 'fmadds', 'fmsubs',
           'fnmadds', 'fnmsubs', 'frsp'}
FUSIONS = {'fmadds', 'fmsubs', 'fnmadds', 'fnmsubs'}
COPIES = {'fmr', 'fneg', 'fabs', 'fnabs'}

MARQUE = '// torchlight : fmadds aux entrées prouvées simples (switch/tools/fmadds_prouvees.py)'
MACROS = MARQUE + '''
#ifndef REX_FMADDS_KNOWN
#if defined(TORCHLIGHT_VERIF_SIMPLES)
extern "C" void TorchlightPreuveSimpleFausse(const char* fonction, double a, double b, double c);
[[gnu::always_inline]] static inline double RexVerifSimple(const char* f, double a, double b, double c) noexcept {
  if (!(RexEstFloatExact(a) & RexEstFloatExact(b) & RexEstFloatExact(c))) TorchlightPreuveSimpleFausse(f, a, b, c);
  return a;
}
#define REX_FMADDS_KNOWN(a, b, c) (RexVerifSimple(__func__, (a), (b), (c)), REX_FMADDS((a), (b), (c)))
#define REX_FRSP_SIMPLE(a) (RexVerifSimple(__func__, (a), 0.0, 0.0), double(float(a)))
#elif defined(REX_FMADDS_DOUBLE_ARRONDI)
#define REX_FMADDS_KNOWN(a, b, c) REX_FMADDS((a), (b), (c))
#define REX_FRSP_SIMPLE(a) double(float(a))
#else
#define REX_FMADDS_KNOWN(a, b, c) double(__builtin_fmaf(float(a), float(b), float(c)))
#define REX_FRSP_SIMPLE(a) (a)
#endif
#endif  // REX_FMADDS_KNOWN
// (fin des macros de switch/tools/fmadds_prouvees.py)
'''


def freg(op):
    m = FREG.match(op.strip())
    return int(m.group(1)) if m else None


def fonctions(lignes):
    """(début, fin) de chaque fonction : de DEFINE_REX_FUNC à la ligne « } » de colonne 0."""
    i = 0
    while i < len(lignes):
        if DEBUT.match(lignes[i]):
            j = i + 1
            while j < len(lignes) and lignes[j] != '}':
                j += 1
            yield i, j
            i = j
        i += 1


def analyser(lignes, debut, fin, stats):
    """Réécrit les lignes de la fonction [debut, fin) en place."""
    corps = range(debut + 1, fin)
    if any(APPEL.search(lignes[k]) and ('setjmp' in lignes[k] or 'longjmp' in lignes[k]) for k in corps):
        stats['setjmp'] += 1
        return
    # Instructions : (mnémonique, opérandes, [indices des lignes]) ; étiquettes : nom -> indice de l'instruction suivante.
    instr, etiq = [], {}
    courante = None
    for k in corps:
        l = lignes[k]
        m = ETIQUETTE.match(l)
        if m:
            etiq[m.group(1)] = len(instr)
            courante = None
            continue
        m = COMMENTAIRE.match(l)
        if m:
            courante = [m.group(1), m.group(2), []]
            instr.append(courante)
            continue
        if courante is not None:
            courante[2].append(k)
    n = len(instr)
    if not n:
        return
    succ = [[] for _ in range(n)]
    for i, (mn, ops, ks) in enumerate(instr):
        chute = True
        for k in ks:
            l = lignes[k]
            for g in GOTO.findall(l):
                if g in etiq and etiq[g] < n:
                    succ[i].append(etiq[g])
                elif g not in etiq:
                    stats['goto_inconnu'] += 1
            s = l.strip()
            if l.startswith('\t') and not l.startswith('\t\t') and (
                    s.startswith('goto ') or s == 'return;' or (s.startswith('{') and s.endswith('return; }'))):
                chute = False
        if chute and i + 1 < n:
            succ[i].append(i + 1)
    entree = [TOUS] * n
    atteint = [False] * n
    entree[0] = 0
    atteint[0] = True
    sorties = [None] * n

    def transfert(i, e):
        mn, ops, ks = instr[i]
        base = mn.rstrip('.')
        parties = [o.strip() for o in ops.split(',')] if ops.strip() else []
        if any(APPEL.search(lignes[k]) for k in ks):
            return 0
        ecrits = set()
        prouve = set()
        for k in ks:
            l = lignes[k]
            m = ECRITURE.match(l)
            if not m:
                continue
            r = int(m.group(1))
            ecrits.add(r)
            dest = freg(parties[0]) if parties else None
            if dest != r:
                continue
            if base in PRODUIT and SIMPLE_RHS.match(l):
                prouve.add(r)
            elif base == 'fmr' and len(parties) > 1:
                c = COPIE.match(l)
                if c and int(c.group(2)) == freg(parties[1]) and (e >> freg(parties[1])) & 1:
                    prouve.add(r)
            elif base in ('fneg', 'fabs', 'fnabs') and len(parties) > 1:
                c = SIGNE.match(l)
                src = freg(parties[1])
                if c and int(c.group(2)) == src and (e >> src) & 1:
                    prouve.add(r)
            elif base == 'fsel' and len(parties) == 4:
                b, c = freg(parties[2]), freg(parties[3])
                if b is not None and c is not None and (e >> b) & 1 and (e >> c) & 1:
                    prouve.add(r)
        for r in ecrits:
            e &= ~(1 << r)
        for r in prouve:
            # une écriture du même registre par une autre ligne de l'instruction après celle prouvée l'aurait effacé :
            # les formes du générateur n'en ont qu'une ; par prudence, une seule écriture exigée.
            if sum(1 for k in ks if (m := ECRITURE.match(lignes[k])) and int(m.group(1)) == r) == 1:
                e |= 1 << r
        return e

    travail = [0]
    while travail:
        i = travail.pop()
        s = transfert(i, entree[i])
        if sorties[i] == s:
            continue
        sorties[i] = s
        for j in succ[i]:
            nouveau = entree[j] & s if atteint[j] else s
            if not atteint[j] or nouveau != entree[j]:
                atteint[j] = True
                entree[j] = nouveau
                travail.append(j)
    for i, (mn, ops, ks) in enumerate(instr):
        base = mn.rstrip('.')
        if base not in FUSIONS and base != 'frsp':
            continue
        if not atteint[i]:
            continue
        e = entree[i]
        parties = [o.strip() for o in ops.split(',')]
        regs = [freg(p) for p in parties[1:]]
        if None in regs or not regs:
            continue
        if not all((e >> r) & 1 for r in regs):
            stats['non_prouve_' + ('frsp' if base == 'frsp' else 'fma')] += 1
            continue
        for k in ks:
            l = lignes[k]
            if base in FUSIONS:
                m = FMADDS.match(l)
                if m:
                    # Les opérandes C++ doivent être ceux de l'instruction (même ordre que le générateur : a, c, b).
                    noms = [int(m.group(3)), int(m.group(5)), int(m.group(7))]
                    if sorted(noms) != sorted(regs):
                        stats['incoherent'] += 1
                        break
                    lignes[k] = '%sREX_FMADDS_KNOWN(%s, %s, %s)%s' % (m.group(1), m.group(2), m.group(4), m.group(6),
                                                                     m.group(8))
                    stats['fma'] += 1
                    break
            else:
                m = FRSP.match(l)
                if m and int(m.group(3)) == regs[0]:
                    lignes[k] = '%sREX_FRSP_SIMPLE(%s);' % (m.group(1), m.group(2))
                    stats['frsp'] += 1
                    break


def main():
    ici = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', default=os.path.join(ici, '..', 'generated', 'default'))
    ap.add_argument('--verifier', action='store_true', help='compter seulement, ne rien écrire')
    ap.add_argument('--annuler', action='store_true', help='remettre REX_FMADDS et double(float(x))')
    a = ap.parse_args()
    pchs = glob.glob(os.path.join(a.gen, '*_pch.h'))
    if len(pchs) != 1:
        sys.exit('ERREUR : fmadds_prouvees : pas un seul *_pch.h dans %s' % a.gen)
    import collections
    stats = collections.Counter()
    for chemin in sorted(glob.glob(os.path.join(a.gen, '*_recomp.*.cpp'))):
        texte = open(chemin, encoding='utf-8').read()
        if a.annuler:
            nouveau = UNDO_FMADDS.sub('REX_FMADDS(', texte)
            nouveau = UNDO_FRSP.sub(r'double(float(\1))', nouveau)
            stats['annules'] += texte.count('REX_FMADDS_KNOWN(') + texte.count('REX_FRSP_SIMPLE(')
        else:
            if 'REX_FMADDS(' not in texte and 'double(float(' not in texte:
                continue
            lignes = texte.split('\n')
            for d, f in fonctions(lignes):
                analyser(lignes, d, f, stats)
            nouveau = '\n'.join(lignes)
            stats['deja'] += texte.count('REX_FMADDS_KNOWN(') + texte.count('REX_FRSP_SIMPLE(')
        if nouveau != texte:
            stats['fichiers'] += 1
            if not a.verifier:
                open(chemin, 'w', encoding='utf-8', newline='\n').write(nouveau)
    texte = open(pchs[0], encoding='utf-8').read()
    if not a.verifier and not a.annuler and MARQUE not in texte:
        ancre = '// torchlight : chargements flottants inversés dans l\'unité vectorielle'
        if ancre not in texte or 'RexEstFloatExact' not in texte:
            sys.exit('ERREUR : fmadds_prouvees : macros de fmadds_simples.py ou ancre absentes de %s' % pchs[0])
        open(pchs[0], 'w', encoding='utf-8', newline='\n').write(texte.replace(ancre, MACROS + ancre, 1))
    if a.annuler:
        print('fmadds_prouvees : %d formes remises (%d fichiers)' % (stats['annules'], stats['fichiers']))
    else:
        print('fmadds_prouvees%s : %d fmadds et %d frsp aux entrées prouvées simples (%d fichiers ; %d déjà sous cette '
              'forme) ; non prouvées : %d fmadds, %d frsp ; incohérences %d, goto inconnus %d, fonctions avec setjmp %d'
              % (' (vérification)' if a.verifier else '', stats['fma'], stats['frsp'], stats['fichiers'],
                 stats['deja'], stats['non_prouve_fma'], stats['non_prouve_frsp'], stats['incoherent'],
                 stats['goto_inconnu'], stats['setjmp']))


if __name__ == '__main__':
    main()

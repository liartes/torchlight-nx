#!/usr/bin/env python3
"""Registres du jeu en variables locales C++ le temps d'une boucle sans appel, dans les fonctions qui ne sont pas des
feuilles. D'après tools/loop_locals.py de sa2-nx (https://github.com/ChanseyIsTheBest/sa2-nx, GPL-3.0), adapté à
Torchlight : sans barrières de boucle chez nous, un début de boucle est une étiquette visée par un saut placé plus
bas dans la fonction (sa2-nx exigeait sa barrière juste après l'étiquette).

Les registres qu'une fonction garde dans le PPCContext (arguments r3-r10, f1-f13, v0-v13 et ce qu'elle mentionne comme
ctx.<registre>) sont relus depuis ctx après chaque écriture dans la mémoire du jeu (accès volatile : le compilateur ne
peut pas prouver qu'ils ne touchent pas ctx) ; dans une boucle, à chaque tour. switch/tools/registres_locaux_feuilles.py
le règle pour les fonctions feuilles ; cette étape le fait pour les boucles des autres fonctions, tant que la boucle
elle-même n'appelle rien :

        REX_LOOP_IN(ll_r4, ctx.r4); REX_LOOP_IN(ll_r11, ctx.r11);        en entrant par le haut : ll_r4 = ctx.r4
    loc_82XXXXXX:
        ... ll_r4 et ll_r11 au lieu de ctx.r4 et ctx.r11 ...
        if (!cr6.eq) goto loc_82XXXXXX;
        REX_LOOP_OUT(ll_r4, ctx.r4);                                      en sortant par le bas : ctx.r4 = ll_r4

Une région va d'un début de boucle à son dernier saut en arrière, prise si elle n'appelle rien (régions qui se
chevauchent fusionnées). « N'appelle rien » : aucune fonction recompilée, interception, import, appel indirect ou
piège, rien à qui ctx est passé, et aucun nom inconnu de cet outil. Les feuilles sont laissées à
registres_locaux_feuilles.py ; les fonctions avec setjmp / longjmp sont laissées telles quelles.

Chaque entrée dans une région charge ses variables et chaque sortie les écrit :
  - en tombant dans sa première ligne et hors de sa dernière : les lignes ci-dessus ;
  - un saut qui franchit sa frontière passe par une passerelle qui fait de même puis saute plus loin (étiquettes
    loc_F0000001... propres à la fonction) ; un `return` à l'intérieur devient un saut vers une passerelle qui écrit
    et revient. Une région dans laquelle on saute depuis plus bas est laissée (ses passerelles d'entrée sauteraient en
    arrière).
Un registre seulement lu dans la région n'est pas réécrit ; un registre qu'elle ne mentionne pas reste dans ctx. Les
variables sont déclarées une fois en tête de fonction (REX_LOOP_LOCAL), un saut ne pouvant franchir une initialisation.

Exact par construction : dans la région rien d'autre ne peut regarder ctx, et à ses frontières ctx contient ce qu'il
contenait. REX_LOOP_LOCALS_OFF (à la compilation) fait de chaque variable une référence à son registre : l'ancien code.

  python3 switch/tools/registres_locaux_boucles.py [--verifier] [--stats]
Idempotent. À lancer après les étapes qui lisent ctx.<registre> et avant appels_directs.py.
"""
import argparse
import os
import re
import sys

FUNC = re.compile(r'^DEFINE_REX_FUNC\((\w+)\) \{$')
LABEL = re.compile(r'^(loc_[0-9A-F]{8}):\s*$')
GOTO = re.compile(r'\bgoto (loc_[0-9A-F]{8});')
ONLY_GOTO = re.compile(r'^\tgoto loc_[0-9A-F]{8};$')
RETURN = re.compile(r'\breturn;')
REG = re.compile(r'\bctx\.(r\d+|f\d+|v\d+|cr\d|ctr|xer|reserved)\b')
BARRIER = '\tREX_LOOP_BARRIER();'
# A call of the generated code's convention, or anything else that is handed the context.
CALL = re.compile(r'\(ctx, base[,)]|REX_CALL_INDIRECT_|ppc_trap\(|\bctx\s*[,)]|ppc_setjmp|ppc_longjmp')
LEAF_CALL = re.compile(r'\(ctx, base[,)]|REX_CALL_INDIRECT_|ppc_trap\(|\bctx\s*[,)]')
# Every name the generator writes in front of a parenthesis in a statement that is not a call of guest code.
NAME = re.compile(r'([A-Za-z_][A-Za-z0-9_:]*(?:<[A-Za-z0-9_ ]+>)?)\s*\(')
KNOWN = re.compile(r'^(?:REX_\w+|simde_\w+|__builtin_\w+|rex::ppc::\w+|std::fma|std::isnan|std::atomic_thread_fence|'
                   r'std::bit_cast<\w+>|double|float|sqrt|log2f|memset|if|for|switch|sizeof|'
                   r'u?int(?:8|16|32|64)_t|u(?:8|16|32|64)|int|short|char|static_cast<[A-Za-z0-9_ ]+>|'
                   r'compare(?:<\w+>)?|setFromMask|(?:en|dis)ableFlushMode(?:Unconditional)?|'
                   r'flushModeIs(?:On|Off))$')
DONE = re.compile(r'\bll_(?:r\d|f\d|v\d|cr\d|ctr\b|xer\b|reserved\b)')
MARKER = '// torchlight : registres en variables locales le temps d\'une boucle sans appel (switch/tools/registres_locaux_boucles.py)'
MACROS = MARKER + '''
#ifndef REX_LOOP_LOCAL
#if defined(REX_LOOP_LOCALS_OFF)
#define REX_LOOP_LOCAL(type, name, reg) type& name = reg
#define REX_LOOP_IN(name, reg) ((void)0)
#define REX_LOOP_OUT(name, reg) ((void)0)
#else
#define REX_LOOP_LOCAL(type, name, reg) type name{}
#define REX_LOOP_IN(name, reg) name = reg
#define REX_LOOP_OUT(name, reg) reg = name
#endif
#endif
'''


def type_of(reg):
    if reg.startswith('cr'):
        return 'PPCCRRegister'
    if reg == 'xer':
        return 'PPCXERRegister'
    if reg.startswith('v'):
        return 'PPCVRegister'
    return 'PPCRegister'


def order(reg):
    return (re.sub(r'\d', '', reg), int(re.sub(r'\D', '', reg) or 0))


def is_code(line):
    return not line.lstrip().startswith('//')


class Counts:
    def __init__(self):
        self.done = self.already = self.with_call = self.unknown_name = self.no_regs = self.from_below = 0
        self.functions = self.registers = self.read_only = self.stubs = 0
        self.unknown = {}


def convert(body, c):
    """One function body (its lines); returns the new lines, or None when nothing is taken."""
    label_at = {}
    for k, line in enumerate(body):
        m = LABEL.match(line)
        if m:
            label_at[m.group(1)] = k
    jumps = []  # (line, target line)
    last_back = {}
    for k, line in enumerate(body):
        if not is_code(line):
            continue
        for target in GOTO.findall(line):
            if target in label_at:
                jumps.append((k, label_at[target]))
                if label_at[target] <= k:
                    last_back[target] = k
    # Call-free stretches of headers that have a barrier, merged where they overlap.
    spans = []
    for header, end in last_back.items():
        first = label_at[header]
        code = [l for l in body[first:end + 1] if is_code(l)]
        if any(DONE.search(l) for l in code):
            c.already += 1
            continue
        if any(CALL.search(l) for l in code):
            c.with_call += 1
            continue
        names = {n for l in code for n in NAME.findall(l) if not KNOWN.match(n)}
        if names:
            c.unknown_name += 1
            for n in names:
                c.unknown[n] = c.unknown.get(n, 0) + 1
            continue
        spans.append((first, end))
    spans.sort()
    merged = []
    for first, end in spans:
        if merged and first <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], end)
        else:
            merged.append([first, end])
    taken = []  # (first, last, registers, stored registers)
    for first, last in merged:
        # Jumped into from below: its entry stubs would jump backwards.
        if any(first <= t <= last and s > last for s, t in jumps):
            c.from_below += 1
            continue
        text = '\n'.join(l for l in body[first:last + 1] if is_code(l))
        regs = sorted({m.group(1) for m in REG.finditer(text)}, key=order)
        if not regs:
            c.no_regs += 1
            continue
        stored = []
        for r in regs:
            if r[0] in 'rf' and r[1:].isdigit():
                mentions = re.findall(r'\bctx\.' + r + r'\b(\.\w+)?(\s*(?:[-+*/|&^%]|<<|>>)?=(?!=))?', text)
                if all(field and not assign for field, assign in mentions) and \
                        not re.search(r'&\s*ctx\.' + r + r'\b', text):
                    c.read_only += 1
                    continue
            stored.append(r)
        taken.append((first, last, regs, stored))
    if not taken:
        return None
    region_of = {}
    for n, (first, last, _, _) in enumerate(taken):
        for k in range(first, last + 1):
            region_of[k] = n

    def sync_in(n):
        return ['\tREX_LOOP_IN(ll_%s, ctx.%s);' % (r, r) for r in taken[n][2]] if n is not None else []

    def sync_out(n):
        return ['\tREX_LOOP_OUT(ll_%s, ctx.%s);' % (r, r) for r in taken[n][3]] if n is not None else []

    counter = [0]

    def new_label():
        counter[0] += 1
        return 'loc_F%07X' % counter[0]

    entry_stubs = {n: [] for n in range(len(taken))}  # region -> [(label, lines)]
    exit_stubs = {n: [] for n in range(len(taken))}
    made = {}
    lines = list(body)
    for k, l in enumerate(body):
        if not is_code(l):
            continue
        here = region_of.get(k)
        if here is not None:
            l = REG.sub(lambda m: 'll_' + m.group(1), l)

        def jump(m, here=here):
            there = region_of.get(label_at.get(m.group(1), -1))
            if there == here:
                return m.group(0)
            sync = sync_out(here) + sync_in(there)
            if not sync:
                return m.group(0)
            key = (here, there, m.group(1))
            if key not in made:
                made[key] = new_label()
                (exit_stubs[here] if here is not None else entry_stubs[there]).append(
                    (made[key], sync + ['\tgoto %s;' % m.group(1)]))
            return 'goto %s;' % made[key]

        l = GOTO.sub(jump, l)
        if here is not None and taken[here][3] and RETURN.search(l):
            key = (here, 'return')
            if key not in made:
                made[key] = new_label()
                exit_stubs[here].append((made[key], sync_out(here) + ['\treturn;']))
            l = RETURN.sub('goto %s;' % made[key], l)
        lines[k] = l
    c.stubs += len(made)
    new = []
    for k, l in enumerate(lines):
        here = region_of.get(k)
        if here is not None and k == taken[here][0]:
            if entry_stubs[here]:
                skip = new_label()
                new.append('\tgoto %s;' % skip)
                for label, text in entry_stubs[here]:
                    new.append(label + ':')
                    new.extend(text)
                new.append(skip + ':')
            new.extend(sync_in(here))  # falling into the region
        new.append(l)
        if here is not None and k == taken[here][1]:
            falls = not ONLY_GOTO.match(body[k])
            if falls:
                new.extend(sync_out(here))  # falling out of it
            if exit_stubs[here]:
                over = new_label() if falls else None
                if falls:
                    new.append('\tgoto %s;' % over)
                for label, text in exit_stubs[here]:
                    new.append(label + ':')
                    new.extend(text)
                if falls:
                    new.append(over + ':')
        if l.strip() == 'REX_FUNC_PROLOGUE();':
            for r in sorted({r for t in taken for r in t[2]}, key=order):
                new.append('\tREX_LOOP_LOCAL(%s, ll_%s, ctx.%s);' % (type_of(r), r, r))
    c.done += len(taken)
    c.functions += 1
    c.registers += sum(len(t[2]) for t in taken)
    return new


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gen', default=os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__)))), 'switch', 'generated', 'default'))
    ap.add_argument('--verifier', dest='check', action='store_true', help='compte sans écrire')
    ap.add_argument('--stats', action='store_true', help='also the names that kept a loop out')
    a = ap.parse_args()
    files = sorted(f for f in os.listdir(a.gen) if re.match(r'\w+_recomp\.\d+\.cpp$', f))
    pchs = [f for f in os.listdir(a.gen) if f.endswith('_pch.h')]
    if not files or len(pchs) != 1:
        sys.exit('ERROR (post-codegen patch): loop_locals: no *_recomp.N.cpp or no single *_pch.h in %s' % a.gen)
    c = Counts()
    changed = 0
    for f in files:
        path = os.path.join(a.gen, f)
        lines = open(path, encoding='utf-8').read().split('\n')
        out = []
        touched = False
        i = 0
        while i < len(lines):
            out.append(lines[i])
            if not FUNC.match(lines[i]):
                i += 1
                continue
            j = i + 1
            while j < len(lines) and lines[j] != '}':
                j += 1
            body = lines[i + 1:j]
            i = j
            code_text = '\n'.join(l for l in body if is_code(l))
            new = None
            if LEAF_CALL.search(code_text) and 'setjmp' not in code_text and 'longjmp' not in code_text and \
                    any(l.strip() == 'REX_FUNC_PROLOGUE();' for l in body):
                new = convert(body, c)
            if new is None:
                out.extend(body)
            else:
                out.extend(new)
                touched = True
        if touched:
            changed += 1
            if not a.check:
                open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
    pch = os.path.join(a.gen, pchs[0])
    text = open(pch, encoding='utf-8').read()
    if MARKER not in text:
        anchor = '#define REX_MEMORY_SIZE'
        if anchor not in text:
            sys.exit('ERREUR : registres_locaux_boucles : pas de REX_MEMORY_SIZE dans %s' % pch)
        if not a.check:
            open(pch, 'w', encoding='utf-8', newline='\n').write(text.replace(anchor, MACROS + anchor, 1))
    print('registres_locaux_boucles%s: %d call-free loop regions in %d functions keep %d registers in locals (%d only read: not '
          'written back; %d border stubs), %d loops already done; left alone: %d loops with a call, %d with a name '
          'this script does not know, %d regions that use no ctx register, %d entered from below; %d files changed' %
          (' (check only)' if a.check else '', c.done, c.functions, c.registers, c.read_only, c.stubs, c.already,
           c.with_call, c.unknown_name, c.no_regs, c.from_below, changed))
    if a.stats and c.unknown:
        print('  names that kept a loop out: ' +
              ', '.join('%s (%d)' % kv for kv in sorted(c.unknown.items(), key=lambda kv: -kv[1])[:20]))


if __name__ == '__main__':
    main()

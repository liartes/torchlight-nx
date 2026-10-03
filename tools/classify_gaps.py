# Trie les trous (gaps.json) : garde les morceaux qui ressemblent à de vraies fonctions.
# Prérequis dans <dossier> : gaps.json, known.json (find_gaps.py) et code.bin (dump_code.sh).
# Usage : python3 tools/classify_gaps.py <dossier>  -> écrit <dossier>/classified2.json
# Désassemblage : llvm-mc-20 -triple=powerpc-unknown-unknown -ppc-asm-full-reg-names
import json, struct, subprocess, sys, re
S = sys.argv[1]
d = open(S + '/code.bin', 'rb').read()
B = 0x82000000
def word(a): return struct.unpack('>I', d[a-B:a-B+4])[0]
CODE_LO, CODE_HI = 0x82190000, 0x83027448

def disasm(words):
    out = []
    for w in words:  # mot par mot pour garder l'alignement (les invalides -> None)
        out.append(w)
    hexs = '\n'.join(' '.join('0x%02x' % b for b in w.to_bytes(4, 'big')) for w in words)
    r = subprocess.run(['llvm-mc-20', '--disassemble', '-triple=powerpc-unknown-unknown',
                        '-ppc-asm-full-reg-names'], input=hexs, capture_output=True, text=True)
    lines = [l.strip() for l in r.stdout.splitlines() if l.strip() and not l.strip().startswith('.text')]
    if len(lines) == len(words): return lines
    res = []
    for w in words:
        r = subprocess.run(['llvm-mc-20', '--disassemble', '-triple=powerpc-unknown-unknown',
                            '-ppc-asm-full-reg-names'], input=' '.join('0x%02x' % b for b in w.to_bytes(4, 'big')),
                           capture_output=True, text=True)
        l = [x.strip() for x in r.stdout.splitlines() if x.strip() and not x.strip().startswith('.text')]
        res.append(l[0] if l else None)
    return res

NONVOL = re.compile(r'^(r(0|11|12|1[4-9]|2\d|3[01])|f(0|1[4-9]|2\d|3[01]))$')
COND = re.compile(r'^(bf|bt|bc|bdnz|bdz|beq|bne|blt|bgt|ble|bge|bns|bso|bclr|bcctr|bdnzf|bdnzt|bdzf|bdzt|bgelr|beqlr|bnelr|bltlr|bgtlr|blelr|bnslr|bsolr)')
def parse(ins):
    m = ins.split(None, 1); mn = m[0]; ops = [o.strip() for o in m[1].split(',')] if len(m) > 1 else []
    return mn, ops
def regs_in(op):
    return re.findall(r'\b([rf]\d+|cr\d)\b', op)
def target(addr, op):
    mm = re.match(r'\.([+-]\d+)$', op)
    if mm: return addr + int(mm.group(1))
    mm = re.match(r'(0x[0-9a-f]+|\d+)$', op)
    return int(mm.group(1), 0) if mm else None

def check_piece(start, instrs):
    """instrs: liste (addr, texte). Renvoie None si ok, sinon raison."""
    written = {'r1','r2','r3','r4','r5','r6','r7','r8','r9','r10','r13'} | {'f%d' % i for i in range(1, 14)}
    cr_set = False; ctr_set = False; frame = False
    end = instrs[-1][0]
    for a, ins in instrs:
        mn, ops = parse(ins)
        base = mn.rstrip('.')
        if COND.match(mn) and not mn.startswith('bdnz') and not mn.startswith('bdz') and not cr_set:
            return 'condition testée sans comparaison (%s)' % ins
        if mn.startswith('bdnz') and not ctr_set: return 'bdnz sans mtctr'
        if mn.startswith('bctr') and not ctr_set: return 'bctr sans mtctr'
        if mn.startswith('b'):
            t = target(a, ops[-1]) if ops else None
            if t is not None:
                if not (CODE_LO <= t < CODE_HI): return 'branche hors code'
                if COND.match(mn) and not (start <= t <= end): return 'branche conditionnelle hors du morceau'
                if mn in ('b', 'bl') and not (start <= t <= end) and t not in KNOWN and not (0x82860000 <= t < 0x82861000):
                    return 'saute au milieu d\'une fonction (%s)' % ins
            if mn in ('bl', 'bctrl', 'bla'):
                written |= {'r3','r4','r5','r6','r7','r8','r9','r10','r11','r12','r0'}
            continue
        # accès mémoire relatifs à r1
        for o in ops:
            mm = re.match(r'(-?\d+)\(r1\)$', o)
            if mm and int(mm.group(1)) >= 0 and not frame and not base.startswith('stwu'):
                return 'pile lue/écrite sans frame (%s)' % ins
        if base in ('addi', 'addis') and ops and ops[0] == 'r1' and not frame:
            return 'libère une frame jamais créée (%s)' % ins
        if base.startswith('l') and ops and re.match(r'-\d+\(r1\)$', ops[-1]) and not frame:
            return 'recharge des registres sauvegardés : épilogue (%s)' % ins
        is_store = base.startswith('st')
        if base.startswith('cmp') or base.startswith('fcmp'):
            reads = [r for o in ops for r in regs_in(o) if not r.startswith('cr')]; writes = []; cr_set = True
        elif base in ('mtctr',): reads = regs_in(ops[0]); writes = []; ctr_set = True
        elif base.startswith('mt'): reads = [r for o in ops for r in regs_in(o)]; writes = []; cr_set = cr_set or 'crf' in base
        elif is_store:
            reads = [r for o in ops for r in regs_in(o)]; writes = []
            if base == 'stwu' and ops[-1].endswith('(r1)'): frame = True
            # sauvegarde de prologue : st* rNV, -x(r1)
            if re.match(r'-\d+\(r1\)$', ops[-1]): reads = []
        elif mn in ('nop',): continue
        else:
            writes = regs_in(ops[0]) if ops else []
            reads = [r for o in ops[1:] for r in regs_in(o)]
        if mn.endswith('.') : cr_set = True
        for r in reads:
            if NONVOL.match(r) and r not in written: return 'lit %s jamais initialisé (%s)' % (r, ins)
        written |= set(writes)
    return None

def is_term(ins):
    mn, ops = parse(ins)
    return mn in ('blr', 'bctr', 'b', 'ba')

gaps = json.load(open(S + '/gaps.json'))
KNOWN = set(g[2] for g in gaps) | set(json.load(open(S + '/known.json')))
good, bad = [], []
for prev, gstart, nxt in gaps:
    if nxt - gstart <= 12: continue
    addrs = list(range(gstart, nxt, 4)); words = [word(a) for a in addrs]
    if sum(CODE_LO <= w < CODE_HI for w in words) >= 2: bad.append((gstart, nxt, 'table d\'adresses')); continue
    text = disasm(words)
    i = 0
    while i < len(addrs):
        while i < len(addrs) and words[i] == 0: i += 1
        if i >= len(addrs): break
        start = addrs[i]; piece = []; maxt = 0; ok_end = False
        while i < len(addrs):
            if text[i] is None: piece = None; break
            piece.append((addrs[i], text[i]))
            mn, ops = parse(text[i])
            if COND.match(mn) and ops:
                t = target(addrs[i], ops[-1]); maxt = max(maxt, t or 0)
            i += 1
            if is_term(text[i-1]) and maxt <= addrs[i-1]: ok_end = True; break
        if piece is None:
            bad.append((start, nxt, 'instruction invalide (données ?)'))
            while i < len(addrs) and (text[i] is None or words[i] != 0): i += 1
            continue
        if not ok_end: bad.append((start, nxt, 'ne finit pas proprement')); continue
        why = 'routine __savegpr/__restgpr' if 0x82860000 <= start < 0x82861000 else check_piece(start, piece)
        # fin = début du morceau suivant (ou fin du trou)
        j = i
        while j < len(addrs) and words[j] == 0: j += 1
        pend = addrs[j] if j < len(addrs) else nxt
        if why: bad.append((start, pend, why))
        else: good.append((start, pend, len(piece), piece[0][1]))
json.dump({'good': good, 'rejected': bad}, open(S + '/classified2.json', 'w'))
print(len(good), 'fonctions retenues,', len(bad), 'morceaux rejetés')
from collections import Counter
print(Counter(re.sub(r' \(.*', '', b[2]).split(' r')[0].split(' f')[0] for b in bad).most_common())

#!/usr/bin/env python3
"""Registres lus avant d'être écrits dans le code généré (portage de tools/lee_antes.py de nfsmw-nx).

Avec cr_as_local, xer_as_local, ctr_as_local et non_volatile_as_local (switch/gen_manifest.sh), ces registres sont des
variables locales de chaque fonction : une fonction qui en lit un avant de l'écrire attend une valeur de sa voisine
(morceau d'une fonction découpée, sauté avec b, bctr ou une table de sauts) et la recevrait à zéro. Ces fonctions
vont dans switch/registres_partages.txt (share_registers), puis on régénère. Quelques fausses alertes connues chez
nfsmw-nx (boucles). À lancer après chaque génération du code.

Usage : registres_lus_avant.py [dossier generated] > liste
"""
import glob, re, sys
gen = sys.argv[1] if len(sys.argv) > 1 else 'switch/generated/default'
fn_re=re.compile(r'^DEFINE_REX_FUNC\((sub_[0-9A-F]+)\)')
tok=re.compile(r'(?<![.\w])(r(?:0|2|11|12|1[4-9]|2[0-9]|3[01])|f(?:0|1[4-9]|2[0-9]|3[01])|v(?:1[4-9]|2[0-9]|3[0-9]|[4-5][0-9]|6[0-3]|6[4-9]|[7-9][0-9]|1[01][0-9]|12[0-7])|cr[0-7]|ctr|xer)\b')
asg=re.compile(r'^([A-Za-z0-9_]+)(?:\.[A-Za-z0-9_\[\]]+)*\s*=[^=]')
res={}
for f in sorted(glob.glob(gen+'/torchlight_recomp.*.cpp')):
    cur=None
    for ln in open(f,encoding='utf-8',errors='ignore'):
        m=fn_re.match(ln)
        if m: cur=m.group(1);written=set();bad={};continue
        if cur is None: continue
        if ln.startswith('}'):
            if bad: res[cur]=bad
            cur=None;continue
        s=ln.strip()
        if not s or s.startswith('//') or s.startswith('PPC') or s.startswith('uint') or s.startswith('REX_FUNC_PROLOGUE'): continue
        if s.startswith('REX_STORE') and re.search(r',\s*(r|f)\d+\.(u64|f64)\);$',s) and ('ctx.r1.' in s): continue  # guardado de prologo
        s2=re.sub(r'compare<[^>]*>\([^;]*,\s*xer\)','',s)
        s2=re.sub(r'cr\d\.so = xer\.so;','',s2)
        mc=re.match(r'^(cr[0-7])\.compare',s)
        if mc:
            for r in tok.findall(re.sub(r',\s*xer\)',')',s[len(mc.group(1)):])):
                if r not in written and r not in bad: bad[r]=s[:100]
            written.add(mc.group(1)); continue
        ms=re.match(r'^simde_mm_store\w*\(\(?(?:simde__m128i\*\))?(v\d+)\.\w+,(.*)$',s2)
        if ms:
            rhs=ms.group(2)
            for r in tok.findall(rhs):
                if r not in written and r not in bad: bad[r]=s[:100]
            written.add(ms.group(1)); continue
        m2=asg.match(s2); lhs=None; rhs=s2
        if m2: lhs=m2.group(1); rhs=s2[m2.end()-1:]
        for r in tok.findall(rhs):
            if r not in written and r not in bad: bad[r]=s[:100]
        if lhs and tok.fullmatch(lhs): written.add(lhs)
        # calls through a function pointer to simd that write by reference: ignore
# Fausses alertes des boucles (une lecture placée avant l'écriture dans le texte, après à l'exécution) : une fonction
# dont le prologue sauvegarde des registres (appel d'une aide __savegprlr_N, « bl 0x8286... », ou std rN,-x(r1)) les
# possède. Ne garder que les fonctions sans une telle sauvegarde (vrais morceaux, ou fonctions du genre setjmp). Ne
# jamais marquer une fonction qui sauvegarde ses registres : son appelante reprendrait des valeurs non restaurées.
prologue = {}
for f in sorted(glob.glob(gen+'/torchlight_recomp.*.cpp')):
    t = open(f, encoding='utf-8', errors='ignore').read()
    for m in re.finditer(r'DEFINE_REX_FUNC\((sub_[0-9A-F]+)\) \{(.*?)\n\}', t, re.S):
        if m.group(1) in res:
            prologue[m.group(1)] = re.findall(r'\t// (.*)', m.group(2))[:8]
def sauve(n):
    return any(x.startswith('bl 0x8286') or re.match(r'std r(1[4-9]|2\d|3[01]),-\d+\(r1\)', x) for x in prologue.get(n, []))
ecartees = [n for n in res if sauve(n)]
for n in ecartees: del res[n]
print('fausses alertes de boucles écartées (prologue qui sauvegarde) :', len(ecartees), file=sys.stderr)
for n in sorted(res): print(n, ' '.join(sorted(res[n])), '|', list(res[n].values())[0])
print('TOTAL',len(res),file=sys.stderr)

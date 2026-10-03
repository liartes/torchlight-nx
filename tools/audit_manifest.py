# Vérifie les entrées de [entrypoint.functions] : une entrée recouverte par une autre
# fonction générée est un milieu de fonction (à retirer).
# Usage : python3 tools/audit_manifest.py
import re, glob, bisect
funcs = {}
for f in glob.glob('generated/default/torchlight_recomp.*.cpp'):
    cur = None; n = 0
    for line in open(f, errors='ignore'):
        m = re.match(r'DEFINE_REX_FUNC\((?:__imp__)?sub_([0-9A-F]{8})\)', line)
        if m or line.startswith('DEFINE_REX_FUNC'):
            if cur: funcs[cur] = n
            cur = int(m.group(1), 16) if m else None; n = 0; continue
        if cur is not None and re.match(r'\s*// (?!ERROR)', line): n += 1
    if cur: funcs[cur] = n
addrs = sorted(funcs)
entries = [int(x, 16) for x in re.findall(r'^0x([0-9A-Fa-f]+)', open('torchlight_manifest.toml').read(), re.M)]
for e in entries:
    k = bisect.bisect_left(addrs, e)
    cont = [p for p in addrs[max(0, k-400):k] if p + 4*funcs[p] > e]
    print(('FAUX %08X  à l\'intérieur de sub_%08X' % (e, cont[-1])) if cont else ('OK   %08X' % e))
n_err = sum(open(f, errors='ignore').read().count('ERROR: ') for f in glob.glob('generated/default/torchlight_recomp.*.cpp'))
print('branches non résolues dans le code généré :', n_err)

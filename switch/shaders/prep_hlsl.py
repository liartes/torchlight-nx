#!/usr/bin/env python3
"""Prépare les shaders HLSL (Direct3D 9) du jeu pour glslang (HLSL -> SPIR-V).

Usage : prep_hlsl.py <dossier hlsl capturés> <assets/rtshaderlib> <sortie>
  sortie/lib/   copie de rtshaderlib corrigée
  sortie/src/   shaders capturés corrigés

Corrections :
- fins de ligne LF et retour à la ligne final (sinon « preprocessor directive cannot be
  preceded by another token » : le #include suivant se colle à la dernière ligne) ;
- glslang ne distingue pas sampler1D/2D/3D/CUBE de Direct3D 9 pour les surcharges :
  FFP_SampleTexture(samplerXX ...) devient FFP_SampleTextureXX, et les appels sont réécrits
  d'après la déclaration du sampler passé en premier argument ;
- emplacements Vulkan tirés de la sémantique (glslang numérote sinon dans l'ordre de déclaration, et les
  sorties du shader de sommets ne correspondent plus aux entrées du shader de pixels) :
  [[vk::location(N)]] ajouté aux paramètres de main() selon LOCATIONS_VS_IN / LOCATIONS_VARYING.
  La même table sert à l'outil de rendu pour placer les attributs des sommets d'Ogre.
"""
import os, re, sys

src_dir, lib_dir, out = sys.argv[1:4]
os.makedirs(out + '/lib', exist_ok=True)
os.makedirs(out + '/src', exist_ok=True)

# Entrées du shader de sommets (attributs) : sémantique HLSL -> emplacement.
LOCATIONS_VS_IN = {'POSITION': 0, 'BLENDWEIGHT': 1, 'BLENDINDICES': 2, 'NORMAL': 3, 'COLOR': 4,
                   'TEXCOORD': 6, 'BINORMAL': 14, 'TANGENT': 15}   # COLORn -> 4+n, TEXCOORDn -> 6+n
# Sorties du shader de sommets / entrées du shader de pixels.
LOCATIONS_VARYING = {'TEXCOORD': 0, 'COLOR': 8, 'FOG': 10}          # TEXCOORDn -> n, COLORn -> 8+n

def location(table, semantic):
    semantic = semantic.upper().split('_')[0]  # COLOR_center : variante centroïde
    m = re.match(r'([A-Z]+?)(\d*)$', semantic)
    if not m or m.group(1) not in table:
        return None
    return table[m.group(1)] + (int(m.group(2)) if m.group(2) else 0)

def add_locations(t, is_vs, io):
    m = re.search(r'void\s+main\s*\((.*?)\)\s*\{', t, re.S)
    if not m:
        return t
    def param(pm):
        direction, rest, semantic = pm.group(1), pm.group(2), pm.group(3)
        sem = semantic.upper()
        if direction == 'in':
            loc = location(LOCATIONS_VS_IN, sem) if is_vs else (None if sem.startswith('POSITION') or sem.startswith('VPOS') else location(LOCATIONS_VARYING, sem))
        else:
            loc = None if sem.startswith('POSITION') or (not is_vs and sem.startswith('COLOR')) or sem.startswith('DEPTH') \
                else location(LOCATIONS_VARYING, sem)
        prefix = f'[[vk::location({loc})]] ' if loc is not None else ''
        if loc is not None:
            io.append(f'{direction} {loc} {sem}')
        # Sémantiques système de Direct3D 9 -> équivalents que glslang reconnaît (sinon POSITION en sortie
        # devient une variable ordinaire à l'emplacement 0 et gl_Position n'est jamais écrit).
        base_sem = sem.split('_')[0]
        if (direction == 'out' and is_vs and base_sem.startswith('POSITION')) or \
           (direction == 'in' and not is_vs and base_sem.startswith('POSITION')):
            semantic = 'SV_Position'
        elif direction == 'out' and not is_vs and base_sem.startswith('COLOR'):
            semantic = 'SV_Target' + (re.sub(r'\D', '', base_sem) or '0')
        elif direction == 'out' and not is_vs and base_sem.startswith('DEPTH'):
            semantic = 'SV_Depth'
        return f'{prefix}{direction} {rest} : {semantic}'
    params = re.sub(r'\b(in|out)\s+([^:,()]+?)\s*:\s*(\w+)', param, m.group(1))
    return t[:m.start(1)] + params + t[m.end(1):]

# Test alpha d'Ogre (_setAlphaRejectSettings), absent de Vulkan : ajouté en fin de main() des shaders de
# pixels. torchlight_alpha_test = (seuil / 255, CompareFunction d'Ogre, 0, 0), rempli par le rendu ;
# fonction 1 (CMPF_ALWAYS_PASS) par défaut.
ALPHA_TEST = '''
\t{
\t\tconst float tl_a = %s.a;
\t\tconst float tl_r = torchlight_alpha_test.x;
\t\tconst int tl_f = (int)torchlight_alpha_test.y;
\t\tbool tl_pass = true;
\t\tif (tl_f == 0) tl_pass = false;
\t\telse if (tl_f == 2) tl_pass = tl_a < tl_r;
\t\telse if (tl_f == 3) tl_pass = tl_a <= tl_r;
\t\telse if (tl_f == 4) tl_pass = tl_a == tl_r;
\t\telse if (tl_f == 5) tl_pass = tl_a != tl_r;
\t\telse if (tl_f == 6) tl_pass = tl_a >= tl_r;
\t\telse if (tl_f == 7) tl_pass = tl_a > tl_r;
\t\tif (!tl_pass) discard;
\t}
'''

def add_alpha_test(t):
    m = re.search(r'void\s+main\s*\((.*?)\)\s*\{', t, re.S)
    if not m:
        return t
    out = re.search(r'\bout\s+float4\s+(\w+)\s*:\s*(COLOR0?|SV_Target0?)\b', m.group(1))
    if not out:
        return t
    # accolade fermante de main()
    depth, i = 1, m.end()
    while i < len(t) and depth:
        depth += {'{': 1, '}': -1}.get(t[i], 0)
        i += 1
    end = i - 1
    decl = 'float4 torchlight_alpha_test;\n'
    return t[:m.start()] + decl + t[m.start():end] + ALPHA_TEST % out.group(1) + t[end:]

# Le RT Shader System d'Ogre écrit parfois dans une constante globale, en dernier argument (sortie) d'une fonction
# FFP_* : FFP_Modulate(iColor_0.xyz, light_diffuse1.xyz, light_diffuse1.xyz). Légal en HLSL D3D9 (copie locale
# implicite), interdit en Vulkan (« cannot store to Uniform Blocks »). Ces constantes reçoivent une copie locale
# au début de main(), utilisée à leur place dans tout le corps.
def shadow_written_globals(t):
    m = re.search(r'void\s+main\s*\((.*?)\)\s*\{', t, re.S)
    if not m:
        return t
    head, body = t[:m.end()], t[m.end():]
    globals_ = dict((n, ty) for ty, n in re.findall(r'^\s*((?:float|half|int)\w*)\s+(\w+)\s*;', head, re.M))
    written = [n for n in globals_ if re.search(r',\s*' + n + r'(\.\w+)?\s*\)\s*;', body)]
    if not written:
        return t
    for n in written:
        body = re.sub(r'\b' + n + r'\b', 'tl_local_' + n, body)
    decls = ''.join(f'\n\t{globals_[n]} tl_local_{n} = {n};' for n in written)
    return head + decls + body

SUFFIX = {'sampler1D': '1D', 'sampler2D': '2D', 'sampler3D': '3D', 'samplerCUBE': 'CUBE'}

def norm(t):
    t = t.replace('\r\n', '\n').replace('\r', '\n')
    return t if t.endswith('\n') else t + '\n'

for name in os.listdir(lib_dir):
    if not name.endswith('.hlsl'):
        continue
    t = norm(open(os.path.join(lib_dir, name), errors='replace').read())
    t = re.sub(r'void\s+FFP_SampleTexture\s*\(\s*in\s+(sampler1D|sampler2D|sampler3D|samplerCUBE)\b',
               lambda m: f'void FFP_SampleTexture{SUFFIX[m.group(1)]}(in {m.group(1)}', t)
    open(os.path.join(out, 'lib', name), 'w').write(t)

# Options de compilation D3DX par empreinte de source (hlsl/options.txt du traceur).
OPTIONS = {}
if os.path.exists(os.path.join(src_dir, 'options.txt')):
    for line in open(os.path.join(src_dir, 'options.txt')):
        parts = line.split()
        if len(parts) == 2:
            OPTIONS[parts[0]] = int(parts[1])

for name in os.listdir(src_dir):
    if not name.endswith('.hlsl'):
        continue
    t = norm(open(os.path.join(src_dir, name), errors='replace').read())
    types = dict((n, ty) for ty, n in re.findall(r'\b(sampler1D|sampler2D|sampler3D|samplerCUBE)\s+(\w+)', t))
    def call(m):
        ty = types.get(m.group(1))
        return f'FFP_SampleTexture{SUFFIX[ty]}({m.group(1)}' if ty else m.group(0)
    t = re.sub(r'\bFFP_SampleTexture\s*\(\s*(\w+)', call, t)
    io = []
    if '_vs_' not in name:
        t = add_alpha_test(t)   # avant add_locations : la sémantique de sortie est encore COLOR
    t = add_locations(t, '_vs_' in name, io)
    t = shadow_written_globals(t)
    # Type des échantillonneurs (Vulkan exige une vue cube pour un samplerCUBE, même sans texture).
    for ty, reg in re.findall(r'\b(sampler1D|sampler2D|sampler3D|samplerCUBE)\s+\w+\s*:\s*register\s*\(\s*s(\d+)\s*\)', t):
        io.append(f'sampler {reg} {SUFFIX[ty]}')
    open(os.path.join(out, 'src', name[:-5] + '.io'), 'w').write('\n'.join(io) + '\n')
    # Rangement des matrices : même option que le compilateur D3DX du jeu, qui varie selon le programme
    # (options.txt, relevé par le traceur : 8 = par lignes, 16 = par colonnes). Sans l'index : les programmes
    # animés (world_matrix_array_3x4) sont par lignes, les autres par colonnes (vérifié sur les deux familles).
    flags = OPTIONS.get(name.split('_')[0])
    row_major = (flags & 8) != 0 if flags is not None else 'world_matrix_array_3x4' in t
    if row_major:
        t = '#pragma pack_matrix(row_major)\n' + t
    open(os.path.join(out, 'src', name), 'w').write(t)

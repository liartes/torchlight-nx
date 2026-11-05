#!/usr/bin/env python3
"""Arbre de la build PC de suivi mémoire (diagnostic, jamais versionné).

preparer.py <dépôt torchlight> <dossier de sortie> [poses|billboards]
  <sortie>/src -> lien vers <dépôt>/src ; <sortie>/switch : liens vers switch/* sauf generated (copie modifiée) et
  CMakeLists.txt (copie + suivi_particules.cpp).
Code généré : accès mémoire instrumentés (pch), REX_RAW_ADDR classés lecture/écriture d'après l'instruction.
Par défaut : mises à jour des systèmes de ParticleUniverse encadrées dans sub_821C8A58 (suivi_particules.cpp).
poses : Entity::cacheBoneMatrices et les étapes de l'image encadrées, thread principal entier suivi (suivi_poses.cpp).
billboards : aucune garde (src/torchlight_particules.cpp appelle les symboles faibles de suivi_billboards.cpp).
"""
import os, re, shutil, sys

DEPOT, SORTIE = map(os.path.abspath, sys.argv[1:3])
MODE = sys.argv[3] if len(sys.argv) > 3 else 'particules'
SOURCE = {'particules': 'suivi_particules.cpp', 'poses': 'suivi_poses.cpp', 'billboards': 'suivi_billboards.cpp'}[MODE]
# Mode poses : gardes en tête de fonction (tous les chemins d'entrée, directs, indirects, branches de fin).
ETAPES = {'sub_821E5910': 1, 'sub_821E6BD8': 2, 'sub_8219D288': 3, 'sub_821B07F0': 4, 'sub_821C8C00': 5,
          'sub_821A6070': 6, 'sub_821F26B0': 7}
GARDES_DECL = (
    'extern "C" int SuiviEtapeEntree(PPCContext& ctx, uint8_t* base, int etape) noexcept;\n'
    'extern "C" void SuiviEtapeSortie(int avant) noexcept;\n'
    'extern "C" void SuiviPoseEntree(PPCContext& ctx, uint8_t* base) noexcept;\n'
    'extern "C" void SuiviPoseSortie() noexcept;\n'
    'extern "C" void SuiviBbEntree() noexcept;\n'
    'extern "C" void SuiviBbSortie() noexcept;\n'
    'extern "C" unsigned SuiviUniteEntree(unsigned unite) noexcept;\n'
    'extern "C" void SuiviUniteSortie(unsigned avant) noexcept;\n'
    'extern "C" void SuiviRendue(unsigned entite) noexcept;\n'
    'namespace { struct SuiviEtapeGarde { int a; SuiviEtapeGarde(PPCContext& c, uint8_t* b, int e) : '
    'a(SuiviEtapeEntree(c, b, e)) {} ~SuiviEtapeGarde() { SuiviEtapeSortie(a); } };\n'
    'struct SuiviPoseGarde { SuiviPoseGarde(PPCContext& c, uint8_t* b) { SuiviPoseEntree(c, b); } '
    '~SuiviPoseGarde() { SuiviPoseSortie(); } };\n'
    'struct SuiviBbGarde { SuiviBbGarde() { SuiviBbEntree(); } ~SuiviBbGarde() { SuiviBbSortie(); } };\n'
    'struct SuiviUniteGarde { unsigned a; explicit SuiviUniteGarde(unsigned u) : a(SuiviUniteEntree(u)) {} '
    '~SuiviUniteGarde() { SuiviUniteSortie(a); } }; }\n')
ICI = os.path.dirname(os.path.abspath(__file__))
SW = os.path.join(SORTIE, 'switch')
os.makedirs(SW, exist_ok=True)
for nom in os.listdir(DEPOT):
    if nom in ('switch',):
        continue
    lien = os.path.join(SORTIE, nom)
    if not os.path.lexists(lien):
        os.symlink(os.path.join(DEPOT, nom), lien)
for nom in os.listdir(os.path.join(DEPOT, 'switch')):
    if nom in ('generated', 'CMakeLists.txt') or nom.startswith('out'):
        continue
    lien = os.path.join(SW, nom)
    if not os.path.lexists(lien):
        os.symlink(os.path.join(DEPOT, 'switch', nom), lien)

cm = open(os.path.join(DEPOT, 'switch', 'CMakeLists.txt')).read()
cm = cm.replace('../src/torchlight_appels_indirects.cpp',
                '../src/torchlight_appels_indirects.cpp ' + os.path.join(ICI, SOURCE), 1)
assert SOURCE in cm
open(os.path.join(SW, 'CMakeLists.txt'), 'w').write(cm)

SRC_GEN = os.path.join(DEPOT, 'switch', 'generated')
DST_GEN = os.path.join(SW, 'generated')
# Copie de travail refaite à côté, puis seuls les fichiers changés sont recopiés (ninja ne recompile qu'eux).
TMP_GEN = DST_GEN + '.tmp'
if os.path.exists(TMP_GEN):
    shutil.rmtree(TMP_GEN)
shutil.copytree(SRC_GEN, TMP_GEN)
G = os.path.join(TMP_GEN, 'default')

# pch : accès suivis
pch_path = os.path.join(G, 'torchlight_pch.h')
pch = open(pch_path).read()
ancre = '#define REX_STORE_U64(x, y) (*(volatile u64*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap64(y))\n'
assert ancre in pch
bloc = r'''
// --- suivi mémoire des particules (diagnostic PC) ---
extern "C" thread_local unsigned t_suivi_actif;
extern "C" void SuiviAcces(unsigned adresse, unsigned taille, int ecriture, const char* fonction) noexcept;
extern "C" void SuiviEcriture(unsigned adresse, unsigned taille, unsigned long long valeur, const char* fonction) noexcept;
extern "C" void SuiviAlloc(unsigned adresse, unsigned taille) noexcept;
#define SUIVI_A(x, n, w) (t_suivi_actif ? SuiviAcces((u32)(x), (n), (w), __func__) : (void)0)
#undef REX_LOAD_U8
#undef REX_LOAD_U16
#undef REX_LOAD_U32
#undef REX_LOAD_U64
#undef REX_STORE_U8
#undef REX_STORE_U16
#undef REX_STORE_U32
#undef REX_STORE_U64
#define REX_LOAD_U8(x) (SUIVI_A((x), 1, 0), (*(volatile u8*)REX_RAW_ADDR(x)))
#define REX_LOAD_U16(x) (SUIVI_A((x), 2, 0), __builtin_bswap16(*(volatile u16*)REX_RAW_ADDR(x)))
#define REX_LOAD_U32(x) (SUIVI_A((x), 4, 0), __builtin_bswap32(*(volatile u32*)REX_RAW_ADDR(x)))
#define REX_LOAD_U64(x) (SUIVI_A((x), 8, 0), __builtin_bswap64(*(volatile u64*)REX_RAW_ADDR(x)))
#define SUIVI_S(x, n, y) (t_suivi_actif ? SuiviEcriture((u32)(x), (n), (unsigned long long)(y), __func__) : (void)0)
#define REX_STORE_U8(x, y) (SUIVI_S((x), 1, (y)), (void)(*(volatile u8*)REX_RAW_ADDR(x) = (y)))
#define REX_STORE_U16(x, y) (SUIVI_S((x), 2, (y)), (void)(*(volatile u16*)REX_RAW_ADDR(x) = __builtin_bswap16(y)))
#define REX_STORE_U32(x, y) (SUIVI_S((x), 4, (y)), (void)(*(volatile u32*)REX_RAW_ADDR(x) = __builtin_bswap32(y)))
#define REX_STORE_U64(x, y) (SUIVI_S((x), 8, (y)), (void)(*(volatile u64*)REX_RAW_ADDR(x) = __builtin_bswap64(y)))
#define REX_RAW_ADDR_L16(x) (SUIVI_A((x), 16, 0), REX_RAW_ADDR(x))
#define REX_RAW_ADDR_E16(x) (SUIVI_A((x), 16, 1), REX_RAW_ADDR(x))
#define REX_RAW_ADDR_E8(x) (SUIVI_A((x), 8, 1), REX_RAW_ADDR(x))
#define REX_RAW_ADDR_E4(x) (SUIVI_A((x), 4, 1), REX_RAW_ADDR(x))
#define REX_RAW_ADDR_E128(x) (SUIVI_A((x), 128, 1), REX_RAW_ADDR(x))
// --- fin du suivi ---
'''
pch = pch.replace(ancre, ancre + bloc, 1)
open(pch_path, 'w').write(pch)

CLASSE = [(re.compile(r'(lvx|lvlx|lvrx|lvehx|lvewx|lvsl|lvsr)'), 'L16'),
          (re.compile(r'(stvx|stvlx|stvrx|stvehx|stvewx)'), 'E16'),
          (re.compile(r'lwarx'), 'E4'), (re.compile(r'ldarx'), 'E8'),
          (re.compile(r'dcbz'), 'E128')]
re_asm = re.compile(r'^\s*// ([a-z0-9.]+)')
stats = {}
encadres = 0
for nom in sorted(os.listdir(G)):
    if not (nom.startswith('torchlight_recomp.') and nom.endswith('.cpp')):
        continue
    p = os.path.join(G, nom)
    lignes = open(p).read().split('\n')
    dernier = None
    modif = False
    dans_8a58 = False
    garde = None
    gardes_fichier = False
    for i, l in enumerate(lignes):
        if l.startswith('DEFINE_REX_FUNC('):
            dans_8a58 = MODE == 'particules' and l.startswith('DEFINE_REX_FUNC(sub_821C8A58)')
            if MODE == 'poses':
                f = l[len('DEFINE_REX_FUNC('):].split(')')[0]
                if f == 'sub_821B93B0':
                    garde = 'SuiviPoseGarde suivi_garde(ctx, base);'
                elif f == 'sub_821BB578':
                    garde = 'SuiviBbGarde suivi_garde;'
                elif f == 'sub_821D5410':
                    garde = 'SuiviUniteGarde suivi_garde(ctx.r3.u32);'
                elif f == 'sub_821E0D18':
                    garde = 'SuiviRendue(ctx.r3.u32);'
                elif f in ETAPES:
                    garde = 'SuiviEtapeGarde suivi_garde(ctx, base, %d);' % ETAPES[f]
        if garde and l.strip() == 'REX_FUNC_PROLOGUE();':
            lignes[i] = l + ' ' + garde
            garde = None
            gardes_fichier = True
            encadres += 1
            modif = True
        m = re_asm.match(l)
        if m:
            dernier = m.group(1)
            continue
        if 'REX_RAW_ADDR(' in l:
            sorte = None
            for r, s in CLASSE:
                if dernier and r.match(dernier):
                    sorte = s
                    break
            if sorte:
                lignes[i] = l.replace('REX_RAW_ADDR(', 'REX_RAW_ADDR_%s(' % sorte)
                modif = True
            stats[(dernier, sorte)] = stats.get((dernier, sorte), 0) + 1
        if '__imp__sub_821CD7F8(ctx, base);' in l:
            lignes[i] = l.replace('__imp__sub_821CD7F8(ctx, base);',
                                  '{ const unsigned suivi_t = ctx.r4.u32; __imp__sub_821CD7F8(ctx, base); SuiviAlloc(ctx.r3.u32, suivi_t); }')
            modif = True
            stats[('alloc', None)] = stats.get(('alloc', None), 0) + 1
        if dans_8a58:
            if l.strip() == '__imp__sub_821C8618(ctx, base);':
                lignes[i] = l.replace('__imp__sub_821C8618(ctx, base);',
                                      '{ SuiviDebut(ctx, base, ctx.r3.u32); __imp__sub_821C8618(ctx, base); SuiviFin(); }')
                encadres += 1
                modif = True
            elif 'REX_CALL_INDIRECT_FUNC_AT(0x821C8A58, 0,' in l:
                lignes[i] = '\t{ SuiviDebut(ctx, base, ctx.r3.u32); ' + l.strip() + ' SuiviFin(); }'
                encadres += 1
                modif = True
    if modif:
        if gardes_fichier:
            k = next(j for j, x in enumerate(lignes) if x.startswith('DEFINE_REX_FUNC('))
            lignes.insert(k, GARDES_DECL)
        if any('SuiviDebut' in x for x in lignes):
            k = next(j for j, x in enumerate(lignes) if x.startswith('DEFINE_REX_FUNC('))
            lignes.insert(k, 'extern "C" void SuiviDebut(PPCContext& ctx, uint8_t* base, unsigned sys) noexcept;\n'
                             'extern "C" void SuiviFin() noexcept;')
        open(p, 'w').write('\n'.join(lignes))
print('encadrés :', encadres)
for (d, s), n in sorted(stats.items(), key=lambda x: -x[1]):
    print('  %-10s %-5s %d' % (d, s, n))

changes = 0
for racine, _, fichiers in os.walk(TMP_GEN):
    for nom in fichiers:
        src = os.path.join(racine, nom)
        dst = os.path.join(DST_GEN, os.path.relpath(src, TMP_GEN))
        if os.path.exists(dst) and open(src, 'rb').read() == open(dst, 'rb').read():
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)
        os.utime(dst)
        changes += 1
shutil.rmtree(TMP_GEN)
print('fichiers du code généré changés :', changes)

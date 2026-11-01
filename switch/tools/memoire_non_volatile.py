#!/usr/bin/env python3
"""Mémoire du jeu sans volatile dans le code généré, avec des barrières du compilateur aux têtes de boucle.

D'après renut-nx / banjo-nx (GPL-3.0, https://github.com/ChanseyIsTheBest/renut-nx : tools/pch_no_volatile.py et
tools/loop_barriers.py, eux-mêmes repris de sa2-nx, futari-nx et marathon-nx ; masseffect-nx y a mesuré +15-20 %
d'images/s sur la console).

Le générateur écrit chaque lecture et écriture de la mémoire du jeu par un pointeur volatile (REX_LOAD_Uxx /
REX_STORE_Uxx) : le compilateur ne peut ni garder une valeur en registre, ni regrouper ou réordonner deux accès, ni
garder en registres de l'hôte les registres restés dans ctx (aliasing). pile_non_volatile.py ne l'a fait que pour la
pile. Cet outil fait de même pour toute la mémoire du jeu (les accès MMIO, REX_MM_*, restent volatile) :

- en-tête précompilé : les huit macros REX_LOAD_U*/REX_STORE_U* passent par des types rex_mem_uN : may_alias et
  aligned(1) avec REX_GUEST_MEMORY_NON_VOLATILE (option CMake TORCHLIGHT_MEMOIRE_NON_VOLATILE), sinon volatile
  comme avant (et barrières vides : code d'origine, pour les A/B sans régénérer) ;
- REX_LOOP_BARRIER() après l'étiquette de chaque tête de boucle (étiquette visée par un goto placé plus bas) qui en a
  besoin. Sans volatile, GCC -O3 sort la lecture d'une boucle d'attente (while (mot != 0) {} : un autre thread ou le
  GPU change le mot) de la boucle et en fait un saut sur lui-même (futari-nx, sur la console ; clang sur PC ne le
  faisait pas). La barrière ne touche que la fenêtre de 4 Go du jeu (forme de marathon-nx) : ctx et la pile de
  l'hôte gardent leurs registres, les variables de registres_locaux_boucles.py aussi. Pas de barrière quand la boucle
  ne lit pas la mémoire du jeu et n'appelle rien, ou quand chaque chemin vers la tête passe par un appel indirect
  (opaque pour le compilateur, même avec le LTO) ;
- REX_SYNC_BARRIER() après chaque sync / lwsync / eieio du jeu (le générateur n'en écrit rien) : le compilateur ne
  réordonne pas les accès de part et d'autre, comme le faisait volatile. Rien de plus pour le processeur (comme avant).

Essayé deux fois le 2026-11-01 et abandonné (journal) : pas de gain sur la console (~2 % plus lent), et sur PC une
différence de validation venue d'un changement de mode des denormals (setcsr sans clobber mémoire : sans volatile, une
lecture et son calcul flottant peuvent remonter avant le changement de mode). Avant un nouvel essai : clobber mémoire
dans setcsr (SDK, rex/platform/fpscr.h).
Contrôle sur l'ELF de la Switch : switch/tools/boucles_infinies.py (aucun saut sur lui-même dans une fonction sub_*).
À refaire après chaque génération du code (build_switch.sh le fait, en dernier) ; idempotent : les barrières sont
recalculées à chaque passage, les fichiers inchangés ne sont pas réécrits.
Usage : memoire_non_volatile.py [--toutes] [--annuler]   (--toutes : une barrière à chaque tête de boucle)
"""
import os
import re
import sys

RACINE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GEN = os.path.join(RACINE, 'switch', 'generated', 'default')

MARQUE = '// --- torchlight : mémoire du jeu sans volatile (switch/tools/memoire_non_volatile.py) ---'
FIN = '// --- fin de la mémoire du jeu sans volatile ---\n'
DEFINITIONS = MARQUE + r'''
#if defined(REX_GUEST_MEMORY_NON_VOLATILE)
typedef u8 __attribute__((may_alias, aligned(1))) rex_mem_u8;
typedef u16 __attribute__((may_alias, aligned(1))) rex_mem_u16;
typedef u32 __attribute__((may_alias, aligned(1))) rex_mem_u32;
typedef u64 __attribute__((may_alias, aligned(1))) rex_mem_u64;
// Tout accès à la fenêtre du jeu est refait après la barrière ; ctx et la pile de l'hôte ne sont pas touchés.
#define REX_LOOP_BARRIER() __asm__ __volatile__("" : "+m"(*(uint8_t(*)[0x100000000ull])base))
#define REX_SYNC_BARRIER() __asm__ __volatile__("" : "+m"(*(uint8_t(*)[0x100000000ull])base))
#else
typedef volatile u8 rex_mem_u8;
typedef volatile u16 rex_mem_u16;
typedef volatile u32 rex_mem_u32;
typedef volatile u64 rex_mem_u64;
#define REX_LOOP_BARRIER() ((void)0)
#define REX_SYNC_BARRIER() ((void)0)
#endif
''' + FIN
PREFIXES = ('#define REX_LOAD_U8(', '#define REX_LOAD_U16(', '#define REX_LOAD_U32(', '#define REX_LOAD_U64(',
            '#define REX_STORE_U8(', '#define REX_STORE_U16(', '#define REX_STORE_U32(', '#define REX_STORE_U64(')

LABEL = re.compile(r'^(loc_[0-9A-F]{8}):\s*$')
GOTO = re.compile(r'goto (loc_[0-9A-F]{8});')
ONLY_GOTO = re.compile(r'^\tgoto (loc_[0-9A-F]{8});$')
IF_GOTO = re.compile(r'^\tif \(.*\) goto (loc_[0-9A-F]{8});$')
RETURN = re.compile(r'^\t(?:\{ (?:REX_LEAF_STORE\(\w+, \w+\); )+)?return;(?: \})?$')
BARRIER = '\tREX_LOOP_BARRIER();'
SYNC_BARRIER = '\tREX_SYNC_BARRIER();'
SYNC = re.compile(r'^\t// (?:sync|lwsync|eieio)\b')
# Lecture de la mémoire du jeu, ou ce qui peut en cacher une : accès, adresse brute, appel de toute sorte.
GUEST = re.compile(r'REX_LOAD|REX_RO_|REX_RAW_ADDR|REX_MM_|REX_CALL_INDIRECT_|REX_PROMOTED|REX_\w*GLOBAL_LOCK|'
                   r'__sync_|\bbase\b|ppc_trap\(')
INDIRECT = re.compile(r'REX_CALL_INDIRECT_')
OTHER_CALL = re.compile(r'\b\w+\(ctx, base[,)]')
FUNC = re.compile(r'^DEFINE_REX_FUNC\(\w+\) \{$')


def code(line):
    return not line.lstrip().startswith('//')


def needed_headers(body, headers):
    """Têtes de boucle d'une fonction (étiquette -> indice de ligne) qui ont besoin d'une barrière."""
    label_at = {}
    for i, line in enumerate(body):
        m = LABEL.match(line)
        if m:
            label_at[m.group(1)] = i
    last_back = {}
    for i, line in enumerate(body):
        for target in GOTO.findall(line):
            if target in headers and label_at[target] <= i:
                last_back[target] = i
    # Successeurs de chaque ligne ; une ligne illisible (goto d'une autre forme : switch...) rend toutes les têtes
    # nécessaires. Un « if (...) return; » compte comme une ligne ordinaire (plus de chemins : plus prudent).
    n = len(body)
    succ = [None] * n
    readable = True
    for i, line in enumerate(body):
        m = ONLY_GOTO.match(line)
        mi = IF_GOTO.match(line) if not m else None
        if 'goto' in line and not m and not mi and code(line):
            readable = False
            break
        if m:
            succ[i] = [label_at[m.group(1)]] if m.group(1) in label_at else None
        elif mi:
            succ[i] = [i + 1, label_at[mi.group(1)]] if mi.group(1) in label_at else None
        elif RETURN.match(line):
            succ[i] = []
        else:
            succ[i] = [i + 1]
        if succ[i] is None:
            readable = False
            break
    need = set()
    for header, at in headers.items():
        end = last_back.get(header, at)
        lines = [l for l in body[at + 1:end + 1] if code(l) and l not in (BARRIER, SYNC_BARRIER)]
        if not any(GUEST.search(l) or OTHER_CALL.search(l) for l in lines):
            continue  # rien à sortir de la boucle
        if not readable:
            need.add(header)
            continue
        # Un chemin de la tête jusqu'à elle-même sans appel indirect ?
        seen = set()
        stack = [at]
        found = False
        while stack and not found:
            i = stack.pop()
            if i in seen or i >= n:
                continue
            seen.add(i)
            line = body[i]
            if INDIRECT.search(line) and not OTHER_CALL.search(line) and code(line):
                continue  # ce chemin passe par l'appel opaque
            for j in succ[i]:
                if j == at:
                    found = True
                    break
                stack.append(j)
        if found:
            need.add(header)
    return need


def traiter_fichier(lines, toutes, annuler, compte):
    plain = [l for l in lines if l not in (BARRIER, SYNC_BARRIER)]
    if annuler:
        return plain
    out = []
    i = 0
    while i < len(plain):
        if not FUNC.match(plain[i]):
            out.append(plain[i])
            i += 1
            continue
        j = i + 1
        while j < len(plain) and plain[j] != '}':
            j += 1
        body = plain[i + 1:j]
        headers, seen = {}, {}
        for k, line in enumerate(body):
            m = LABEL.match(line)
            if m:
                seen[m.group(1)] = k
                continue
            for target in GOTO.findall(line):
                if target in seen:
                    headers[target] = seen[target]
        need = set(headers) if toutes else needed_headers(body, headers)
        compte['tetes'] += len(headers)
        compte['barrieres'] += len(need)
        out.append(plain[i])
        for line in body:
            out.append(line)
            m = LABEL.match(line)
            if m and m.group(1) in need:
                out.append(BARRIER)
            elif SYNC.match(line):
                out.append(SYNC_BARRIER)
                compte['sync'] += 1
        i = j
    return out


def main():
    annuler = '--annuler' in sys.argv
    toutes = '--toutes' in sys.argv
    pch = os.path.join(GEN, 'torchlight_pch.h')
    texte = avant = open(pch, encoding='utf-8').read()
    lignes = texte.split('\n')
    for k, l in enumerate(lignes):
        if l.startswith(PREFIXES):
            lignes[k] = (re.sub(r'\(rex_mem_(u\d+)\*\)', r'(volatile \1*)', l) if annuler
                         else re.sub(r'\(volatile (u\d+)\*\)', r'(rex_mem_\1*)', l))
    texte = '\n'.join(lignes)
    if MARQUE in texte:
        debut = texte.index(MARQUE)
        texte = texte[:debut] + texte[texte.index(FIN) + len(FIN):]
    if not annuler:
        if '#define REX_LOAD_U8(x)' not in texte:
            sys.exit('ERREUR : memoire_non_volatile : pas de REX_LOAD_U8 dans %s (en-tête généré changé ?)' % pch)
        texte = texte.replace('#define REX_LOAD_U8(x)', DEFINITIONS + '#define REX_LOAD_U8(x)', 1)
        if texte.count('(rex_mem_u') != 8:
            sys.exit('ERREUR : memoire_non_volatile : %d macros converties au lieu de 8' % texte.count('(rex_mem_u'))
    if texte != avant:  # sinon l'en-tête précompilé change de date et tout le code généré se recompile
        open(pch, 'w', encoding='utf-8', newline='\n').write(texte)
    compte = {'tetes': 0, 'barrieres': 0, 'sync': 0}
    fichiers = 0
    for f in sorted(os.listdir(GEN)):
        if not (f.startswith('torchlight_recomp.') and f.endswith('.cpp')):
            continue
        chemin = os.path.join(GEN, f)
        lines = open(chemin, encoding='utf-8').read().split('\n')
        out = traiter_fichier(lines, toutes, annuler, compte)
        if out != lines:
            open(chemin, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
            fichiers += 1
    if annuler:
        print('mémoire du jeu : volatile rétabli, barrières retirées (%d fichiers)' % fichiers)
    else:
        print('mémoire du jeu sans volatile : %d têtes de boucle, %d barrières (%d sans rien à protéger), '
              '%d barrières de sync, %d fichiers modifiés' % (compte['tetes'], compte['barrieres'],
                                                              compte['tetes'] - compte['barrieres'], compte['sync'],
                                                              fichiers))


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Copie de l'image du jeu chargée (default.xex décompressé, 0x82000000 .. + REX_IMAGE_SIZE) pour
switch/tools/constantes_lecture_seule.py : lance la version PC (switch/out-pc, copie des données utilisateur de
natif_pc.sh), attend le chargement, relit /proc/<pid>/mem à l'adresse hôte 0x100000000 + 0x82000000, puis arrête le
jeu. Le processus du jeu doit descendre de ce script (ptrace_scope = 1).

  python3 switch/tools/image_jeu.py [--sortie switch/out-pc/image_jeu.bin] [--attente 20]
Donnée dérivée du jeu : jamais versionnée (switch/out-pc est ignoré).
"""
import argparse
import os
import re
import signal
import subprocess
import sys
import time

RACINE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PCH = os.path.join(RACINE, 'switch', 'generated', 'default', 'torchlight_pch.h')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--sortie', default=os.path.join(RACINE, 'switch', 'out-pc', 'image_jeu.bin'))
    ap.add_argument('--attente', type=float, default=20.0)
    a = ap.parse_args()
    pch = open(PCH, encoding='utf-8').read()
    base = int(re.search(r'#define REX_IMAGE_BASE (0x[0-9A-Fa-f]+)', pch).group(1).rstrip('ul'), 16)
    taille = int(re.search(r'#define REX_IMAGE_SIZE (0x[0-9A-Fa-f]+)', pch).group(1).rstrip('ul'), 16)
    jeu = subprocess.Popen([os.path.join(RACINE, 'switch', 'natif_pc.sh'), '--torchlight_natif_seul=true'],
                           cwd=RACINE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    try:
        time.sleep(a.attente)
        pids = subprocess.run(['pgrep', '-x', 'torchlight'], capture_output=True, text=True).stdout.split()
        if not pids:
            sys.exit('image_jeu : le jeu ne tourne pas')
        donnees = bytearray()
        with open('/proc/%s/mem' % pids[-1], 'rb') as f:
            f.seek(0x100000000 + base)
            while len(donnees) < taille:
                donnees += f.read(min(0x100000, taille - len(donnees)))
        if donnees[:2] != b'MZ':
            sys.exit('image_jeu : pas d\'en-tête PE à %08X' % base)
        open(a.sortie, 'wb').write(donnees)
        print('image_jeu : %d octets (%08X) -> %s' % (len(donnees), base, a.sortie))
    finally:
        os.killpg(jeu.pid, signal.SIGTERM)
        time.sleep(2)
        for pid in subprocess.run(['pgrep', '-x', 'torchlight'], capture_output=True, text=True).stdout.split():
            os.kill(int(pid), signal.SIGKILL)


if __name__ == '__main__':
    main()

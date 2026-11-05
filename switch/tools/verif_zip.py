#!/usr/bin/env python3
"""Vérifie les lignes « [zip] taille crc32 » d'un journal (--torchlight_zip_verif, src/torchlight_inflate.cpp) contre le dossier
de toutes les archives du jeu (pak.zip, et translations/<langue>/pak_<langue>.zip : une console en français lit aussi la sienne)
: chaque fichier lu en entier par le tampon du lecteur zip doit avoir la taille et le CRC32 d'une entrée.

  python3 switch/tools/verif_zip.py <dossier du jeu> <journal>...
"""
import os
import re
import sys
import zipfile


def main():
    connus = set()
    archives = 0
    for dossier, _, fichiers in os.walk(sys.argv[1]):
        for nom in fichiers:
            if not nom.lower().endswith('.zip'):
                continue
            archives += 1
            with zipfile.ZipFile(os.path.join(dossier, nom)) as z:
                for i in z.infolist():
                    connus.add((i.file_size, i.CRC))
    lignes = bons = 0
    mauvais = []
    for chemin in sys.argv[2:]:
        for l in open(chemin, errors='replace'):
            m = re.search(r'\[zip\] (\d+) ([0-9a-f]{8})', l)
            if m:
                lignes += 1
                if (int(m.group(1)), int(m.group(2), 16)) in connus:
                    bons += 1
                else:
                    mauvais.append(m.group(0))
    print('%d archives, %d entrées ; %d fichiers lus : %d justes, %d faux' % (archives, len(connus), lignes, bons, len(mauvais)))
    for m in mauvais[:10]:
        print('  ', m)
    sys.exit(1 if mauvais else 0)


if __name__ == '__main__':
    main()

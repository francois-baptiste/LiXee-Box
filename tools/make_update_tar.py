#!/usr/bin/env python3
"""Paquet de mise a jour du fork : build/update.tar

Meme format que l'update.tar de la LiXee-Box (voir docs/maintenance_api.md) :
  firmware.bin          -> partition OTA
  web/<chemin>          -> /littlefs/web/<chemin> (pages web compressees)
Archive ustar, entrees fichiers uniquement (pas de repertoires).

A lancer apres `idf.py build` (qui regenere data/web depuis web_src). Le
fichier est l'asset "update.tar" d'une release GitHub dont le tag vaut
v<PROJECT_VER> : la page Mise a jour de la box le telecharge depuis
releases/latest/download/update.tar du depot LIXEE_UPDATE_REPO.
"""
import os
import sys
import tarfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FW = os.path.join(ROOT, 'build', 'lixee_matter_bridge.bin')
WEB = os.path.join(ROOT, 'data', 'web')
OUT = os.path.join(ROOT, 'build', 'update.tar')


def main():
    if not os.path.isfile(FW):
        print('make_update_tar: %s absent, lancer idf.py build' % FW, file=sys.stderr)
        return 1
    count = 0
    with tarfile.open(OUT, 'w', format=tarfile.USTAR_FORMAT) as tar:
        tar.add(FW, arcname='firmware.bin', recursive=False)
        for base, dirs, files in os.walk(WEB):
            dirs.sort()
            for name in sorted(files):
                path = os.path.join(base, name)
                rel = os.path.relpath(path, WEB).replace(os.sep, '/')
                # Le firmware n'accepte qu'un niveau de sous-repertoire sous web/
                if rel.count('/') > 1:
                    print('make_update_tar: ignore (trop profond) %s' % rel, file=sys.stderr)
                    continue
                tar.add(path, arcname='web/' + rel, recursive=False)
                count += 1
    print('make_update_tar: %s (firmware + %d fichiers web, %d octets)'
          % (OUT, count, os.path.getsize(OUT)))
    return 0


if __name__ == '__main__':
    sys.exit(main())

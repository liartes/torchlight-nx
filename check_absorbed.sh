#!/bin/sh
cd "$HOME/workspace/torchlight"
python3 - << 'PY'
import re
reg = set(int(m, 16) for m in re.findall(r'SetFunction\(0x([0-9A-Fa-f]+),', open('generated/default/torchlight_register.cpp').read()))
man = re.findall(r'^0x([0-9A-Fa-f]+)\s*=', open('torchlight_manifest.toml').read(), re.M)
missing = [m for m in man if int(m, 16) not in reg]
print(len(man), "déclarées,", len(missing), "absentes de la génération")
for m in missing: print("0x" + m.upper())
PY

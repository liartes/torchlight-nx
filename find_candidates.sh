#!/bin/sh
cd "$HOME/workspace/torchlight"
LD_LIBRARY_PATH="$HOME/rexglue-sdk-0.10.0-linux-amd64/linux-amd64/lib" \
  gdb -batch -ex "handle SIGSEGV nostop noprint pass" -ex run -ex "dump binary memory /tmp/image.bin 0x182000000 0x1835C0000" \
  --args ./out/build/linux-amd64-debug/torchlight --game_data_root="$PWD/assets" --gpu_plugin=xenos > /tmp/gdb.log 2>&1
[ -s /tmp/image.bin ] || { echo "Dump échoué, voir /tmp/gdb.log"; exit 1; }
python3 - << 'PY'
import re, struct, glob, sys
BASE = 0x82000000
CODE_LO, CODE_HI = 0x82190000, 0x83027448
gen = 'generated/default/'
reg = set(int(m, 16) for m in re.findall(r'SetFunction\(0x([0-9A-Fa-f]+),',
      open(gen + 'torchlight_register.cpp').read()))
locs = set()
for f in glob.glob(gen + 'torchlight_recomp.*.cpp'):
    locs.update(int(m, 16) for m in re.findall(r'loc_([0-9A-Fa-f]{8})', open(f, errors='ignore').read()))
data = open('/tmp/image.bin', 'rb').read()
out = set()
for i in range(CODE_HI - BASE, len(data) - 3, 4):
    v = struct.unpack('>I', data[i:i+4])[0]
    if CODE_LO <= v < CODE_HI and v % 4 == 0 and v not in reg and v not in locs:
        out.add(v)
for v in sorted(out):
    print('0x%08X = {}' % v)
print(len(out), 'candidats', file=sys.stderr)
PY

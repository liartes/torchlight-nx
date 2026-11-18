#!/usr/bin/env python3
"""Test des macros de switch/tools/lanes_registre.py : chaque indice de 100 000 vecteurs au hasard, contre la forme
indexée d'origine (V.u32[i], V.u16[i]). Compile et lance sur l'hôte (clang++-20) ; avec --aarch64, compile aussi la
forme NEON avec le compilateur de la Switch (docker devkitpro/devkita64) et affiche son assembleur.
  python3 switch/tools/test_lanes_registre.py [--aarch64]
"""
import os, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lanes_registre

TEST = r'''
#include <cstdint>
#include <cstdio>
#include <random>
union V { uint8_t u8[16]; uint16_t u16[8]; uint32_t u32[4]; uint64_t u64[2]; };
%s
uint32_t lane32(const V& v, uint32_t ea) { return REX_LANE_U32(v, 3 - ((ea & 0xF) >> 2)); }
uint16_t lane16(const V& v, uint32_t ea) { return REX_LANE_U16(v, 7 - ((ea & 0xF) >> 1)); }
int main() {
  std::mt19937_64 g(12345);
  long erreurs = 0, essais = 0;
  for (int k = 0; k < 100000; k++) {
    V v; v.u64[0] = g(); v.u64[1] = g();
    for (uint32_t ea = 0; ea < 64; ea++) {
      essais += 2;
      if (lane32(v, ea) != v.u32[3 - ((ea & 0xF) >> 2)]) erreurs++;
      if (lane16(v, ea) != v.u16[7 - ((ea & 0xF) >> 1)]) erreurs++;
    }
  }
  std::printf("lanes_registre : %%ld lectures comparées, %%ld différences\n", essais, erreurs);
  return erreurs != 0;
}
'''
d = tempfile.mkdtemp()
src = os.path.join(d, 't.cpp')
open(src, 'w').write(TEST % lanes_registre.MACROS)
subprocess.check_call(['clang++-20', '-O2', '-std=c++20', src, '-o', os.path.join(d, 't')])
r = subprocess.call([os.path.join(d, 't')])
if '--aarch64' in sys.argv:
    b = '/opt/devkitpro/devkitA64/bin/aarch64-none-elf-'
    cmd = ("%sg++ -std=gnu++20 -O2 -c t.cpp -o t.o && %sobjdump -d --no-show-raw-insn t.o | sed -n '/<_Z6lane32/,/ret/p'"
           % (b, b))
    subprocess.check_call(['sg', 'docker', '-c', 'docker run --rm -v %s:/w -w /w devkitpro/devkita64 sh -c "%s"'
                           % (d, cmd)])
sys.exit(r)

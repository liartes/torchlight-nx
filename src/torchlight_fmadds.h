// fmadds / fmsubs / fnmadds / fnmsubs du code recompilé : arrondies une seule fois en simple précision, comme le Xenon
// (switch/tools/fmadds_simples.py, d'après le port 96 de renut_nx_ultimate_base). Copie de RexFmaddsSimple, que la
// passe écrit dans torchlight_pch.h : les deux doivent rester identiques, pour que les natives restent au bit près du
// code généré.
//
// Quand a, b et c sont des float exacts (valeurs venues de lfs ou d'opérations simples), une seule fma simple
// précision : le résultat exact arrondi une fois, celui du Xenon. Sinon (une valeur double dans le registre), la forme
// d'avant : fma en double puis arrondi en simple. -DREX_FMADDS_DOUBLE_ARRONDI : la forme double partout.
#pragma once

#include <bit>
#include <cmath>
#include <cstdint>

namespace torchlight {

[[gnu::always_inline]] inline bool EstFloatExact(double x) noexcept {
  return std::bit_cast<uint64_t>(double(float(x))) == std::bit_cast<uint64_t>(x);
}

[[gnu::always_inline]] inline double TorchlightFmadds(double a, double b, double c) noexcept {
#if !defined(REX_FMADDS_DOUBLE_ARRONDI)
  if (__builtin_expect(EstFloatExact(a) & EstFloatExact(b) & EstFloatExact(c), 1))
    return double(__builtin_fmaf(float(a), float(b), float(c)));
#endif
  return double(float(__builtin_fma(a, b, c)));
}

}  // namespace torchlight

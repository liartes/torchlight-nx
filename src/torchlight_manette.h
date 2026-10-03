// torchlight - manette virtuelle pilotée par un script (essais automatiques, voir torchlight_manette.cpp).

#pragma once

#include <rex/runtime.h>

namespace torchlight::manette {

// Enveloppe config.input_factory pour ajouter la manette du script si torchlight_manette n'est pas vide.
// À appeler dans OnPreSetup.
void WrapInput(rex::RuntimeConfig& config);

}  // namespace torchlight::manette

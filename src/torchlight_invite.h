// torchlight - lecture des objets d'Ogre en mémoire du jeu (« invitée »), partagée par la capture et le
// rendu natif. Disposition des objets : switch/RENDU_NATIF.md (phase 1).

#pragma once

#if defined(__linux__)
#include <sys/uio.h>
#include <unistd.h>
#endif
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace torchlight::invite {

constexpr uint32_t kDevice = 0x4000A480u;  // clé des maps par device d'Ogre

inline uint32_t Load32(const uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return __builtin_bswap32(v);
}

inline float LoadFloat(const uint8_t* base, uint32_t addr) {
  const uint32_t w = Load32(base, addr);
  float f;
  std::memcpy(&f, &w, 4);
  return f;
}

// Pages (4 Ko) déjà vérifiées lisibles, par thread : la vérification coûte un appel système (PC) ou le verrou du
// tas (console). Une table de bits pour les 4 Go invités (128 Ko, allouée au premier usage par les seuls threads qui
// lisent) : l'ancienne table de 4096 entrées indexée par page & 4095 confondait tas (0x4xxxxxxx) et piles
// (0x7xxxxxxx). Oubliées régulièrement (ForgetVerifiedPages), au cas où le jeu libère de la mémoire.
// Type trivial (pointeur brut, jamais libéré : threads durables) dans une seule variable : un thread_local non
// trivial (unique_ptr) appelait sa routine d'initialisation à chaque accès (profil de la console).
struct VerifiedPages {
  uint64_t* bits;
  uint32_t epoch;
};
inline thread_local VerifiedPages t_verified = {nullptr, 0};
inline std::atomic<uint32_t> g_verified_epoch{1};
constexpr size_t kVerifiedWords = (1u << 20) / 64;

inline void ForgetVerifiedPages() { g_verified_epoch.fetch_add(1, std::memory_order_relaxed); }

// Une lecture de mémoire invitée non validée bloque le thread (gestionnaire d'exceptions du runtime) :
// toute lecture au-delà des arguments vérifie d'abord la plage. ReadableSlow : vérification complète, pages
// retenues ensuite dans la table de bits du thread.
[[gnu::noinline]] inline bool ReadableSlow(uint32_t addr, uint32_t len) {
  auto* mem = rex::system::kernel_memory();
  if (!mem || !len || uint64_t(addr) + len > 0x100000000ull) return false;
  const uint32_t epoch = g_verified_epoch.load(std::memory_order_relaxed);
  VerifiedPages& v = t_verified;
  if (!v.bits) v.bits = new uint64_t[kVerifiedWords];
  if (v.epoch != epoch) {
    std::memset(v.bits, 0, kVerifiedWords * sizeof(uint64_t));
    v.epoch = epoch;
  }
  const uint32_t first = addr >> 12, last = (addr + len - 1) >> 12;
  bool known = true;
  for (uint32_t p = first; p <= last && known; p++) known = (v.bits[p >> 6] >> (p & 63)) & 1;
  if (known) return true;
#if defined(__linux__)
  // Sans verrou : process_vm_readv sur notre propre processus échoue (EFAULT) au lieu de provoquer une
  // faute si la mémoire n'est pas lisible. L'ancien test (BaseHeap::QueryRangeAccess) prenait le verrou
  // global du runtime alors que nos propres verrous étaient tenus : blocage du thread de rendu (inversion).
  static thread_local std::vector<uint8_t> scratch;
  if (scratch.size() < len) scratch.resize(len);
  iovec local{scratch.data(), len};
  iovec remote{mem->TranslateVirtual<void*>(addr), len};
  static const pid_t self = getpid();
  const bool ok = process_vm_readv(self, &local, 1, &remote, 1, 0) == ssize_t(len);
#else
  auto* heap = mem->LookupHeap(addr);
  if (!heap || mem->LookupHeap(addr + len - 1) != heap) return false;
  const bool ok = heap->QueryRangeAccess(addr, addr + len - 1) != rex::memory::PageAccess::kNoAccess;
#endif
  if (ok)
    for (uint32_t p = first; p <= last; p++) v.bits[p >> 6] |= uint64_t(1) << (p & 63);
  return ok;
}

// Chemin rapide (~15 appels par dessin dans le relevé) : plage d'une ou deux pages déjà vérifiées, sans appel de
// fonction ; sinon ReadableSlow.
inline bool Readable(uint32_t addr, uint32_t len) {
  const VerifiedPages& v = t_verified;
  if (v.bits && len && uint64_t(addr) + len <= 0x100000000ull &&
      v.epoch == g_verified_epoch.load(std::memory_order_relaxed)) [[likely]] {
    const uint32_t first = addr >> 12, last = (addr + len - 1) >> 12;
    if (last - first <= 1 && ((v.bits[first >> 6] >> (first & 63)) & 1) &&
        ((v.bits[last >> 6] >> (last & 63)) & 1))
      return true;
  }
  return ReadableSlow(addr, len);
}

// Pointeur vers un objet du tas (0x4xxxxxxx) ou de la pile d'un thread invité (0x7xxxxxxx).
inline bool Ptr(uint32_t v, uint32_t len = 0x40) {
  return v >= 0x40000000u && v < 0x80000000u && (v & 3) == 0 && Readable(v, len);
}

// Valeur de la map « par device » d'Ogre (std::map MSVC, un seul élément : la racine).
inline uint32_t DeviceMapValue(const uint8_t* base, uint32_t head) {
  if (!Ptr(head)) return 0;
  const uint32_t root = Load32(base, head + 4);
  if (!Ptr(root) || Load32(base, root + 0xC) != kDevice) return 0;
  return Load32(base, root + 0x10);
}

// Adresse GPU stockée par le Direct3D du jeu -> adresse physique. Les adresses de la vue 0xE0000000
// (pages de 4 Ko) gardent leurs bits de poids fort ; le runtime place cette vue à physique + 0x1000
// (PhysicalHeap::GetPhysicalAddress). Sans cette correction, les données lues sont décalées de 4 Ko.
inline uint32_t ToPhysical(uint32_t addr) {
  return addr >= 0xE0000000u ? (addr - 0xE0000000u + 0x1000u) & 0x1FFFFFFFu : addr & 0x1FFFFFFFu;
}

inline const uint8_t* Physical(uint32_t addr) {
  auto* mem = rex::system::kernel_memory();
  return mem ? mem->TranslatePhysical<const uint8_t*>(ToPhysical(addr)) : nullptr;
}

// std::string MSVC (Xbox) : tampon de 16 octets ou pointeur en +0, taille +0x10, capacité +0x14.
inline std::string LoadStdString(const uint8_t* base, uint32_t s) {
  if (!Readable(s, 0x18)) return {};
  const uint32_t size = Load32(base, s + 0x10);
  const uint32_t cap = Load32(base, s + 0x14);
  if (size > 256) return {};
  const uint32_t data = cap >= 16 ? Load32(base, s) : s;
  if (!Readable(data, size ? size : 1)) return {};
  return std::string(reinterpret_cast<const char*>(base + data), size);
}

// Parcours en ordre d'un std::map MSVC : l'en-tête est le nœud sentinelle (_Left = plus petit,
// _Parent = racine, _Right = plus grand) ; nœud { _Left +0, _Parent +4, _Right +8, valeur +0xC }.
template <typename F>
void WalkMap(const uint8_t* base, uint32_t head, int max, F&& visit) {
  if (!Ptr(head)) return;
  uint32_t n = Load32(base, head);
  for (int k = 0; k < max && Ptr(n) && n != head; k++) {
    visit(n + 0xC);
    const uint32_t right = Load32(base, n + 8);
    if (right != head && Ptr(right)) {
      n = right;
      for (int g = 0; g < 64; g++) {
        const uint32_t left = Load32(base, n);
        if (left == head || !Ptr(left)) break;
        n = left;
      }
    } else {
      uint32_t parent = Load32(base, n + 4);
      for (int g = 0; g < 64 && Ptr(parent) && parent != head && n == Load32(base, parent + 8); g++) {
        n = parent;
        parent = Load32(base, n + 4);
      }
      n = parent;
    }
  }
}

}  // namespace torchlight::invite

// torchlight - mesure du coût des défauts de TLB sur la console (Switch, essai : -DTORCHLIGHT_MESURE_TLB=ON, jamais dans
// une build des joueurs).
//
// Question : une part du temps du thread principal ne suit pas la fréquence du processeur (attente de la mémoire). Une
// page de 4 Ko demande une entrée de TLB (1024 au total sur le Cortex-A57, partagées entre code et données), un bloc de
// 2 Mo une seule pour 512 pages. Le noyau n'emploie un bloc que si l'adresse virtuelle et l'adresse physique sont
// alignées de la même façon sur 2 Mo et si tout le bloc est projeté d'un coup. Pour la mémoire du jeu, le SDK vise cela ;
// le code du .nro, lui, est placé par le chargeur, et une page protégée coupe le bloc qui la contient.
//
// Méthode : à nombre égal de lignes de cache (donc même comportement des caches), deux parcours en série dans un ordre
// aléatoire, chaque lecture dépendant de la précédente :
//   - groupé : les lignes de quelques pages consécutives (le TLB suffit) ;
//   - dispersé : une ligne par page sur toute la zone (pour 4096 lignes, 4096 pages : plus que le TLB si la zone est en
//     pages de 4 Ko, quelques entrées seulement si elle est en blocs).
// L'écart dispersé - groupé est le prix du défaut de TLB, proche de zéro pour une zone en blocs. Le code est aussi
// mesuré en exécution : des appels vers des instructions `ret` existantes de .text, sans autre effet.
// Page et ligne sont tirées au hasard : un motif régulier (première version) tombait dans 64 ensembles du cache L2 sur
// 2048 en mémoire physiquement continue, et mesurait la RAM plutôt que le TLB.
// Témoin : les mêmes pages physiques projetées deux fois par l'appel des vues du jeu (svcMapProcessMemory), à une adresse
// alignée comme la physique (blocs possibles) et à une adresse décalée de 4 Ko (blocs impossibles). Mêmes lignes de cache
// des deux côtés : l'écart ne peut venir que du TLB.
// Rien n'est écrit dans la mémoire du jeu. Le journal reçoit les lignes « [tlb] ».

#ifdef __SWITCH__

#include <switch.h>

#include <malloc.h>
#include <rex/logging.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>

extern "C" {
uint8_t* RexGmBase(void);
size_t RexGmSize(void);
}

namespace {

constexpr uint64_t kPage = 0x1000;
constexpr uint64_t kBloc = 0x200000;
constexpr uint64_t kBandeInvite = 0x10000000;       // zones de la mémoire du jeu regroupées par 256 Mo
constexpr uint64_t kMiniInvite = 16 * 1024 * 1024;  // zone mesurée à partir de cette taille projetée
constexpr size_t kLignesMax = 8192;
constexpr size_t kLignesParPage = 64;
constexpr uint64_t kTailleTas = 64 * 1024 * 1024;
constexpr uint64_t kTailleTemoin = 32 * 1024 * 1024;
constexpr int kBlocs = 3;   // alternances groupé / dispersé
constexpr int kPasses = 5;  // passes par bloc, la première sert à remplir les caches
constexpr int kCycles = 3;  // ensuite plus aucune activité : les temps d'image redeviennent mesurables
constexpr uint32_t kRet = 0xD65F03C0;

struct Plage {
  uint64_t debut;
  uint64_t taille;
};

alignas(64) uint64_t g_groupe[kLignesMax];
alignas(64) uint64_t g_disperse[kLignesMax];
uint64_t g_alea = 0x9E3779B97F4A7C15ull;
Thread g_thread;

uint64_t Alea() {
  g_alea ^= g_alea << 13;
  g_alea ^= g_alea >> 7;
  g_alea ^= g_alea << 17;
  return g_alea;
}

void Melanger(uint64_t* t, size_t n) {
  for (size_t i = n - 1; i > 0; --i) std::swap(t[i], t[Alea() % (i + 1)]);
}

// Lectures en série : chaque adresse de la table dépend de la valeur lue juste avant (eor x, y, y : dépendance d'adresse).
__attribute__((noinline)) uint64_t PasseLecture(const uint64_t* t, size_t n) {
  const uint64_t* fin = t + n;
  uint64_t v = 0, zero = 0, a = 0;
  const uint64_t debut = armGetSystemTick();
  __asm__ volatile(
      "1:\n"
      "  eor %[zero], %[v], %[v]\n"
      "  ldr %[a], [%[p], %[zero]]\n"
      "  ldrb %w[v], [%[a]]\n"
      "  add %[p], %[p], #8\n"
      "  cmp %[p], %[fin]\n"
      "  b.ne 1b\n"
      : [p] "+r"(t), [v] "+r"(v), [zero] "=&r"(zero), [a] "=&r"(a)
      : [fin] "r"(fin)
      : "cc", "memory");
  return armGetSystemTick() - debut;
}

// Appels vers des `ret` de .text : prix du défaut de TLB côté instructions.
__attribute__((noinline)) uint64_t PasseExecution(const uint64_t* t, size_t n) {
  const uint64_t* fin = t + n;
  const uint64_t debut = armGetSystemTick();
  __asm__ volatile(
      "1:\n"
      "  ldr x16, [%[p]], #8\n"
      "  blr x16\n"
      "  cmp %[p], %[fin]\n"
      "  b.ne 1b\n"
      : [p] "+r"(t)
      : [fin] "r"(fin)
      : "x16", "x30", "cc", "memory");
  return armGetSystemTick() - debut;
}

using Passe = uint64_t (*)(const uint64_t*, size_t);

double NsParAcces(uint64_t ticks, size_t n) { return double(armTicksToNs(ticks)) / double(n); }

double Mediane(std::vector<double>& v) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

uint64_t PagesDe(const std::vector<Plage>& plages) {
  uint64_t n = 0;
  for (const Plage& p : plages) n += p.taille / kPage;
  return n;
}

// Page d'indice i dans la suite des plages.
uint64_t PageNumero(const std::vector<Plage>& plages, uint64_t i) {
  for (const Plage& p : plages) {
    const uint64_t n = p.taille / kPage;
    if (i < n) return p.debut + i * kPage;
    i -= n;
  }
  return 0;
}

// Compare les deux tables en blocs alternés ; rend les médianes (ns par accès).
void Comparer(Passe passe, size_t n, double& groupe, double& disperse) {
  std::vector<double> g, d;
  for (int b = 0; b < kBlocs; ++b) {
    for (int p = 0; p < kPasses; ++p) {
      const uint64_t t = passe(g_groupe, n);
      if (p) g.push_back(NsParAcces(t, n));
    }
    for (int p = 0; p < kPasses; ++p) {
      const uint64_t t = passe(g_disperse, n);
      if (p) d.push_back(NsParAcces(t, n));
    }
    svcSleepThread(1'000'000);  // laisse la main aux threads du jeu sur ce cœur
  }
  groupe = Mediane(g);
  disperse = Mediane(d);
}

void MesurerLecture(const char* nom, const std::vector<Plage>& plages) {
  const uint64_t pages = PagesDe(plages);
  for (size_t n : {size_t(2048), size_t(4096)}) {
    if (pages < n) continue;
    const uint64_t pas = pages / n;
    for (size_t k = 0; k < n; ++k) {
      g_disperse[k] = PageNumero(plages, k * pas + Alea() % pas) + (Alea() % kLignesParPage) * 64;
    }
    const uint64_t depart = pages / 2 > n / kLignesParPage ? pages / 2 : 0;
    for (size_t k = 0; k < n; ++k) {
      g_groupe[k] = PageNumero(plages, depart + k / kLignesParPage) + (k % kLignesParPage) * 64;
    }
    Melanger(g_disperse, n);
    Melanger(g_groupe, n);
    double groupe, disperse;
    Comparer(PasseLecture, n, groupe, disperse);
    REXLOG_INFO("[tlb] {} lecture, {} lignes : groupé {:.1f} ns, dispersé {:.1f} ns, écart {:+.1f} ns ({} pages de la zone)",
                nom, n, groupe, disperse, disperse - groupe, pages);
  }
}

void MesurerExecution(const char* nom, const Plage& code) {
  const uint64_t pages = code.taille / kPage;
  for (size_t n : {size_t(2048), size_t(4096)}) {
    if (pages < n) continue;
    // Dispersé : un `ret` tiré au hasard dans la première page qui en a un à partir d'une page tirée au hasard.
    const uint64_t pas = pages / n;
    size_t rempli = 0;
    std::vector<uint32_t> positions;
    for (size_t k = 0; k < n; ++k) {
      const uint64_t depart_page = k * pas + Alea() % pas;
      for (uint64_t essai = 0; essai < 256; ++essai) {
        const uint64_t page = code.debut + ((depart_page + essai) % pages) * kPage;
        const uint32_t* mots = reinterpret_cast<const uint32_t*>(page);
        positions.clear();
        for (uint32_t i = 0; i < kPage / 4; ++i) {
          if (mots[i] == kRet) positions.push_back(i);
        }
        if (!positions.empty()) {
          g_disperse[rempli++] = page + uint64_t(positions[Alea() % positions.size()]) * 4;
          break;
        }
      }
    }
    if (rempli < n) {
      REXLOG_INFO("[tlb] {} exécution, {} lignes : seulement {} `ret` trouvés, abandon", nom, n, rempli);
      continue;
    }
    // Groupé : tous les `ret` de pages consécutives, à partir du milieu du code, au plus un par ligne de cache.
    rempli = 0;
    uint64_t pages_groupe = 0;
    for (uint64_t i = pages / 2; i < pages && rempli < n; ++i) {
      const uint64_t page = code.debut + i * kPage;
      const uint32_t* mots = reinterpret_cast<const uint32_t*>(page);
      ++pages_groupe;
      for (uint32_t ligne = 0; ligne < kLignesParPage && rempli < n; ++ligne) {
        for (uint32_t m = 0; m < 16; ++m) {
          if (mots[ligne * 16 + m] == kRet) {
            g_groupe[rempli++] = page + uint64_t(ligne * 16 + m) * 4;
            break;
          }
        }
      }
    }
    if (rempli < n) {
      REXLOG_INFO("[tlb] {} exécution, {} lignes : seulement {} `ret` groupés, abandon", nom, n, rempli);
      continue;
    }
    Melanger(g_disperse, n);
    Melanger(g_groupe, n);
    double groupe, disperse;
    Comparer(PasseExecution, n, groupe, disperse);
    REXLOG_INFO("[tlb] {} exécution, {} lignes : groupé {:.1f} ns ({} pages), dispersé {:.1f} ns ({} pages), écart {:+.1f} ns",
                nom, n, groupe, pages_groupe, disperse, n, disperse - groupe);
  }
}

// Le second paramètre de virtmemFindAslr est une garde, pas un alignement : l'adresse rendue est seulement alignée sur la
// page. À appeler sous virtmemLock.
void* TrouverAligne(size_t taille) {
  const uint64_t z = reinterpret_cast<uint64_t>(virtmemFindAslr(taille + kBloc, kBloc));
  if (!z) return nullptr;
  void* a = reinterpret_cast<void*>((z + kBloc - 1) & ~(kBloc - 1));
  virtmemAddReservation(a, taille);
  return a;
}

// Témoin : voir l'en-tête. Rend false si une étape échoue (la mesure continue sans lui).
bool PreparerTemoin(uint64_t& aligne, uint64_t& decale) {
  void* source = memalign(kBloc, kTailleTemoin);
  if (!source) return false;
  __builtin_memset(source, 1, kTailleTemoin);
  const Handle proc = envGetOwnProcessHandle();
  virtmemLock();
  void* ombre = virtmemFindCodeMemory(kTailleTemoin, kBloc);
  Result rc = ombre ? svcMapProcessCodeMemory(proc, reinterpret_cast<u64>(ombre), reinterpret_cast<u64>(source), kTailleTemoin)
                    : Result(1);
  void* a = TrouverAligne(kTailleTemoin);
  void* d = TrouverAligne(kTailleTemoin + kBloc);
  virtmemUnlock();
  if (R_FAILED(rc) || !a || !d) {
    REXLOG_INFO("[tlb] témoin : préparation impossible ({:#x})", rc);
    return false;
  }
  rc = svcSetProcessMemoryPermission(proc, reinterpret_cast<u64>(ombre), kTailleTemoin, Perm_Rw);
  aligne = reinterpret_cast<uint64_t>(a);
  decale = reinterpret_cast<uint64_t>(d) + kPage;
  if (R_SUCCEEDED(rc)) rc = svcMapProcessMemory(reinterpret_cast<void*>(aligne), proc, reinterpret_cast<u64>(ombre), kTailleTemoin);
  if (R_SUCCEEDED(rc)) rc = svcMapProcessMemory(reinterpret_cast<void*>(decale), proc, reinterpret_cast<u64>(ombre), kTailleTemoin);
  REXLOG_INFO("[tlb] témoin : ombre {}, aligné {:#x}, décalé {:#x}, résultat {:#x}", ombre, aligne, decale, rc);
  return R_SUCCEEDED(rc);
}

void MesurerTemoin(uint64_t aligne, uint64_t decale) {
  const uint64_t pages = kTailleTemoin / kPage;
  for (size_t n : {size_t(2048), size_t(4096)}) {
    const uint64_t pas = pages / n;
    for (size_t k = 0; k < n; ++k) {
      const uint64_t ecart = (k * pas + Alea() % pas) * kPage + (Alea() % kLignesParPage) * 64;
      g_groupe[k] = aligne + ecart;
      g_disperse[k] = decale + ecart;
    }
    // Même ordre des deux côtés.
    for (size_t i = n - 1; i > 0; --i) {
      const size_t j = Alea() % (i + 1);
      std::swap(g_groupe[i], g_groupe[j]);
      std::swap(g_disperse[i], g_disperse[j]);
    }
    double a, d;
    Comparer(PasseLecture, n, a, d);
    REXLOG_INFO("[tlb] témoin, {} lignes : aligné {:.1f} ns, décalé de 4 Ko {:.1f} ns, écart {:+.1f} ns", n, a, d, d - a);
  }
}

// Adresses physiques d'un échantillon de pages, si le système le permet : un bloc de 2 Mo n'est possible que si
// virtuelle et physique sont alignées de la même façon. Le noyau refuse le code et les vues du jeu (0xD401).
void Physique(const char* nom, const std::vector<Plage>& plages) {
  if (!envIsSyscallHinted(0x54)) {
    REXLOG_INFO("[tlb] {} : svcQueryPhysicalAddress non disponible", nom);
    return;
  }
  const uint64_t pages = PagesDe(plages);
  if (!pages) return;
  constexpr int kEchantillons = 64;
  int ok = 0, memes_2m = 0, memes_64k = 0;
  Result dernier = 0;
  std::vector<uint64_t> tailles;
  for (int i = 0; i < kEchantillons; ++i) {
    const uint64_t va = PageNumero(plages, (pages * uint64_t(i)) / kEchantillons);
    PhysicalMemoryInfo sortie{};
    const Result rc = svcQueryPhysicalAddress(&sortie, va);
    if (R_FAILED(rc)) {
      dernier = rc;
      continue;
    }
    ++ok;
    // Selon le système, l'adresse rendue est celle de la page ou le début du morceau continu : ramenée à la page.
    const uint64_t pa = sortie.physical_address + (va - sortie.virtual_address);
    if (((va ^ pa) & (kBloc - 1)) == 0) ++memes_2m;
    if (((va ^ pa) & 0xFFFF) == 0) ++memes_64k;
    tailles.push_back(sortie.size);
    if (ok <= 2) {
      REXLOG_INFO("[tlb] {} : virtuelle {:#x} -> physique {:#x}, base {:#x}, taille {:#x}", nom, va,
                  sortie.physical_address, sortie.virtual_address, sortie.size);
    }
  }
  std::sort(tailles.begin(), tailles.end());
  REXLOG_INFO("[tlb] {} : {}/{} pages lues (dernière erreur {:#x}), même alignement sur 2 Mo {}, sur 64 Ko {}, taille "
              "continue min {:#x} médiane {:#x} max {:#x}",
              nom, ok, kEchantillons, dernier, memes_2m, memes_64k, tailles.empty() ? 0 : tailles.front(),
              tailles.empty() ? 0 : tailles[tailles.size() / 2], tailles.empty() ? 0 : tailles.back());
}

// Plages lisibles de la fenêtre du jeu, par bandes de 256 Mo. Relu à chaque cycle : la projection change avec le jeu.
std::map<uint64_t, std::vector<Plage>> PlagesInvite() {
  std::map<uint64_t, std::vector<Plage>> bandes;
  const uint64_t base = reinterpret_cast<uint64_t>(RexGmBase());
  const uint64_t fin = base + RexGmSize();
  for (uint64_t a = base; a < fin;) {
    MemoryInfo mi{};
    u32 pi = 0;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a)) || mi.size == 0) break;
    const uint64_t bout = std::min<uint64_t>(mi.addr + mi.size, fin);
    if ((mi.type & MemState_Type) == MemType_ProcessMem && (mi.perm & Perm_R)) {
      // Coupée aux limites des bandes.
      uint64_t d = std::max<uint64_t>(mi.addr, base);
      while (d < bout) {
        const uint64_t bande = (d - base) / kBandeInvite;
        const uint64_t limite = std::min<uint64_t>(bout, base + (bande + 1) * kBandeInvite);
        bandes[bande * kBandeInvite].push_back({d, limite - d});
        d = limite;
      }
    }
    a = bout;
  }
  return bandes;
}

// Tranches de 2 Mo entièrement projetées (blocs possibles) et trouées (blocs impossibles).
void Trous(const char* nom, const std::vector<Plage>& plages) {
  std::map<uint64_t, uint64_t> tranches;
  for (const Plage& p : plages) {
    for (uint64_t a = p.debut; a < p.debut + p.taille;) {
      const uint64_t t = a & ~(kBloc - 1);
      const uint64_t bout = std::min(p.debut + p.taille, t + kBloc);
      tranches[t] += bout - a;
      a = bout;
    }
  }
  size_t entieres = 0, trouees = 0;
  uint64_t dans_trouees = 0;
  for (const auto& [t, octets] : tranches) {
    if (octets == kBloc) {
      ++entieres;
    } else {
      ++trouees;
      dans_trouees += octets;
    }
  }
  REXLOG_INFO("[tlb] {} : {} Mo projetés en {} plages ; tranches de 2 Mo entières {}, trouées {} ({} Mo)", nom,
              PagesDe(plages) / 256, plages.size(), entieres, trouees, dans_trouees / (1024 * 1024));
}

void Segments(uint64_t debut_code) {
  uint64_t a = debut_code;
  for (int i = 0; i < 6; ++i) {
    MemoryInfo mi{};
    u32 pi = 0;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a)) || mi.size == 0) break;
    REXLOG_INFO("[tlb] segment {:#x} : {} Ko, type {:#x}, droits {:#x}, reste sur 2 Mo {:#x}", mi.addr, mi.size / 1024,
                mi.type & MemState_Type, mi.perm, mi.addr & (kBloc - 1));
    a = mi.addr + mi.size;
  }
}

void Fil(void*) {
  svcSetThreadCoreMask(CUR_THREAD_HANDLE, -1, 0x3);  // comme les autres threads : le cœur 2 reste au thread principal
  while (!RexGmBase()) svcSleepThread(1'000'000'000);
  svcSleepThread(60'000'000'000ll);  // laisse le jeu arriver au menu

  MemoryInfo mi{};
  u32 pi = 0;
  svcQueryMemory(&mi, &pi, reinterpret_cast<uint64_t>(&PasseLecture));
  const Plage code{mi.addr, mi.size};
  Segments(mi.addr);
  REXLOG_INFO("[tlb] fenêtre du jeu {:#x}, reste sur 2 Mo {:#x}", reinterpret_cast<uint64_t>(RexGmBase()),
              reinterpret_cast<uint64_t>(RexGmBase()) & (kBloc - 1));

  void* tas = memalign(kBloc, kTailleTas);
  if (tas) {
    __builtin_memset(tas, 1, kTailleTas);
  } else {
    REXLOG_INFO("[tlb] pas de tas de mesure");
  }

  uint64_t aligne = 0, decale = 0;
  const bool temoin = PreparerTemoin(aligne, decale);

  for (int cycle = 1; cycle <= kCycles; ++cycle) {
    REXLOG_INFO("[tlb] === cycle {} ===", cycle);
    if (temoin) {
      MesurerTemoin(aligne, decale);
      MesurerLecture("témoin aligné", {{aligne, kTailleTemoin}});
      MesurerLecture("témoin décalé", {{decale, kTailleTemoin}});
    }
    const std::vector<Plage> code_seul{code};
    MesurerLecture("code", code_seul);
    MesurerExecution("code", code);
    if (tas) {
      const std::vector<Plage> plage_tas{{reinterpret_cast<uint64_t>(tas), kTailleTas}};
      if (cycle == 1) Physique("tas", plage_tas);
      MesurerLecture("tas", plage_tas);
    }
    for (const auto& [bande, plages] : PlagesInvite()) {
      char nom[32];
      snprintf(nom, sizeof nom, "jeu %09llX", static_cast<unsigned long long>(bande));
      Trous(nom, plages);
      if (PagesDe(plages) * kPage < kMiniInvite) continue;
      MesurerLecture(nom, plages);
    }
    svcSleepThread(30'000'000'000ll);
  }
  REXLOG_INFO("[tlb] fin de la mesure");
}

__attribute__((constructor)) void Demarrer() {
  if (R_SUCCEEDED(threadCreate(&g_thread, Fil, nullptr, nullptr, 256 * 1024, 0x2C, -2))) threadStart(&g_thread);
}

}  // namespace

#endif  // __SWITCH__

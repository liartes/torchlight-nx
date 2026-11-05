// torchlight - code du .nro reprojeté en blocs de 2 Mo au démarrage (Switch, -DTORCHLIGHT_CODE_BLOCS=ON).
//
// Le chargeur (hbloader) projette le .nro à une adresse virtuelle quelconque, depuis le début de son tas. Le noyau n'emploie
// un bloc de 2 Mo que si l'adresse virtuelle et l'adresse physique ont le même reste sur 2 Mo : ce n'est presque jamais le
// cas, et les 50 Mo de code recompilé sont en pages de 4 Ko. Un défaut de TLB y coûte plus de 100 ns (src/torchlight_tlb.cpp).
//
// Ce module refait la projection à la MÊME adresse virtuelle (rien à reloger), depuis une copie placée dans le tas de façon
// que virtuelle et physique aient le même reste :
//   1. relevé des segments du .nro et de sa source (le bloc du tas marqué « emprunté », de la même taille) ;
//   2. copie du .nro, cache de données nettoyé ;
//   3. un petit relais, placé dans une page exécutable hors du .nro, retire l'ancienne projection, pose la nouvelle, remet
//      les droits de chaque segment, invalide le cache d'instructions et revient. Pendant ce temps, le .nro n'existe plus :
//      le relais n'utilise que des registres, des appels système et une demande posée sur la pile (hors du .nro).
//
// Conditions : aucun autre thread (constructeur de priorité 101 : le premier thread du SDK est créé à la priorité 102) et pas
// de retour au chargeur, qui voudrait retirer l'ancienne projection : le jeu sort par ISelfController::Exit (rex_app.cpp).
// Tout échec avant le retrait laisse le jeu tel quel, en pages de 4 Ko. L'ancienne source (la taille du .nro) reste perdue.
// Résultat dans le journal : ligne « [code] ». Fichier compilé sans LTO : assembleur de premier niveau.

#ifdef __SWITCH__

#include <switch.h>

#include <malloc.h>
#include <rex/logging.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" {
uint8_t* RexGmBase(void);
extern const char torchlight_relais_debut[];
extern const char torchlight_relais_fin[];
}

// Relais : Result relais(const Demande* x0). Copié dans sa page ; aucune référence hors de lui-même.
// Registres x19-x22 : préservés par les appels système, sauvegardés sur la pile.
__asm__(R"(
  .pushsection .rodata.torchlight_relais,"a"
  .balign 4
  .global torchlight_relais_debut
torchlight_relais_debut:
  stp x19, x20, [sp, #-48]!
  stp x21, x22, [sp, #16]
  stp x29, x30, [sp, #32]
  mov x19, x0
  // Retire l'ancienne projection (rien n'a changé si l'appel échoue).
  ldr w0, [x19, #0]
  ldr x1, [x19, #8]
  ldr x2, [x19, #16]
  ldr x3, [x19, #32]
  svc 0x78
  cbnz w0, 9f
  // Pose la nouvelle.
  ldr w0, [x19, #0]
  ldr x1, [x19, #8]
  ldr x2, [x19, #24]
  ldr x3, [x19, #32]
  svc 0x77
  mov w20, w0
  cbz w20, 2f
  // Échec : remet l'ancienne.
  ldr w0, [x19, #0]
  ldr x1, [x19, #8]
  ldr x2, [x19, #16]
  ldr x3, [x19, #32]
  svc 0x77
2:
  // Droits de chaque segment.
  ldr x21, [x19, #40]
  add x22, x19, #64
3:
  cbz x21, 4f
  ldr w0, [x19, #0]
  ldr x1, [x22, #0]
  ldr x2, [x22, #8]
  ldr w3, [x22, #16]
  svc 0x73
  add x22, x22, #24
  sub x21, x21, #1
  b 3b
4:
  // Cache d'instructions du code (lignes de 64 octets sur le Cortex-A57).
  ldr x21, [x19, #48]
  ldr x22, [x19, #56]
  add x22, x21, x22
5:
  ic ivau, x21
  add x21, x21, #64
  cmp x21, x22
  b.lo 5b
  dsb ish
  isb
  mov w0, w20
9:
  ldp x29, x30, [sp, #32]
  ldp x21, x22, [sp, #16]
  ldp x19, x20, [sp], #48
  ret
  .global torchlight_relais_fin
torchlight_relais_fin:
  .popsection
)");

namespace {

constexpr uint64_t kPage = 0x1000;
constexpr uint64_t kBloc = 0x200000;
constexpr int kSegmentsMax = 8;

struct Segment {
  uint64_t adresse;
  uint64_t taille;
  uint64_t droits;
};

// Disposition lue par le relais.
struct Demande {
  uint64_t processus;    // 0
  uint64_t cible;        // 8
  uint64_t ancienne;     // 16
  uint64_t nouvelle;     // 24
  uint64_t taille;       // 32
  uint64_t nb_segments;  // 40
  uint64_t code;         // 48
  uint64_t taille_code;  // 56
  Segment segments[kSegmentsMax];  // 64
};
static_assert(offsetof(Demande, segments) == 64 && sizeof(Segment) == 24);

// Résultat, lu plus tard par le thread du journal.
const char* g_etat = "non lancé";
Result g_resultat = 0;
Demande g_bilan{};
Thread g_thread;

constexpr bool Code(u32 type) {
  return (type & MemState_Type) == MemType_ModuleCodeStatic || (type & MemState_Type) == MemType_ModuleCodeMutable;
}

void Reprojeter() {
  if (!envIsSyscallHinted(0x73) || !envIsSyscallHinted(0x77) || !envIsSyscallHinted(0x78)) {
    g_etat = "appels système indisponibles";
    return;
  }
  const Handle proc = envGetOwnProcessHandle();
  Demande d{};
  d.processus = proc;

  // Segments du .nro, à partir du bloc qui contient cette fonction.
  MemoryInfo mi{};
  u32 pi = 0;
  if (R_FAILED(svcQueryMemory(&mi, &pi, reinterpret_cast<u64>(&Reprojeter))) || !Code(mi.type) || mi.perm != Perm_Rx) {
    g_etat = "code introuvable";
    return;
  }
  d.cible = mi.addr;
  d.code = mi.addr;
  d.taille_code = mi.size;
  uint64_t a = mi.addr;
  while (d.nb_segments < kSegmentsMax) {
    if (R_FAILED(svcQueryMemory(&mi, &pi, a)) || !Code(mi.type) || mi.perm == Perm_None) break;
    d.segments[d.nb_segments++] = {mi.addr, mi.size, mi.perm};
    a = mi.addr + mi.size;
  }
  d.taille = a - d.cible;

  // La pile doit rester en place.
  const uint64_t pile = reinterpret_cast<uint64_t>(__builtin_frame_address(0));
  if (pile >= d.cible && pile < d.cible + d.taille) {
    g_etat = "pile dans le .nro";
    return;
  }

  // Source du .nro : l'unique bloc du tas marqué emprunté, de cette taille.
  u64 tas = 0, taille_tas = 0;
  if (R_FAILED(svcGetInfo(&tas, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0)) ||
      R_FAILED(svcGetInfo(&taille_tas, InfoType_HeapRegionSize, CUR_PROCESS_HANDLE, 0))) {
    g_etat = "région du tas inconnue";
    return;
  }
  int trouves = 0;
  for (uint64_t p = tas; p < tas + taille_tas;) {
    if (R_FAILED(svcQueryMemory(&mi, &pi, p)) || mi.size == 0) break;
    if ((mi.type & MemState_Type) == MemType_Heap && (mi.attr & MemAttr_IsBorrowed) && mi.size == d.taille) {
      d.ancienne = mi.addr;
      ++trouves;
    }
    p = mi.addr + mi.size;
  }
  if (trouves != 1) {
    g_etat = "source du .nro introuvable";
    return;
  }

  // Relais dans sa propre page exécutable.
  const size_t taille_relais = size_t(torchlight_relais_fin - torchlight_relais_debut);
  void* source_relais = memalign(kPage, kPage);
  if (!source_relais) {
    g_etat = "pas de mémoire pour le relais";
    return;
  }
  std::memcpy(source_relais, torchlight_relais_debut, taille_relais);
  armDCacheFlush(source_relais, kPage);
  virtmemLock();
  void* relais = virtmemFindCodeMemory(kPage, kPage);
  Result rc = relais ? svcMapProcessCodeMemory(proc, reinterpret_cast<u64>(relais), reinterpret_cast<u64>(source_relais), kPage)
                     : Result(1);
  virtmemUnlock();
  if (R_SUCCEEDED(rc)) rc = svcSetProcessMemoryPermission(proc, reinterpret_cast<u64>(relais), kPage, Perm_Rx);
  if (R_FAILED(rc)) {
    g_etat = "page du relais refusée";
    g_resultat = rc;
    return;
  }
  armICacheInvalidate(relais, taille_relais);

  // Copie : même reste sur 2 Mo que la cible. Le tas a virtuelle et physique alignées pareil (mesuré sur la console).
  uint8_t* brut = static_cast<uint8_t*>(memalign(kBloc, d.taille + kBloc));
  if (!brut) {
    g_etat = "pas de mémoire pour la copie";
    return;
  }
  uint8_t* copie = brut + (d.cible & (kBloc - 1));
  d.nouvelle = reinterpret_cast<uint64_t>(copie);
  g_etat = "fait";  // écrit avant la copie : il doit y être aussi
  g_bilan = d;
  std::memcpy(copie, reinterpret_cast<const void*>(d.cible), d.taille);
  armDCacheFlush(copie, d.taille);

  // Plus aucune écriture dans le .nro avant le retour du relais : la copie serait en retard.
  rc = reinterpret_cast<Result (*)(const Demande*)>(relais)(&d);

  g_resultat = rc;
  if (R_FAILED(rc)) g_etat = "échec, ancienne projection gardée ou remise";
}

// Le journal n'existe pas encore à ce moment-là : un petit thread attend qu'il soit prêt.
void Journal(void*) {
  while (!RexGmBase()) svcSleepThread(1'000'000'000);
  REXLOG_INFO("[code] projection du .nro : {} (résultat {:#x}) ; cible {:#x}, reste sur 2 Mo {:#x}, source {:#x} -> {:#x}, {} Ko, {} segments",
              g_etat, g_resultat, g_bilan.cible, g_bilan.cible & (kBloc - 1), g_bilan.ancienne, g_bilan.nouvelle,
              g_bilan.taille / 1024, g_bilan.nb_segments);
}

// Priorité 101 : avant le thread du profileur du SDK (priorité 102).
__attribute__((constructor(101))) void Demarrer() { Reprojeter(); }

__attribute__((constructor)) void DemarrerJournal() {
  if (R_SUCCEEDED(threadCreate(&g_thread, Journal, nullptr, nullptr, 64 * 1024, 0x3B, -2))) threadStart(&g_thread);
}

}  // namespace

#endif  // __SWITCH__

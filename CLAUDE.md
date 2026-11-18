# Projet : recompilation de Torchlight (Xbox 360 XBLA) avec ReXGlue

Objectif : version PC (Linux, faite) puis Switch (publiée : torchlight-nx 1.0.0 le 2026-10-03, 1.0.1 le 2026-10-13,
1.0.2 le 2026-10-15, 1.0.3 le 2026-10-28, 1.0.4 le 2026-11-01, 1.0.5 le 2026-11-05, 1.0.6 préparée le 2026-11-18).
Torchlight = Ogre3D (D3D9RenderSystem porté Xbox), RTTI MSVC présents dans l'exécutable, shaders générés en HLSL à
l'exécution par le RT Shader System d'Ogre (assets/rtshaderlib).

## État et reprise
- **REPRENDRE ICI (2026-11-18, ~11 h 15) : session PGO faite et récupérée ; build de la 1.0.6 lancée (switch/build_joueurs.sh, TORCHLIGHT_VERSION passé à 1.0.6), puis A/B contre la 1.0.5 sur la console (demandé par l'utilisateur).** Session : ~20 min de jeu selon l'utilisateur (journal 10:27-11:04), 11 vidages, 3-4 min en ville à la fin ; pgo/ = 248 .gcda (pgo-105/ : profil de la 1.0.5). Journaux : switch-logs-archive/20261118-session-pgo-lots/ (relojes de base, GPU 460,8). Sauvegarde : 36 « [renommage] » à 00000000, aucune copie de repli, save.tmp disparu, 4.TSV (désormais en majuscules) à jour, **rechargement du personnage confirmé par l'utilisateur** ; copie d'après la session : switch-logs-archive/sauvegarde-console-XJE1-20261118-apres-pgo/. **Ne pas restaurer les sauvegardes d'avant (choix de l'utilisateur : garder la progression).** « [xam differe] » une ligne, 0 ms ; « [fichiers absents] » 0 différence. ordre_natives.py relancé : ordre_fonctions.ld inchangé (pas de nouveau relevé de piles). Build faite (11:26, 385 objets, audit_profil : 0 fonction sans profil) : dist/torchlight-joueurs-1.0.6-20261118-1126.nro (MD5 e9340763, protégé), zip dist/torchlight-nx-1.0.6.zip (copie dist/torchlight-nx-1.0.6-build.zip), ELF switch/elf/torchlight-joueurs-1.0.6-20261118-1126.elf ; sur la console XJE1 avec dist/torchlight-essai-reference.toml (à retirer avant de rendre la console au joueur). Mesure 1.0.6 seule (choix de l'utilisateur) 11:40-11:45, fréquences de base, switch-logs-archive/20261118-mesure-106/, contre 2026-11-05 22:01 (1.0.5) et 2026-11-10 (1.0.5 + cache) avec bandes.py et ouvertures.py : mine immobile 160-165 dessins image 8,10 ms (8,59 / 8,62 : -6 %) ; ville 241-245 image 12,38 (12,68 le 2026-11-10 : -2,4 %) ; ville et mine plafonnées à 60 i/s (écart 16,66) ; combat sans gain visible (150-250 : écart 18,96 contre 18,78 / 18,71 ; 250-320 : 21,51 contre 20,61 / 21,65 ; combats différents) ; images longues démarrage -> ville 29,6 s (29,8 / 30,3), ville -> mine 10,0 s (10,4 / 10,6), attentes de l'avis de sauvegarde (4986 ms, ~990 ms) toujours là (option à -1). Commit « 1.0.6 » sur main et torchlight-nx synchronisé le 2026-11-18 (non poussé) ; zip protégé dist/torchlight-nx-1.0.6-publie.zip (MD5 4b37f5bf, SHA-256 8fb28c3c), texte dist/RELEASE-1.0.6.md (sans chiffres). Reste : push et release à la demande de l’utilisateur ; retirer torchlight.toml de la console.
- **REPRENDRE ICI (état du 2026-11-18, ~10 h) : une session PGO est jouée par l'utilisateur sur la console XJE1 avec la build instrumentée des deux lots du 2026-11-18 (détail des lots : deux points suivants).** Rien n'est commité depuis 81651b1 (tout l'arbre de travail : deux lots + prototype de l'attente de sauvegarde) ; rien de poussé ; TORCHLIGHT_VERSION toujours 1.0.5 (passer à 1.0.6 avant toute build_joueurs.sh) ; aucune version décidée.
  Étapes à la reprise, dans l'ordre :
  1. `console.sh etat` (monter d'abord la console : `gio mount -li | grep mtp` puis `gio mount mtp://-_DBI_XJE10027209225/` ; DBI doit être en mode MTP). Copier switch-logs/ dans switch-logs-archive/ avant toute récupération (piège : `console.sh journaux` remplace switch-logs/).
  2. `console.sh pgo` (vide pgo/ local puis copie celui de la console ; profil de la 1.0.5 déjà gardé dans pgo-105/, 245 .gcda) ; compter les .gcda, demander combien de temps a duré la session et si l'utilisateur est resté 3-4 min après le dernier combat.
  3. `console.sh journaux`, puis dans le journal : lignes « [renommage] » (fermeture / renommage / réouverture de save.tmp, statut 00000000 attendu), aucune erreur « impossible à rouvrir », plus d'avertissement « RenameEntryInternal: ... copié » ; « [xam differe] » (attendu : une ligne après « Begin ») ; « [fichiers absents] ». Sur la carte : save.tmp doit avoir disparu de torchlight/B13EBABEBABEBABE/58410A7E/00000001/torchlight.sav/ et le .tsv du personnage joué être à jour ; demander à l'utilisateur si le personnage se recharge bien. En cas de problème de sauvegarde : copie d'avant dans switch-logs-archive/sauvegarde-console-XJE1-20261118-0930/ (17 fichiers, MD5 vérifiés), option fs_rename_closes_handle = false pour revenir à la copie.
  4. Build finale : profil dans pgo/, ordre des fonctions à refaire si un relevé de piles existe (sinon garder switch/ordre_fonctions.ld), switch/tools/ordre_natives.py, `JOBS=4 CMAKE_OPTIONS="-DTORCHLIGHT_PGO=utiliser" switch/build_switch.sh` (ou build_joueurs.sh après le passage à 1.0.6) ; audit du profil (0 fonction du code généré sans profil ; seul « number of counters » perd le profil).
  5. Mesure A/B contre la 1.0.5 (dist/torchlight-joueurs-1.0.5-20261105-2149.nro) aux fréquences de base avec dist/torchlight-essai-reference.toml : ville et mine immobiles, combat, images longues démarrage -> ville et ville -> mine ; toujours lire « relojes ». Le gain du lot fmadds n'est pas sûr (test de ~15 instructions là où les entrées ne sont pas prouvées : 912 sites sur 5901) : -DREX_FMADDS_DOUBLE_ARRONDI pour un A/B si besoin (demande son propre profil).
  Console XJE1 maintenant : dist/torchlight-pgo-lot-20261118-0958.nro (MD5 625bcd80, ELF switch/elf/torchlight-pgo-lot-20261118-0958.elf), sans torchlight.toml, journaux vidés avant la session (anciens dans switch-logs-archive/switch-logs-avant-pgo-20261118-console/), pas de pgo/ au départ. Le .nro instrumenté contient des chemins locaux (profil) : ne jamais le distribuer.
  Ryzen (allumé par l'utilisateur le 2026-11-18) : arbre et SDK synchronisés sur l'état des deux lots ; ~/essais/bin6 (lot 1), ~/essais/bin7 (lots 1 + 2, compilé avec TORCHLIGHT_VERIF_SIMPLES, option remise à OFF dans switch/out-pc depuis).
  Encore à décider par l'utilisateur, sans urgence : attente de l'avis de sauvegarde (prototype, défaut -1 = comme le jeu ; proposer 0 ou une durée courte) ; -ffp-contract=off sur le code généré de la Switch (exactitude des fmul + fadd en double, effet de vitesse inconnu ; permettrait ensuite les opérations simples ordinaires en float).
- **2026-11-18 : trois reprises du guide renut_nx_ultimate_base (github ChanseyIsTheBest, clone de lecture dans le bloc-notes), NON COMMITÉES, validées sur PC, pas encore sur la console.** (1) fmadds / fmsubs / fnmadds / fnmsubs arrondies une seule fois comme le Xenon (port 96, blur-nx) : switch/tools/fmadds_simples.py (5906 sites, ajouté à build_switch.sh après chargements_flottants ; --annuler, --verifier ; -DREX_FMADDS_DOUBLE_ARRONDI pour un A/B), natives sur la même forme (src/torchlight_fmadds.h, copie à garder identique au bloc du pch ; 160 appels dans torchlight_ogre_natif.cpp). Coût : là où GCC ne voit pas l'origine des valeurs (registres de ctx), un test d'une quinzaine d'instructions ; chaînes de lfs : deux fmadd s. (3) msr fpcr de mtfsf (storeFromGuest) sauté si la valeur ne change pas (port 140) ; le reste était déjà fait par notre SDK (enable/disableFlushMode comparent ; les formes Unconditional sont des bascules réelles). (4) délai de 100 ms de Xenia des appels XAM différés à 0 (kernel_deferred_overlapped_delay_ms, port 143) avec remise à zéro de l'événement (kernel_overlapped_reset_event) ; ligne « [xam differe] » au journal. Constat : seul le premier XamContentCreate (après « Begin ») est différé, les autres sont synchrones ; aucun gain à attendre aux sauvegardes. SDK : include/rex/ppc/context.h, src/system/kernel_state.cpp, switch/patches/nfsmw-sdk.patch mis à jour. Ryzen (~/essais/bin6, session fma-valid de 260 s, --torchlight_ogre_natif=2) : 127 M appels comparés, 0 différence, aucune erreur nouvelle ; aucune sauvegarde écrite pendant la session (à vérifier sur la console). Piège : sur le Ryzen, en-têtes système mis à jour -> supprimer les cmake_pch.hxx.pch de switch/out-pc ; le runtime est dans ~/workspace/nfsmw-nx/sdk/out/linux-amd64/ (librexruntime.so à copier avec le binaire). Reste : build Switch, nouvelle session PGO (le code généré change), A/B contre la 1.0.5.
  **Second lot du 2026-11-18 (même état : non commité, validé sur PC, pas sur la console)** : (1) switch/tools/fmadds_prouvees.py, analyse « doit » par fonction sur les instructions du jeu : 4989 fmadds sur 5901 aux trois entrées prouvées simples -> REX_FMADDS_KNOWN (fmaf seule, sans test), 6 frsp -> copie ; vérification à l'exécution -DTORCHLIGHT_VERIF_SIMPLES=ON (src/torchlight_fmadds.cpp) : 0 preuve fausse en 300 s. Les opérations simples ordinaires (fadds, fmuls…) laissées en double : **le code généré de la Switch est compilé sans -ffp-contract=off**, GCC y fusionne déjà fmul + fadd en double, et des chaînes en float le seraient aussi (décision à part si on veut l'exactitude là). (2) Allocateur par thread (port 151) écarté après mesure (relevé du 2026-11-04 : allocations 0,5 % du thread principal et du rendu, attente du verrou ~0,15 %, 0 % sur les aides) ; à la place, le std::stable_sort des billboards (seule allocation par image de notre code) remplacé par un tri par indices de même ordre (src/torchlight_particules.cpp). (3) switch/tools/lanes_registre.py : 121 lanes de stvewx / stvehx lues dans le registre (sub_821D3638 0,74 % du thread principal, sub_821DB498 0,28 %) ; test switch/tools/test_lanes_registre.py [--aarch64] (12,8 M lectures, 0 différence). (4) Boucle du mélange des textures d'équipement de sub_822DC6B0 sur l'hôte (d'après torchlight-recomp) : torchlight_melange dans src/torchlight_donnees_natif.cpp (fichier passé en -ffp-contract=off), insertion par switch/tools/melange_natif.py ; option torchlight_melange_natif 0/1/2 (2 : native, remise en état, boucle recompilée, comparaison des pixels et des temporaires de la pile) ; 0 différence mais moins de 64 images dans la session (couverture mince), mode 1 sans erreur en 200 s. (5) Sauvegarde : XFile::Rename ferme le descripteur de l'hôte, renomme, rouvre (File::CloseHostHandleForRename / ReopenHostHandleAfterRename dans le SDK, option fs_rename_closes_handle, ligne « [renommage] » au journal) au lieu de la copie complète de save.tmp par CopyInsteadOfRename. **Jamais exercé : les sessions du PC n'écrivent pas la sauvegarde** ; à vérifier sur la console, avec une copie de la sauvegarde avant (ligne « [renommage] », save.tmp disparu, .TSV à jour, rechargement). Les cinq étapes de build_switch.sh sont en fin de chaîne (après billboards_differes). Crédits : THIRD_PARTY_NOTICES.md (renut_nx_ultimate_base, torchlight-recomp). Ryzen : ~/essais/bin7 (session lot2-valid : 166 M appels des natives, 0 différence).
  Session PGO (2026-11-18 ~10 h, voir « REPRENDRE ICI ») : build instrumentée switch/out-pgo (deux lots ci-dessus + prototype de l'attente de sauvegarde à -1), .nro dist/torchlight-pgo-lot-20261118-0958.nro (MD5 625bcd80, ELF switch/elf/torchlight-pgo-lot-20261118-0958.elf) envoyé sur la console XJE1, sans torchlight.toml (copie : dist/torchlight-essai-reference.toml), pgo/ absent au départ, journaux vidés (archivés dans switch-logs-archive/switch-logs-avant-pgo-20261118-console/). Sauvegarde de la console copiée avant : switch-logs-archive/sauvegarde-console-XJE1-20261118-0930/ (MD5 vérifiés ; save.tmp = 3.tsv, reste de l'ancienne copie de repli). Ensuite : `console.sh pgo` (profil de la 1.0.5 déjà copié dans pgo-105/), vérifier [renommage] et save.tmp dans les journaux, puis build finale -DTORCHLIGHT_PGO=utiliser, ordre des fonctions.
- **État du 2026-11-10, 13 h (avant les lots du 2026-11-18). 1.0.5 publiée ; depuis, sur main seulement : cache des fichiers absents (gardé, choix de l'utilisateur) et bilan fpcr inactif.** main : cec40da (cache) plus ces notes ; non commité : prototype de l'attente de sauvegarde (ci-dessous). Rien de poussé vers torchlight-nx, pas de 1.0.6 décidée (TORCHLIGHT_VERSION toujours 1.0.5).
  Console XJE1 le 2026-11-10 : .nro d'ESSAI (dist/torchlight-essai-absents-20261110-1244.nro, MD5 7a616f71, ELF switch/elf/torchlight-essai-absents-20261110-1244.elf, affiche 1.0.5) + torchlight.toml = dist/torchlight-essai-reference.toml (fenêtres de 2 s, images longues au journal). L'utilisateur n'a pas dit s'il voulait remettre la 1.0.5 (dist/torchlight-joueurs-1.0.5-20261105-2149.nro, sans torchlight.toml) : le lui redemander. Ce jour-là la console a démarré avec la mémoire à 2400 MHz et le CPU à 1224 MHz (remis aux fréquences de base par l'utilisateur en cours de session) : toujours lire « relojes ».
  Ryzen joignable le 2026-11-10, arbre synchronisé sur l'état du jour, build PC avec le cache dans ~/essais/bin4 (bin3 : 1.0.5), bin5 : avec le prototype de l'attente de sauvegarde.
  **Trouvé le 2026-11-10 après-midi, à présenter à l'utilisateur (il n'a encore rien décidé) : le jeu endort lui-même son thread principal à chaque sauvegarde.** sub_82344EE0 (r3 objet du jeu, r4 afficher / cacher ; appelée par la sauvegarde sub_8221CC70 et par sub_8221EE10) note l'heure en affichant l'avis de sauvegarde (+6332) et, en le cachant, appelle Sleep (sub_8287D878) pour qu'il soit resté 1 s (3 s si la sauvegarde a dépassé 1 s), ou 5 s si l'octet +276 de l'avis (+980) est levé. Sur la console, dans les deux sessions (2026-11-05 et 2026-11-10), aux mêmes moments : une image de ~4995 ms au démarrage (177-179 dessins) et trois de 992-999 ms (fin de l'arrivée en ville, départ vers la mine, arrivée dans la mine : 240-290 dessins, scène affichée et figée), soit ~6 s des ~30 s d'images longues du démarrage -> ville et ~2 s des ~10,5 s ville -> mine. C'est exactement la priorité de l'utilisateur (réagir dès l'arrivée).
  Prototype NON COMMITÉ et NON VALIDÉ : src/torchlight_attente_sauvegarde.cpp (ajouté à switch/CMakeLists.txt), option torchlight_attente_sauvegarde = -1 (défaut, comme le jeu) ou N ms (0 à 1000 : durée minimale de l'avis ; décale l'heure notée avant l'appel pour que le calcul du jeu donne cette attente). Appels figés remis en sub_82344EE0 dans le code généré (torchlight_recomp.48.cpp et .152.cpp). Compilé sur le Ryzen (~/essais/bin5) ; l'essai de comparaison (-1 contre 0, --torchlight_images_longues=true, essai3.sh att-1 / att0) a été interrompu à la demande de l'utilisateur : à refaire, puis vérifier que la sauvegarde est bien écrite. À décider par l'utilisateur avant d'aller plus loin : c'est un changement visible (l'avis de sauvegarde reste moins longtemps ; aucune image n'est sautée) ; proposer 0 ou une durée courte.
  Reste des images longues (piles du chien de garde, switch-logs-archive/20261110-essai-absents/ : agregat.py et appelants.py <rex_longues.txt> <ELF> [fonctions]) : code du jeu 62-65 % du temps propre, noyau 15-16 % (svcSendSyncRequest 9-10 % : surtout les écritures de la sauvegarde, CopyInsteadOfRename du SDK = copie au lieu du renommage, ~250 échantillons de 2 ms, puis lectures 127 ; svcSleepThread = l'attente ci-dessus), memcpy 2-4 % (dont 125 échantillons sous sub_825EBAA0), inflate 2-3 %. Pistes secondaires : écritures de la sauvegarde hors du thread principal ou sans double copie ; micro-test msr fpcr ; mélange des textures d'équipement sub_822DC6B0 (0,7 %).
  Piège : `console.sh journaux` semble remplacer le contenu de switch-logs/ (les 28 journaux archivés par « --vider » juste avant n'y étaient plus ; 027 et 028 du 2026-11-06 non retrouvés) : copier switch-logs/ dans switch-logs-archive/ avant toute nouvelle récupération.
  État précédent (2026-11-05 au soir) : Release v1.0.5 créée par l'utilisateur le 2026-11-05 (22:14), tag sur torchlight-nx 27b6b20 ; pièce jointe identique à dist/torchlight-nx-1.0.5.zip (SHA-256 d640f420, MD5 cd0698f8 ; copie protégée dist/torchlight-nx-1.0.5-publie.zip). main : 774c195 (« 1.0.5 : à-coups de l'arrivée ») plus ces notes ; torchlight-nx n'a pas encore ces notes (à synchroniser au prochain commit publié). Aucun code en attente.
  Console XJE1 : le .nro publié (dist/torchlight-joueurs-1.0.5-20261105-2149.nro, MD5 894c80be, ELF switch/elf/torchlight-joueurs-1.0.5-20261105-2149.elf), sans torchlight.toml, relevé des piles coupé, pas de pgo/. À vérifier en reprenant : `console.sh etat`.
  Ryzen (joignable le 2026-11-05) : arbre et SDK synchronisés sur l'état de la 1.0.5 (le rsync copie aussi .git), build PC dans ~/essais/bin3 (bin3-avant : sans la vérification du cache de shaders sortie du thread principal). ~/essais/essai3.sh <nom> <s> <cache|vide> [trace] [options] (BIN=<dossier> ; Weston sans écran, manette du donjon, trace des openat par thread avec « trace »). Sur PC, passer les réglages de la console en ligne de commande : --nfsmw_io_ventana_kb=0 --nfsmw_io_cache_mb=64 --nfsmw_io_cache_bloque_kb=64 --nfsmw_io_rangos_mb=0 --nfsmw_io_cache_anticipa=1. Copies des scripts du soir (non versionnées) : switch-logs-archive/outils-session-20261105/ (essai3.sh ; feuilles.py : temps propre par fonction d'un relevé de piles, par plages de l'ELF, images normales et longues à part ; bandes.py : comparaison à dessins égaux entre sessions ; trace des blocs manqués).
  Avant toute nouvelle build_joueurs.sh : passer TORCHLIGHT_VERSION à 1.0.6 dans switch/CMakeLists.txt.
  Session du 2026-11-06 (commitée le 2026-11-10 avec le cache : src/torchlight_coeurs.cpp, bilan sous TORCHLIGHT_COMPTE_FPCR, inactif ailleurs ; fin du journal) : revue des pratiques propres à la Switch contre le code. Déjà faits ou clos : cœur 2 réservé, grandes pages (sauf vues 0xA0/0xC0/0x7F, 0-2 %), tas, TLS, compilateur, mode 4 cœurs, boost. Ouverts : (1) bascules du mode flottant (msr fpcr, FZ commun au scalaire et à NEON) comptées sur PC : 860-1210 par image en ville (build switch/out-fpcr/build, copie du SDK en liens physiques) ; plafond ~0,3-2 % selon le coût inconnu de msr fpcr sur l'A57, à trancher par un micro-test sur la console avant tout correctif ; (2) entrées-sorties de l'arrivée : fsSetPriority(FsPriority_Realtime) et __nx_fs_num_sessions (3 par défaut) de libnx jamais utilisés, index des dossiers en mémoire ; (3) memcpy de newlib 3,35 % (~1 %). Prochaine étape au choix de l'utilisateur. Ryzen injoignable le 2026-11-06.
  **Cache des fichiers absents (2026-11-10, codé, validé sur PC, mesuré sur la console, commité)** — mesure (switch-logs-archive/20261110-essai-absents/, .nro d'essai dist/torchlight-essai-absents-20261110-1244.nro 7a616f71, resté sur la console XJE1 avec torchlight.toml de référence) : ouvertures des images longues 15 099 -> 764 (779 -> 186 ms) au démarrage -> ville, 5536 -> 14 (277 -> 84 ms) ville -> mine, 0 différence ; durée totale des images longues inchangée au bruit près (~2 % du total seulement) : poste clos, le reste des arrivées est du calcul. Détail : src/torchlight_fichiers_absents.cpp, d'après torchlight-recomp (YaPeL, commit d3d79f8, src/hooks/find_file_hooks.cpp et missing_file_cache.{h,cpp}, GPL-3.0, crédité dans THIRD_PARTY_NOTICES.md). Interception de sub_82883A68 (recherche native sous FindFirstFileA sub_8287E0C8 : r3 = OBJECT_ATTRIBUTES, racine +0, nom +4 : ANSI_STRING ; coupe le chemin à la dernière '\', ouvre le dossier par NtOpenFile, puis cherche le nom). Les 12 683 ouvertures en échec de la mesure du 2026-11-05 sont ces ouvertures de dossiers (game:\music|programs|RTShaderLib\<dossier>\, GAME:\MEDIA\...). Deux niveaux, à la différence de l'original : dossier absent (vérifié une fois par ResolvePath du VFS ; le SDK rend 0xC000000F dans tous les cas, le statut ne départage pas) -> toute recherche dans ce dossier rend le statut retenu ; dossier présent, fichier absent -> retenu par chemin complet sans joker. Seulement GAME:, racine ObDosDevices, nom non vide. Option torchlight_fichiers_absents (0 coupé, 1 défaut, 2 validation : original toujours appelé, statuts comparés ; ligne « [fichiers absents] » du journal toutes les 4096 recherches). Appel figé remis en sub_82883A68 dans le code généré (torchlight_recomp.3.cpp, seul site, dans sub_8287E0C8 ; à refaire après une régénération). Ryzen (~/essais/bin4, session du donjon de 260 s) : 12 288 recherches, 11 781 évitées (96 %), 0 différence en mode 2 ; modes 0 et 1 tracés : mêmes fichiers du jeu ouverts. Sur PC ces échecs ne touchent pas le disque (aucun gain à y attendre) ; le gain est à mesurer sur la console avec dist/torchlight-essai-reference.toml (postes « ouverture » des images longues : 779 ms démarrage -> ville, 277 ms ville -> mine avant). Pas de nouveau profil PGO nécessaire. Écarté : leur patch de skinning (RTSS d'OGRE 14 ; nos shaders indexent déjà la palette d'os en place). Possible ensuite : boucle de mélange des textures d'équipement sub_822DC6B0 sur l'hôte (leur 38 -> 21 ms ; nous n'avons que pack/unpackColour).
  Pistes pour la suite (rien de commencé) : ouvertures de fichiers à l'arrivée dans une zone (280 à 780 ms par phase dans les images longues de la mesure de 22:01 : le plus gros poste d'entrées-sorties restant) ; rejouer les quadrilatères des fenêtres de l'interface inchangées (estimé 2-3 % en combat, davantage au menu) ; laisser le thread principal continuer pendant les aides (état partagé) ; étaler les créations d'unités (logique du jeu) ; ville +4 % de la 1.0.5 contre les mesures du 2026-11-04, jamais expliquée (torchlight_interface = 0 ou ancien ordre des fonctions pour départager ; l'essai du GPU est écarté). Jamais testés sur la 1.0.5 (publiée sans la session finale de 30 min, choix de l'utilisateur) : dock, veille, sessions longues, batterie et chauffe à 460,8 MHz, blocs du noyau (2821 au plus dans la session PGO, sans erreur).
  Décisions de l'utilisateur : GPU à 460,8 MHz en portable par défaut, pas de boost en jeu, pics des archers laissés tels quels (2026-11-04) ; mesure du plafond rendu / GPU abandonnée, « ça ne me parle pas » : ne pas la reproposer (2026-11-05).
  Limite qui reste : en combat lourd, ~24-29 ms entre images (relevé actif), attente du GPU quasi nulle : le thread principal limite, profil plat.
- **Historique (2026-11-05) : préparation et publication de la 1.0.5.** Détail, essai par essai : fin de switch/RENDU_NATIF.md.
  - Session PGO 13:12-13:46 (34 min, 10 vidages, ~30 s finales hors profil, switch-logs-archive/20261105-session-pgo/) ; pgo/ = cette session (245 .gcda ; seul torchlight_init.cpp, tables de données, n'en a pas), profil précédent dans pgo-natives-seules/. Ordre des fonctions refait (2962 fonctions, pilas-archive/rex_pilas-20261104-2003.txt) puis ordre_natives.py. Audits : 0 fonction du code généré sans profil. Rappels : « 0 coverage mismatch » ne prouve rien pour le code généré ; -Wno-missing-profile masque un .gcda absent ; « source locations » garde le profil, seul « number of counters » le perd.
  - Premier candidat (.nro d8867f5f, dist/torchlight-nx-1.0.5-candidat1.zip) mesuré à 19:31 (switch-logs-archive/20261105-mesure-finale/) : aucun gain visible contre les sessions du 2026-11-04 ; environ un quart de moins que le recompilé du profil 1.0.4. Crédits ajoutés : zlib, poses de torchlight-rekindled. Sources poussées une première fois (5325c43) pour partager le patch du SDK (correctif de la fenêtre mémoire : lien vers 5325c43, lignes 357-366 de switch/patches/nfsmw-sdk.patch).
  - Étude « autres gains à bas niveau » (relevé du 2026-11-04 20:03) : sous le code du jeu, ~8 % du thread principal, dont la moitié d'attente ; rien d'aussi gros que le TLB en régime normal ; les images longues passaient 13 % de leur temps en requêtes au système. D'où quatre correctifs, intégrés à la 1.0.5 à la demande de l'utilisateur : vérification du cache de shaders sur le thread de compilation (src/torchlight_natif.cpp) ; dans le SDK, lecture en avance d'un bloc du cache de lecture (nfsmw_io_cache_anticipa = 1 dans src/torchlight_app.h et switch/torchlight.toml), recherche d'AllocRange qui saute les groupes de 64 pages pleins (heap_busqueda_rapida, heap_busqueda_verif pour comparer), journal vidé par un thread à part avec un tampon de 64 Ko (log_flush_hilo, log_file_buffer_kb). Validés sur le Ryzen (8448 recherches identiques, 5058 fichiers du zip justes) ; mesurés sur la console à 22:01 (switch-logs-archive/20261105-mesure-candidat2/) : lectures sur la SD -70 %, images longues démarrage -> ville -17 %, ville -> mine -9 %, allocations physiques -87 à -89 %, régime stable sans recul. Piège : les lignes « T: » de rex_pilas.txt sont les échantillons des images longues.
- **Acquis du 2026-11-04, tous sur main.** Bilan transmissible : switch/DECOUVERTES_2026-11-04.md (aussi en page privée https://claude.ai/artifact/6EeZLZfPzMs63p4JZ3iznv, à republier si le fichier change, script dans switch-logs-archive/outils-session-20261104/). Détail, essai par essai : fin de switch/RENDU_NATIF.md.
  Mémoire : fenêtre du jeu alignée sur 2 Mo (bug du SDK, virtmemFindAslr : combat -15 à -17 %), code du .nro reprojeté (src/torchlight_code_blocs.cpp, -2 à -4 %), vue 0xE0000000 (0 à -2 %). Rendu : géométrie persistante (src/natif/rendu.cpp, ville -7,4 %, mine -7,0 %, combat écart -4,7 à -6,3 %), pause dans l'attente du GPU (sub_82774170), avertissement de Mesa écrit une fois sur 1000. Chargement : inflate et lectures du zip (src/torchlight_inflate.cpp), primitives natives (src/torchlight_donnees_natif.cpp), atomiques sans verrou global (switch/tools/atomiques_sans_verrou.py), seuil du boost à 400 ms. GPU : torchlight_gpu_mhz (src/torchlight_boost.cpp). Interface : torchlight_interface, mode 3 par défaut (reconstruction seulement si une fenêtre dessinée a changé ; console, vrai combat : 14 400 images, 0 différence ; peu de gain en combat, barres et chiffres de dégâts changent vraiment). 13 natives de plus et verrous à courte attente des aides, validés sur PC ; gain seulement avec le nouveau profil. ZCULL essayé : aucun gain, gardé coupé.
  Sans mesure de gain à ce jour : interface, aides, ces 13 natives.
  Essais sur ce PC, sans le Ryzen (injoignable le 2026-11-04) : fenêtre sur le bureau de l'utilisateur, à prévenir ; pas de Weston ici. switch-logs-archive/outils-session-20261104/ : essai.sh <nom> <s> [options] (session scriptée, ville et menus, données switch/out-pc/donnees-console-20261103), lance_sans_gdb.py (autorise l'attachement de gdb : ptrace_scope = 1) et gdb_redessin.py (point d'observation), tranches.py et episodes.py (piles par scène, rex_longues.txt), asm.py (asm d'une fonction). La manette du Ryzen ne convient pas ici : le menu est prêt plus tôt, les appuis se décalent ; gdb dès le démarrage les décale aussi.
- Natives (demande de l'utilisateur : top 50 même pour un gain minime, puis les 50 suivantes) : fin de src/torchlight_ogre_natif.cpp, 109 fonctions : 49 du top 50 (sub_821D5410 exceptée), 37 des 50
  suivantes, le reste de rang inférieur ou auxiliaires. Option -DTORCHLIGHT_NATIVES_FIXES (Switch, ON par défaut) : sans alternance, validation ni repli ; OFF pour un A/B, qui demande alors le profil de
  l'alternance. Validées sur le Ryzen (mode 2 : 122 M appels en 240 s, 0 différence, dont 1,32 M de free du tas ; 15 sessions de 260 s en donjon sans plantage ; capture vérifiée). Couverture PC :
  lignes « [couverture] » du journal ; 6 natives jamais atteintes, leur seul appelant étant devenu natif. Aucun défaut vu par l'utilisateur dans les menus et les dialogues.
  Mesures sur la console (fin du journal) : sans profil pour elles, **aucun gain** (switch-logs-archive/20261103-essai-natives/) ; avec le profil de l'alternance (dist/torchlight-essai-natives-pgo.nro,
  80008c80), dans la session ville -6,2 ± 1,4 %, mine -4,5 ± 0,7 %, combat -7,3 ± 3,5 %, **mais écart surestimé : ce profil ralentit le recompilé de ~3 % à l'arrêt** ; de session à session contre le
  recompilé d'avant : ville -3,7 %, mine -1,5 %, combat non comparable (switch-logs-archive/20261103-essai-natives-pgo/). Poses du rendu (torchlight_poses_rendu) : pas de gain et p99 du combat pire
  dans les deux sessions (+21 ± 19 %, +16 ± 15 %) : coupées. Laissées de côté : sub_821D5410 (événements froids, que les essais ne couvrent pas), bibliothèque C du jeu (sub_82883C50
  attente, sub_8287F408, sub_8287F090, sub_82867A10, sub_821BC898 printf), inflate (sub_823E1590, sub_823E31D8), sub_8247F888 (chargement d'un maillage), sub_821F3F28 et sub_821A6D90 (anneau et
  registres du GPU émulé), sub_821BA040 (Direct3D du jeu), sub_821DA090 (billboards, déjà en parallèle). Après une régénération du code : switch/tools/appels_figes.py ; après une nouvelle
  native ou un nouvel ordre : switch/tools/ordre_natives.py. Méthode et pièges : fin du journal.
  Essais sur le Ryzen : `ssh ryzen`, mêmes chemins, jeu sans écran par Weston ; `rsync -a --exclude-from=<exclusions> ./ ryzen:workspace/torchlight/`, `cmake --build switch/out-pc -j 12`, binaire copié dans
  ~/essais/bin2 ; `BINAIRE=$HOME/essais/bin2/torchlight MANETTE=$HOME/essais/manette-donjon.txt ~/essais/veille.sh <nom> <s> [options]` (le jeu démarre ~1 min après : compter large),
  ~/essais/serie.sh 3 260 5. Copies des scripts du bloc-notes et du Ryzen (non versionnées, elles citent des chemins locaux) : switch-logs-archive/outils-session-20261103/ (show.sh : asm d'une
  fonction depuis le code généré ; suivants.py : classement du profil hors crochets ; rsync-exclusions.txt).
- Historique (2026-11-03, commité depuis). Trois essais, sur la console dans dist/torchlight-essai-coeur-bb-recyclage.nro
  + .toml (torchlight_alterner). Mesuré (fin du journal) : ville -3,95 %, mine -4,17 % tout activé : cœur -2,1 à -2,4 %,
  recyclage -1,5 %, billboards -0,2 à -0,35 % (peu de particules). Combat non concluant (± 7 %) : à refaire avec des
  fenêtres courtes. Analyse : switch-logs-archive/20261103-essai-coeur-bb/analyse_ab.py. La console a torchlight.toml.
  Étude des poses faite sur PC, rien de codé. switch/out-joueurs contient cette build, plus la 1.0.4.
  Poses : prototype src/torchlight_poses.cpp (--torchlight_poses, --torchlight_poses_double, défauts coupés), validé sur PC
  et mesuré sur la console (fin du journal) : combat -4,1 à -4,6 %, ville ~-1 %, mine -0,3 % ; billboards et seconde instance
  plus petits. Analyse : switch/tools/ab_regression.py. Validation à l'octet en vrai combat faite (103 923 poses, 1 différente, aucun défaut vu ; décalage d'une image accepté par l'utilisateur, souvenir
  rendu-intact). Reste : valeurs par défaut pour une version. Appels figés
  remis en sub_X dans le code généré (non versionné) : sub_821BB578, sub_821B9478, sub_8219D288, sub_821E2D90.
- (2026-11-01, nuit). **1.0.4 publiée** : main (commit « 1.0.4 »), torchlight-nx poussé sur GitHub
  le 2026-11-01 ; release v1.0.4 à créer par l'utilisateur avec dist/torchlight-nx-1.0.4.zip (MD5 52a8bab9, refait
  à la main avec le .nro testé dist/torchlight-joueurs-1.0.4-20261101-2145.nro, MD5 6e78a8dd, et le README aux
  crédits de renut-nx ; zip de build_joueurs.sh gardé en dist/torchlight-nx-1.0.4-build.zip) et
  dist/RELEASE-1.0.4.md (sans chiffres). ELF switch/elf/torchlight-joueurs-1.0.4-20261101-2145.elf. La console XJE1
  a ce .nro (pgo/ supprimé, pas de torchlight.toml). Profil PGO de pgo/ : session 6 (code de main 589625f) ; celui
  de la 1.0.3 dans pgo-103/. Protéger le zip publié (copie dist/torchlight-nx-1.0.4-publie.zip) avant tout
  build_joueurs.sh, et changer TORCHLIGHT_VERSION avant.
  - Mesures (fréquences de base, switch-logs-archive/20261101-joueurs-104/) : ville 242 dessins 17,42-17,60 ms
    (1.0.3 : 18,25-18,38, -4,5 %) ; mine ~14,6 ms (184 dessins) ; combat 352 dessins 28,7 ms. L'essai nonarg avec
    l'ancien profil faisait 17,13-17,36 en ville : ~1,5 % de mieux, non expliqué (décalages rlwinm jamais mesurés
    seuls, ou profil n°6) ; un A/B nonarg / 1.0.4 dans la même session le dirait (dist/torchlight-essai-nonarg.nro).
  - Contenu depuis la 1.0.3 : ctx.lr non écrit (switch/tools/retour_sans_lr.py), non_argument_as_local refaite
    (générateur du SDK modifié, nfsmw-sdk.patch ; liste switch/non_arguments_gardes.txt par
    switch/tools/audit_non_arguments.py ; à refaire si le manifeste change), rlwinm/rlwimi en décalages
    (switch/tools/decalages_rlwinm.py, test exhaustif), nouveau profil PGO. Écarté : mémoire du jeu sans volatile
    (pas de gain ; risque d'exactitude : setcsr sans clobber mémoire, voir switch/tools/memoire_non_volatile.py),
    sections critiques en ligne (0,28 %), natives du chemin du dessin (setVertexBufferBinding, setPass : journal
    du 2026-11-01 soir). Pistes restantes : concurrence sur le verrou de malloc de newlib (~1,7 % du thread
    principal, hors code généré), mode 4 cœurs (renut-nx docs/four-core-mode.md), changements de mode des denormals
    (24 661 sites, à compter d'abord), écritures de registres mortes (dead_ctx_defs, risqué). La doc de renut-nx :
    https://github.com/ChanseyIsTheBest/renut-nx (docs/codegen-enhancements.md surtout).
- Historique (2026-10-28, soir). **1.0.3 publiée** : main 25ccc79 (+ commit des notes), torchlight-nx 6624ad2
  poussé sur GitHub le 2026-10-28 ; release v1.0.3 à créer par l'utilisateur (texte fourni) avec
  dist/torchlight-nx-1.0.3.zip (MD5
  259af13c ; .nro 3e94a750 = dist/torchlight-joueurs-1.0.3-20261028-2037.nro ; ELF
  switch/elf/torchlight-joueurs-1.0.3-20261028-2037.elf) et dist/RELEASE-1.0.3.md (sans chiffres, crédits
  torchlight-recomp et torchlight-rekindled). La console XJE1 a ce .nro, sans torchlight.toml. Les notes de fin de
  session (CLAUDE.md, journal) ne sont pas encore dans torchlight-nx : les synchroniser au prochain commit publié.
  - Profil PGO de pgo/ : session 5 du 2026-10-28 (combats compris) ; précédents gardés dans pgo-20261028/ et
    pgo-20261021/. Toute modification du code généré demande une nouvelle session (build instrumentée
    switch/out-pgo, -DTORCHLIGHT_PGO=generer -DTORCHLIGHT_LTO=OFF -DTORCHLIGHT_OUTILS=OFF ; dire au joueur de rester
    3-4 min dans le jeu après le dernier combat : vidage toutes les 3 min, la fin est perdue si on quitte par HOME).
  - Mesures de la 1.0.3 sur la console (fréquences de base, switch-logs-archive/20261028-joueurs-final/) : ville
    242 dessins 18,25-18,4 ms (42 images/s), mine ~14,2 ms (51-52), combat ~370 dessins ~32 ms (37 ms avec le jeu
    d'origine, session A du 2026-10-28) ; mode boost pendant tous les chargements, jamais en combat.
  - Contenu depuis la 1.0.2 : blocs de géométrie statique hors écran retirés (src/torchlight_blocs.cpp), liaisons et
    états du Direct3D du jeu sautés, écouteur du RT Shader System et getKeyFramesAtTime natifs (d'après
    torchlight-recomp) ; boucle des contrôleurs en liste plate, dormants sautés (--torchlight_controleurs) ;
    particules de ParticleUniverse sur plusieurs cœurs (src/torchlight_particules.cpp, --torchlight_particules 2,
    combat ~-8-10 %) ; natives de la logique (sub_821A1510, sub_822F92A0, sub_821D7D38, sub_82312D98) et du rendu
    (sub_821CC998 RenderPriorityGroup::clear, sub_821CF100 Material::getBestTechnique) ; mode boost du processeur
    pendant les chargements (src/torchlight_boost.cpp, --torchlight_boost_chargement). Détail : fin du journal.
  - Diagnostic : le jeu est limité par son thread principal (99 % d'un cœur en combat, attente du rendu < 1 ms) ;
    GPU à 460 MHz sans effet ; CPU à 1224 MHz : +12 % en ville. Thread principal en combat (profil
    pilas-archive/rex_pilas-20261028-1520.txt) : dessin des files 25 %, interface 7,7 %, recherche des objets
    visibles 18,5 %, contrôleurs 8,6 %, logique de Runic 30 % ; profil plat (aucune fonction du jeu > 2,2 %).
  - Pistes restantes : sommets des billboards en parallèle (BillboardRenderer::_updateRenderQueue sub_821DA090,
    3,96 % en combat, gain attendu ~1,5-2 % ; étude dans le journal du 2026-10-28) ; délai de sortie du mode boost à
    1 s (un chargement apparaît en plusieurs morceaux dans le journal) ; refus « rendu » des particules (systèmes
    non billboards, ~1-8 par image). Écarté : logique de Runic en parallèle, parcours de l'octree par caméra en
    parallèle, dessin (un seul périphérique), poses des squelettes (torchlight-rekindled : la logique pose chaque
    unité pendant sa mise à jour), recouvrir logique et rendu (scène partagée).
  - Outils nouveaux : switch/tools/suivi_memoire/ (build PC de diagnostic qui suit chaque accès mémoire pendant les
    mises à jour des systèmes de particules : preparer.py <dépôt> <sortie>, puis cmake/ninja dans la sortie ;
    TORCHLIGHT_SUIVI_FICHIER, TORCHLIGHT_SUIVI_TACHES=1 avec --torchlight_particules=1 ; incrémental). Manette de
    script qui fait parcourir la mine (vrais déplacements et attaques) : journal du 2026-10-28 (stick 8 s dans une
    direction puis A répété, X, Y ; 8 fois).
  - Pièges du 2026-10-28 : ninja ne suit pas les .gcda (build_joueurs.sh supprime désormais les objets plus anciens
    que le profil) ; build_joueurs.sh refait toujours l'édition des liens (le .nro change) : pour refaire seulement
    le zip, le reconstruire à la main avec le .nro testé ; un thread du jeu qui appelle du code recompilé doit
    réserver un cadre sous le sommet de sa pile (l'appelé écrit dans le cadre de l'appelant) ; le bloc-notes de
    session peut être vidé entre deux reprises (scripts de manette, piles symbolisées : à refaire).
  - Code généré modifié (non versionné, à refaire après toute régénération, comme les autres natives) : appels figés
    remis en sub_X pour sub_821A1510 (~90 fichiers), sub_822F92A0 (6, 64), createController sub_824B4C78 (9),
    ~Controller sub_824B5A10 (216), sub_821CCDD0 (119), sub_8258F8F8 (103), sub_825A4C68 (74, 94), sub_8247A588
    (64), sub_821A1CA8 (145), sub_821D7D38 (69 fichiers), sub_82312D98 (3), sub_821CC998 (193), sub_821CF100
    (24 sites).
  - Zips gardés : dist/torchlight-nx-1.0.1-publie.zip (ff78834e), dist/torchlight-nx-1.0.2-publie.zip (f6f94267) ;
    versions intermédiaires de la 1.0.3 (premier eec6c786, refaite febc143e, 20261021-1133 9fce6dce, pgo4 c608ea1c),
    supprimables.
- 1.0.2 publiée le 2026-10-15 (torchlight-nx 6f0b346, release faite par l'utilisateur) : dist/torchlight-nx-1.0.2.zip
  (MD5
  f6f94267, .nro MD5 d6685a15, ELF switch/elf/torchlight-joueurs-1.0.2-20261015-2011.elf ; profil PGO de pgo/ du
  2026-10-15 soir, aucune incompatibilité), texte de release dist/RELEASE-1.0.2.md (performances seulement, sans
  chiffres, à la demande de l'utilisateur ; README sans chiffres d'images/s non plus). Build des joueurs vérifiée
  sur la console XJE1 sans torchlight.toml (switch-logs-archive/20261015-joueurs-102/). torchlight-nx 6f0b346
  « torchlight-nx 1.0.2 », poussé sur GitHub le 2026-10-15 ; release v1.0.2 créée par l'utilisateur (zip +
  dist/RELEASE-1.0.2.md). Zip 1.0.1 publié gardé :
  dist/torchlight-nx-1.0.1-publie.zip (MD5 ff78834e).
  Contenu depuis la 1.0.1 : appels indirects promus, constantes de .rdata, chargements flottants, -fno-math-errno,
  registres locaux (feuilles, boucles), natives walkOctree / largeur des textes de CEGUI / contrôleurs, relevé
  préchargé, -ffp-contract=off pour les natives. Dialogues vérifiés sur la console. Mesure du soir (build de test
  équivalente, fréquences de base) : ville 272-277 dessins 20,2-20,9 ms (36-38 images/s) ; mine 198-199 dessins
  15,6-15,9 ms (41-42) ; pas de référence 1.0.1 valable à 1600 MHz (celles du 2026-10-14 étaient à 2400).
  Depuis la 1.0.2 (2026-10-20, d'après torchlight-recomp de YaPeL, https://github.com/YaPeL/torchlight-recomp,
  GPL-3.0, clone de lecture dans le bloc-notes de session) : liaisons de tampons, mode polygone et sprites du
  Direct3D du jeu sautés (src/torchlight_trace.cpp, torchlight_natif_liaisons) ; blocs de géométrie statique hors
  de l'écran non mis en file pour la caméra principale (src/torchlight_blocs.cpp, torchlight_blocs 0/1/2), validé
  à l'octet sur PC (mine, ville, outil hors ligne avec EXCLURE) ; A/B sur la console aux fréquences de base : ville
  ~-4 % (39-40 -> 41-42 images/s), mine ~-28 % (33-35 -> 44-45). Essai : dist/torchlight-essai-blocs.nro (non
  publié). Copie de la sauvegarde de la console (5 personnages) : switch/out-pc/donnees-console-20261020 (manette
  « 15:a,20:a,25:a » : plus d'appuis ouvrent un dialogue). Sur PC, --torchlight_natif_seul=true n'est pas activé par
  défaut (piège des A/B). Leur dépôt offre aussi : écouteur du RT Shader System en natif, mods PC (dossier
  replacements simple ; mods de données moyen ; nouveaux objets lourd), icônes de boutons Nintendo (Kenney, CC0).
  Pistes restantes (< 1,2 % chacune) : addRenderable sub_821C3730, BillboardRenderer sub_821DA090,
  getSquaredViewDepth sub_821C4DF8, recherche par nom sub_821C04C8.
  Pas encore fait : A/B propre des natives sur la console (même build, même endroit, torchlight_ogre_natif = 0 dans
  torchlight.toml puis 1) ; mesure de la 1.0.1 aux fréquences de base. À savoir : GCC (Switch, mode GNU) contracte
  aussi a * b + c en fma dans le code généré pour les opérations en double précision (fmul puis fadd du jeu) :
  écart possible avec le PowerPC, jamais observé ; -ffp-contract=off seulement sur torchlight_ogre_natif.cpp.
  Une native ajoutée : ses appels déjà figés en __imp__sub_X dans le code généré sont à remettre en sub_X (sed),
  appels_directs.py les respecte ensuite (adresse citée dans src/).
  Méthode de mesure : personnage immobile 1 min en ville puis dans la mine juste après l'arrivée, mêmes endroits,
  et TOUJOURS vérifier la ligne « relojes » de rex_perfil.log des deux sessions (une session est passée à 2400 MHz
  de mémoire sans que l'utilisateur le sache : profil sys-clk du jeu ?). Avant de supprimer le .nro de la console,
  vérifier que le fichier de remplacement existe (le dossier temporaire peut disparaître).
  Pipeline du code généré (build_switch.sh) : constantes_lecture_seule.py (si switch/out-pc/image_jeu.bin existe,
  sinon refaire switch/tools/image_jeu.py), chargements_flottants.py, appels_indirects.py --promouvoir,
  registres_locaux_feuilles.py --toutes, registres_locaux_boucles.py, appels_directs.py, pile_non_volatile.py ; tous idempotents. Après une régénération du code : nouveau relevé des appels indirects (PC,
  torchlight_releve_appels_indirects), nouvelle liste, nouvelle image si besoin, nouveau profil PGO.
  Zip publié 1.0.1 : dist/torchlight-nx-1.0.1.zip (MD5 ff78834e), à protéger avant tout build_joueurs.sh.
- 2026-10-14 : appels indirects promus en appels directs gardés (switch/tools/appels_indirects.py, lancé par
  build_switch.sh avant appels_directs.py ; liste switch/appels_indirects_promus.txt, 381 sites, 90 % des appels
  indirects ; relevé et vérification PC : src/torchlight_appels_indirects.cpp) avec un nouveau profil PGO : -13 % en
  ville, -15 % dans la mine sur la console (A/B contre la 1.0.1, journal). Essai prêt : dist/torchlight-essai-
  indirects.nro (non publié, version encore 1.0.1). La liste vaut pour le code généré exact : après une
  régénération, refaire le relevé (torchlight_releve_appels_indirects sur PC) puis la liste, puis le profil PGO.
  Limites d'intégration du LTO : essai sans gain, abandonné (f929bb4). Lot 2 (même soir) : constantes de .rdata
  repliées (switch/tools/constantes_lecture_seule.py, image du jeu par switch/tools/image_jeu.py), chargements
  flottants dans l'unité vectorielle (switch/tools/chargements_flottants.py), -fno-math-errno : -4 à -6 % de plus aux
  fréquences de base (ville ~20 ms, 37-38 images/s ; mine ~15 ms, 50 images/s). Essai : dist/torchlight-essai-lot2.nro
  (non publié). Prochaine étape possible : une 1.0.2 (nouvelle session PGO si le code change encore).
- 1.0.1 publiée le 2026-10-13 (torchlight-nx 9a52619, release v1.0.1 créée par l'utilisateur ; détail dans
  « Problèmes ouverts et tâches »), vérifiée sur la console XJE1 sans torchlight.toml. Reste à confirmer : le
  poisson de la pêche (bug intermittent selon l'utilisateur, retour attendu après une session longue). Journaux du
  test : switch-logs-archive/20261013-v101/. Ancienne libvulkan.a (avant le correctif GPFIFO) gardée dans
  ~/workspace/mesa-switch/nvk-sdk-sauvegarde-20261011/ (supprimable).
- Pour tester sur PC avec la sauvegarde de la console : copie dans switch/out-pc/donnees-switch (0.tsv renommé
  0.TSV) ; lancer natif_pc.sh avec DONNEES modifié ; manette de script : A vers 15 s suffit pour « Begin ».
- Switch : jouable en rendu natif Vulkan, sauvegardes OK, clavier virtuel (swkbd), appuis fantômes corrigés.
  Performances aux fréquences de base (CPU 1020 / GPU 307,2 / mémoire 1600 MHz), test n°23 (LTO + PGO) : ville
  21,7-25 ms de rendu côté jeu (31-35 images/s sans profil, 29-30 avec), mine 17-19 ms (39-47 images/s) ; test
  n°22 (même code avant le PGO et les réécritures du soir) : 28,7 ms, 27 images/s. Mesure « image » des bilans
  [natif] = _updateAllRenderTargets seulement : la logique du jeu (~6 ms) s'y ajoute (35 images/s pour 22 ms).
- Publié le 2026-10-03 : dépôt public https://github.com/liartes/torchlight-nx (~/workspace/torchlight-nx, commit
  b3893eb ; release v1.0.0 avec dist/torchlight-nx-1.0.0.zip, .nro MD5 3e8f995a, créée par l'utilisateur sur GitHub).
  Voir « Distribution ». Dernière build : switch/out-joueurs (LTO + PGO, sans outils de développement), avec le
  correctif des blocs mémoire du noyau (granule de 2 Mo, vérifié sur la console) et sans chemins locaux dans le .nro.
  Le README publié n'est pas à jour sur deux points, laissés tels quels par l'utilisateur : mode TV « testé seulement
  en forcé » (testé depuis sur un vrai dock) et 30-35 images/s en ville (25-29 sur une Switch V1 Erista).
- Consoles : principale (forwarder Sphaira, mémoire 1600 MHz) ; MTP XAJ7… = Switch V1 Erista (title override Fitness
  Boxing 2, mémoire 1331 MHz, tests du 2026-10-03) ; MTP XJE1… = troisième console, livrable joueur installé le
  2026-10-03 (sans torchlight.toml). Sur XJE1, le renommage en MTP échoue (« libmtp : could not get property
  description ») : copier sous le nom final, sans .tmp + rename (console.sh le fait déjà).
- Limite : le thread principal (Ogre ~74 %, logique ~25 %). Profil plat ; voir « Performances » pour la suite.
- Consignes de l'utilisateur : optimiser aux fréquences de base (CPU 1020 MHz, RAM et GPU d'origine ; le surcadençage
  reste au choix du joueur ; exception voulue par l'utilisateur le 2026-11-04 : GPU au profil officiel de 460,8 MHz en portable) et sans rien changer au rendu (pas d'ombres une image sur deux, pas de mise à jour
  sautée : risque de clignotement). Répondre en français. Tests n°1 à 21 faits avec RAM et GPU surcadencés ;
  à partir du n°22, fréquences de base (ligne « relojes » de rex_perfil.log).
- Pour reprendre : lire la fin du journal switch/RENDU_NATIF.md (chaque essai, mesures, pièges). Restent les tests
  sur la console de « Problèmes ouverts et tâches » (sessions longues, zones lourdes, sauvegardes). Tout correctif
  publié : commit sur main ici, puis synchronisation de torchlight-nx par un commit normal, push seulement à la
  demande. Ensuite possible : parcours de l'octree natif (~0,5 ms, validation difficile), sous-allocation Vulkan des
  textures si les blocs du noyau redeviennent justes. Boutons ABXY et icônes : laissés tels quels (choix de
  l'utilisateur, peut-être plus tard).

## Règles
- Sauvegarder torchlight_manifest.toml avant toute modification.
- Ne renommer ni supprimer de fichiers dans assets/ sans demander.
- Les fichiers du jeu viennent de la copie légitime de l'utilisateur. Ne jamais versionner ni distribuer de données
  dérivées du jeu : shaders compilés (la console les compile elle-même, raison légale), images (captures d'écran
  comprises : switch/images/ est ignoré, retiré du dépôt le 2026-10-03 mais encore dans l'historique). nro-icon.png
  n'est pas une image du jeu (trouvée ailleurs par l'utilisateur) : versionnée.
- Dépôt destiné à être publié (GPL-3.0, LICENSE et THIRD_PARTY_NOTICES.md) : textes vus par les joueurs (fenêtres,
  README.md) en anglais ; documentation de développement (CLAUDE.md, journal) en français.
- Données utilisateur PC : ~/.local/share/torchlight/ (sauvegardes, cache) : ne pas supprimer. Les essais se font
  sur une copie (switch/out-pc/donnees-test, faite par switch/natif_pc.sh).

## Version PC (recompilation d'origine)
SDK : ~/rexglue-sdk-0.10.0-linux-amd64/linux-amd64 (Clang 20, CMake, Ninja). Manifeste : torchlight_manifest.toml.
- ./run.sh : génère, reconfigure (cmake --preset), compile, lance, affiche le FATAL du log.
- Lancement : LD_LIBRARY_PATH=<sdk>/lib ./out/build/linux-amd64-release/torchlight --game_data_root=$PWD/assets
  --gpu_plugin=xenos --license_mask=1 --log_file=torchlight.log (license_mask=1 : version complète, pas la démo).
- Débogage : build debug (cmake --build --preset linux-amd64-debug), runtime librexruntimed.so. Sous gdb, toujours
  `handle SIGSEGV nostop noprint pass` et `handle SIG35 nostop noprint pass` (le runtime rattrape les SIGSEGV des pages
  surveillées 0xA0000000+ ; SIG35 = signal temps réel du runtime).
- ./check_absorbed.sh (fonctions absorbées par une voisine), ./find_candidates.sh (dump gdb, propositions d'adresses :
  surtout des adresses de retour, ne rien ajouter sans vérifier).
Manifeste, ce qu'on a appris :
- FATAL « invalid or unregistered function at 0x... » : ajouter `0x... = {}` dans [entrypoint.functions], régénérer,
  puis reconfigurer (cmake --preset), sinon les sources générées ne sont pas prises en compte.
- Fonction déclarée sans taille qui avale sa voisine : `{ end = 0x... }`. Une entrée au milieu d'une fonction la coupe
  (REX_FATAL « Unresolved branch ») : vérifier prologue (mflr/stwu) et fin de la fonction précédente.
- Critère fiable : aucune fonction générée ne recouvre l'entrée, et 0 « ERROR: conditional branch to unknown address ».
- 130 petites fonctions récupérées dans les trous (llvm-mc-20 -triple=powerpc-unknown-unknown) : section en fin de
  manifeste.
- Logs bénins : échecs NtCreateFile (game:\music\..., game:\programs\...) et ResolvePath(SAVE:\) (le jeu essaie
  plusieurs chemins).
- /dev/shm/xenia_memory_* : jamais libérée en hard-exit ; /dev/shm plein => « Erreur du bus » au lancement. Supprimer
  ceux qu'aucun processus n'ouvre (fuser) ; switch/natif_pc.sh et switch/trace_pc.sh le font.

## Version Switch
Base : le port Need for Speed MW (~/workspace/nfsmw-nx, commit 66611e3, SDK modifié dans sdk/, BSD-3 ; guide
docs/porting-another-game.md ; son rendu natif docs/native-renderer.md, GPL-3). Rendu Vulkan via NVK
(~/workspace/mesa-switch, branche locale `nfsmw` = 1a8c1a66d6f + patch nfsmw). Console : Atmosphère, lancer par un
forwarder (Sphaira, recommandé : ressource système propre) ou en title override (maintenir R : ressource système du
titre remplacé, parfois aucune), pas depuis l'album.

### Chaîne d'outils
- Docker : image devkitpro/devkita64 (utilisateur dans le groupe docker ; sinon `sg docker -c "..."`).
- Générateur rexglue du SDK nfsmw-nx (hôte) : cmake -S ~/workspace/nfsmw-nx/sdk -B ~/workspace/nfsmw-nx/out/host -G Ninja
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 -DGIT_EXECUTABLE=/bin/false
  -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2 ; cible rexglue
  (GIT_EXECUTABLE=/bin/false : sinon rex_version.cmake échoue sur les tags de nfsmw-nx ; x86-64-v2 : sinon erreurs ssse3).
- Dépendances du SDK : tools/fetch_thirdparty.py de nfsmw-nx (corrigé : liens symboliques copiés sans les suivre) ;
  dossiers .git vides dans sdk/thirdparty/*/ (exigés par CMake). sdk/thirdparty n'est pas suivi par git.
- Mesa NVK : sg docker -c ./build-switch.sh (branche nfsmw), puis switch/assemble_nvk_sdk.sh ->
  ~/workspace/mesa-switch/nvk-sdk/opt/devkitpro/portlibs/switch. Pièges : partie Rust compilée pour x86-64 si
  MESA_SWITCH_RUST_TARGET n'est pas défini (corrigé dans build-switch.sh et rustc-switch-wrapper.sh, non commité) ;
  index de libvulkan.a à refaire avec aarch64-none-elf-ranlib (assemble_nvk_sdk.sh). Contrôle :
  `ar x libvulkan.a && file *.o | grep -c x86-64` doit donner 0. Recompilation incrémentale : docker start
  mesa-switch-build, ninja dans le conteneur, puis assemble_nvk_sdk.sh.
- Modifications locales des dépôts clonés, à réappliquer si on reclone : switch/patches/ (nfsmw-sdk.patch pour sdk/,
  glslang-verrou.patch pour sdk/thirdparty/glslang, mesa-switch-build.patch, mesa-switch-zcull-toggle.patch,
  mesa-switch-gpfifo-reserve.patch).
- Code généré : switch/gen_manifest.sh (manifeste Switch depuis celui du PC, à relancer après toute modification),
  puis cd switch && ~/workspace/nfsmw-nx/sdk/out/linux-amd64/rexglue codegen torchlight_manifest.toml (~3 min ;
  44052 fonctions). Piège : un `includes` mal placé dans le manifeste est ignoré sans erreur.
- Compilation : JOBS=4 switch/build_switch.sh -> switch/out/torchlight.nro (aussi l'icône : nro-icon.png à la racine,
  versionnée). switch/carte_sd.sh -> switch/out/sd/ (.nro + torchlight.toml) et garde
  l'ELF non strippé dans switch/elf/ (symboliser les plantages).
- PC avec le SDK nfsmw-nx (pour développer le rendu natif) : cmake -S switch -B switch/out-pc ... -DREXSDK_DIR=
  ~/workspace/nfsmw-nx/sdk, puis switch/natif_pc.sh [--torchlight_natif_seul=true] (lance sur une copie des données).
  Outils de shaders (pour switch/shaders/build_lib.sh, bibliothèque de développement) : switch/out-pc/outils.

### Rendu natif (switch/RENDU_NATIF.md : plan, journal de chaque essai)
L'émulation du GPU Xenos saturait la console (~1-3 images/s puis gel). Le rendu natif redessine en Vulkan ce que le
D3D9RenderSystem d'Ogre reçoit ; l'émulation reste mais ne dessine plus rien.
- src/natif/rendu.{h,cpp} : cœur Vulkan (aussi utilisé par l'outil hors ligne switch/tools/rendu_hors_ligne,
  résultat identique à l'octet : test de non-régression sur switch/out-pc/trace-mine-6/capture).
- src/torchlight_natif.cpp : thread du jeu = relevé de chaque image (Frame : commandes, DrawDesc, copies des
  sommets/indices/constantes) ; thread de rendu dédié = rejeu Vulkan, présentation ; thread de compilation des shaders.
  Threads créés par pthread depuis SetPaths (std::thread lève std::system_error sur la console, depuis un
  thread du jeu comme depuis celui de l'interface : toujours pthread_create avec une taille de pile explicite).
- src/torchlight_trace.cpp : interceptions des méthodes du RenderSystem (cases : switch/ogre/D3D9RenderSystem.txt).
- Shaders : sources HLSL du jeu interceptées (D3DXCompileShader), préparées (src/natif/hlsl_prep.cpp, portage
  de switch/shaders/prep_hlsl.py, identique à l'octet) et compilées par glslang dans le jeu (hlsl_compiler.cpp) ;
  cache <données utilisateur>/cache/natif (*.tlsh + pipelines.bin).
- Options : --torchlight_natif (rendu natif), --torchlight_natif_seul (les dessins ne vont plus au Direct3D du jeu),
  --torchlight_natif_thread (thread de rendu, vrai par défaut), --torchlight_natif_ecrans=60,100 (captures d'écran),
  --torchlight_capture_s=T (capture d'une image pour l'outil hors ligne), --torchlight_trace (traceur, lent),
  --torchlight_manette="20:a,24:a,..." (manette de script). switch/torchlight.toml active natif et natif_seul.
- Pièges majeurs (détails dans le journal) : Ogre 1.7 met à jour ses tampons D3D dans _render (lire après l'original) ;
  CEGUI réécrit son tampon de sommets au même endroit dans une image ; indices d'os UBYTE4 à retourner comme un mot ;
  décalage de profondeur (_setDepthBias, facteur -1/250000) indispensable (traînées d'ombre sinon) ; verrou global de
  glslang réinitialisé par le SDK (corrigé par glslang-verrou.patch) ; sur la console, std::ifstream lève des
  exceptions sur erreur de lecture, std::filesystem liste mal les dossiers, et un fichier ouvert ne peut pas être
  renommé (sauvegardes : copie de repli dans le SDK).

### Carte SD et console
- sdmc:/switch/torchlight/ = torchlight.nro + torchlight.toml + tout le contenu de assets/. Le runtime y crée
  torchlight/ (sauvegardes, cache) et logs/ (journaux ; logs/rex/ : rex_perfil.log, rex_crash.log, rex_pilas.txt).
- switch/console.sh (console branchée en MTP via DBI) : etat, envoyer, pilas-on|pilas-off, journaux [--vider]
  (-> switch-logs/), vider-cache.
- Profil : `console.sh pilas-on` (fichier logs/rex/perfil_pilas.flag) -> rex_perfil.log contient les piles par thread,
  et rex_pilas.txt les échantillons bruts (piles complètes, « thread|pc retours... » relatifs à l'image). Symboliser
  avec llvm-symbolizer-20 --obj=switch/elf/<ELF du .nro testé>. Toujours `pilas-off` ensuite (ralentit le jeu).
- Bilan [natif] toutes les 10 s dans le journal : images/s, ms par image côté jeu (image, relevé, attente du rendu),
  thread de rendu (Vulkan, présentation), pipelines et textures créés, dessins ignorés, shaders absents.

## Distribution (préparée le 2026-10-03)
- Nom public : torchlight-nx (portage non officiel, nom du dépôt, du zip et du menu homebrew) ; dossier sur la carte :
  sdmc:/switch/torchlight (inchangé). Icône : nro-icon.png de ce dépôt (sans lien avec le marketing du jeu).
- Dépôt public (release GitHub) : ~/workspace/torchlight-nx, seul dépôt publié (pas de branche release ici). Un seul
  commit sans historique (l'historique d'ici contient des captures du jeu), dont les fichiers sont ceux de main ici.
  Après chaque commit à publier : cd ~/workspace/torchlight-nx && git fetch ~/workspace/torchlight main &&
  git read-tree -u --reset FETCH_HEAD, puis un commit normal (message en anglais) : publié sur GitHub le
  2026-10-03 (b3893eb, release v1.0.0), plus de --amend ni de push forcé. Pousser seulement à la demande. Vérifier : git rev-parse HEAD^{tree} égal à main^{tree} d'ici. Remote origin
  git@github.com:liartes/torchlight-nx.git (clé ~/.ssh/id_ed25519_github, core.sshCommand local), auteur
  Liartes <13268731+liartes@users.noreply.github.com>. Aucune trace de l'identité réelle de l'utilisateur (ni dans
  les fichiers, ni dans le .nro : -fmacro-prefix-map, vérifier avec strings -a ... | grep -ci <nom d'utilisateur>).
- Le joueur copie seulement torchlight.nro et son paquet XBLA dans sdmc:/switch/torchlight (torchlight.toml devient
  facultatif : ses valeurs sont les valeurs par défaut du .nro, OnPostInitLogging de src/torchlight_app.h ; garder
  les deux en accord).
- switch/build_joueurs.sh : build LTO + PGO sans outils de développement (-DTORCHLIGHT_OUTILS=OFF : manette de script
  et capture d'écran ; TORCHLIGHT_NATIF reste) dans switch/out-joueurs, puis dist/torchlight-nx-<version>.zip
  (switch/torchlight/ : .nro, README.md, LICENSE, THIRD_PARTY_NOTICES.md). Version : TORCHLIGHT_VERSION dans
  switch/CMakeLists.txt (1.0.5).
- Lancement en mode applet (album) : message bloquant à l'écran (appletGetAppletType != Application) au lieu du
  simple avertissement du SDK.
- Le .nro contient le code de default.xex recompilé (comme les .nro de nfsmw-nx) : zone grise assumée dans le README ;
  le dépôt, lui, ne contient ni code généré ni données du jeu.

## Performances : outils et pistes
Outils (voir le journal) :
- Profil de la console : `console.sh pilas-on` (puis pilas-off), switch/tools/pilas.py (rex_pilas.txt + ELF de
  switch/elf/ -> piles symbolisées, thread « Main XThread (F »), profil_arbre.py (temps inclusif/propre),
  profil_descente.py, profil_noms.py (cases de tables virtuelles depuis les RTTI : /tmp/image.bin relue dans
  /proc/<pid>/mem à 0x182000000, puis tools/rtti_vtables.py). Les ~30-40 premiers % des échantillons sont souvent
  le chargement : découper par tranches. `console.sh journaux` archive rex_pilas.txt dans pilas-archive/.
- Profil du PC : perf record --call-graph dwarf (kernel.perf_event_paranoid=1) ; temps PC peu fiables si la
  machine est chargée (comparer en A/B alterné).
- Fonctions natives (src/torchlight_ogre_natif.cpp, --torchlight_ogre_natif=2 : validation contre le recompilé,
  au bit près, compteur « [ogre natif] validation ») ; memcpy/memset du jeu par l'hôte (src/torchlight_crt.cpp).
  Après toute nouvelle interception : vérifier que le code généré n'appelle pas directement __imp__sub_X
  (appels_directs.py fige les appels des fonctions alors non interceptées) et remettre sub_X.
- Build Switch : appels directs, pile sans volatile (tools/pile_non_volatile.py), ordre des fonctions
  (switch/ordre_fonctions.ld, aussi .text.hot.* pour le PGO), LTO (OUT=switch/out-lto, ~1 h), PGO :
  build instrumentée OUT=switch/out-pgo CMAKE_OPTIONS="-DTORCHLIGHT_PGO=generer -DTORCHLIGHT_LTO=OFF", session de
  jeu, `console.sh pgo`, puis -DTORCHLIGHT_PGO=utiliser. Le profil vaut pour le code généré exact : toute
  régénération (gen_manifest, codegen) demande une nouvelle session. OUT est rendu absolu par build_switch.sh.
Fait (2026-10-01/02) : cache D3DXCompileShader, Lock sans attente, géométrie reprise, interceptions sans verrous,
registres PowerPC en variables locales + pile sans volatile (+ blocage au démarrage = course de l'anneau dans le
SDK, corrigée), appels directs, ordre des fonctions, LTO, PGO ; en natif (au bit près) : frustum, produit de
matrices, bindGpuProgramParameters court, KeTlsGetValue (SDK), sommets des billboards, animation des squelettes
(nœuds, applyToNode, images clés, matrices des os), concatenateAffineMatrices, _updateAutoParams (10 types) ;
relevé allégé (tables plates, pages vérifiées) ; lectures de pak.zip par cache de blocs de 64 Ko (64 Mo).
Écarté : skip_lr / non_argument_as_local (section critique jamais relâchée), sauter des mises à jour d'images.
Options d'essai du 2026-11-04 : --torchlight_interface (2 : validation à l'octet), --torchlight_journal_stats (bilans au journal, noms des fenêtres de l'interface), --torchlight_images_longues et --torchlight_images_longues_ms (chien de garde, rex_longues.txt), --torchlight_gpu_mhz.
Fait aussi (2026-10-14) : appels indirects promus (-13/-15 %). Fait le 2026-10-27/28 : boucle des contrôleurs en
liste plate, particules de ParticleUniverse sur plusieurs cœurs (src/torchlight_particules.cpp ; état partagé
analysé avec switch/tools/suivi_memoire/), natives de la logique et du rendu, mode boost des chargements (voir
« État et reprise »). Pistes restantes (ville, console, test n°23) : textures de rendu 207 (ombres, 5 dessins) et 208 (lumière, 40
dessins) ~4 ms dont ~2/3 de recherche des objets visibles ; parcours de l'octree ~1,1 ms propre + 0,3 ms de
visibilité des octants (réécriture native : ~0,5 ms, validation indirecte à prévoir) ; _updateRenderQueue des
particules (~1 ms), de la géométrie statique (~0,8 ms) ; contrôleurs (particules, ~2 ms par image) ; accesseurs de
matrices d'AutoParamDataSource ; régénérer l'ordre des fonctions avec pilas-archive/rex_pilas-20261002-test23.txt
et switch/elf/torchlight-20261002-2313.elf. Gros levier restant : sortir une partie du travail d'Ogre vers un autre
cœur (chantier lourd).

## Problèmes ouverts et tâches
- Version 1.0.1 préparée le 2026-10-13 (dist/torchlight-nx-1.0.1.zip, .nro MD5 926eb485, ELF
  switch/elf/torchlight-joueurs-1.0.1-20261013-2005.elf ; texte de release dist/RELEASE-1.0.1.md) : plantage
  « device lost » (GPFIFO de NVK, mesa-switch-gpfifo-reserve.patch), petites textures (côté <= 16 texels : poisson
  de la pêche), langue de la console (user_language ; une valeur fixée dans torchlight.toml, même 1, est respectée :
  test de la source du réglage), écran des succès, fond du menu dès le démarrage ; README mis à jour. Poisson à
  revoir après une session plus longue (bug intermittent selon l'utilisateur). Poussé sur GitHub le 2026-10-13
  (torchlight-nx 9a52619) ; release v1.0.1 créée par l'utilisateur.
- Écran des succès du menu principal (2026-10-13, fait, vérifié sur la console : affichage, défilement, fermeture
  par B) : XamShowAchievementsUI (stub du SDK) ouvre une fenêtre ImGui (xam_ui.cpp, nfsmw-sdk.patch) avec la liste
  de l'AchievementManager (noms dans la langue de user_language), affichage bloquant comme la boîte de message ;
  défilement croix/stick, A ou B ferme. Seulement depuis le menu (pas de raccourci en jeu, choix de l'utilisateur).
- Fond du menu principal d'après le dernier personnage dès le démarrage (2026-10-13, fait, vérifié
  sur la console : « fond du démarrage d'après SAVE:\2.TSV : trouvé ») : src/torchlight_menu_fond.cpp (interception de sub_8221DC20, option torchlight_menu_fond).
  Le jeu construit le menu à 3,5 s, avant « Begin » (sub_82212950 état 0) : sub_8221DC20 ouvre la sauvegarde dont le
  chemin est au champ +5388 de l'objet du jeu et y lit le niveau (fond = MAINMENURULES du niveau), sinon ville
  (sub_82217248, mainmenu_townrules.dat 0x820AFDD8). Ce chemin (« SAVE:\0.TSV ») n'est rempli qu'en choisissant un
  personnage et SAVE: n'est monté qu'après « Begin » : toujours la ville (probablement comme sur la Xbox).
  L'interception retient le chemin (menu_dernier_personnage.txt) puis, au démarrage suivant, copie le fichier dans
  menu_fond/ (données utilisateur), monte cette copie en lecture seule sous SAVE: le temps de l'appel et passe le
  chemin au jeu. Piège : le jeu garde ce fichier ouvert ; retirer le périphérique faisait planter le SDK à « Begin »
  (std::bad_alloc dans ContentManager::CloseOpenedFilesFromContent) : périphérique gardé, lien SAVE: retiré ; la
  copie protège la vraie sauvegarde (fichier ouvert non remplaçable sur la console). Tracés gdb :
  switch/tools/gdb_menu_fond.py, gdb_menu_fichier.py.
- Switch, gels de 5-6 s sans plantage (2026-10-03 22:09 et 22:14, switch-logs-archive/20261003-2203-gels/) : thread
  principal du jeu à 100 % CPU, thread de rendu inactif, ni E/S ni mémoire en cause. À faire : session avec
  `console.sh pilas-on` jusqu'à un gel, puis profil de l'intervalle du gel. (Particules roses du sort de protection :
  effet normal, vérifié sur PC contre l'émulation.) Non reproduit le 2026-10-04 : session de 74 min avec le relevé
  des piles (build des joueurs 1.0.0), seulement deux ralentissements de chargement de zone ; au plus 2197 blocs
  du noyau et 188 trozos, aucune erreur (switch-logs-archive/20261004-0908-longue/,
  pilas-archive/rex_pilas-20261004-longue-joueurs100.txt). Relancer pilas-on si les gels reviennent.
- Switch, blocs mémoire du noyau épuisés (0xCE01, 2026-10-03, version joueurs sur une autre console : ville en
  partie noire puis console figée après ville-mine-ville) : granule de confirmation porté à 2 Mo (SDK) et compteur
  de blocs dans rex_perfil.log (ligne « nucleo: ») : corrigé et vérifié sur la console (148-164 trozos, ~1950 blocs,
  ville affichée correctement ; journal, fin de RENDU_NATIF.md). En title override, la ressource système est celle du
  titre remplacé (Fitness Boxing 2 : aucune) ; un forwarder (Sphaira) a la sienne. Mode TV (720p mis à l'échelle),
  veille et bouton HOME : vérifiés sur cette console (Switch V1 Erista, mémoire à 1331 MHz : ville 25-29 images/s).
- Switch, boutons ABXY : A/B et X/Y inversés par rapport à la manette Xbox 360 ; laissé tel quel à la demande de
  l'utilisateur (2026-10-03), peut-être plus tard.
- Tests sur la console restants (mode TV, veille, HOME, build des joueurs, message du mode applet et démarrage sans
  torchlight.toml faits le 2026-10-03) : sessions longues (1-2 h) et zones lourdes (boss, niveaux profonds :
  stabilité, mémoire, ligne « nucleo: » du profil), sauvegardes (création, chargement, suppression, quitter pendant
  une sauvegarde).
- Switch, icônes des boutons : remplacer les icônes Xbox 360 par des icônes au style Switch (textures
  media/ui/icons2/widgets*.dds) : peut attendre ; l'utilisateur doute de l'intérêt sans changer la disposition des
  boutons.
- Installation depuis le paquet XBLA (fait le 2026-10-03, PC et Switch ; vérifié sur PC et sur la console) :
  src/torchlight_installation.{h,cpp}, appelé par OnFinalizePaths (avant la construction du runtime, fenêtre ImGui,
  messages en anglais à la demande de l'utilisateur). Paquet (type 0x000D0000, titre 58410A7E) cherché dans le
  dossier du jeu et dans 58410A7E/000D0000 ; table des empreintes src/torchlight_paquet_complet.inc (SHA-256 des
  57 fichiers de la version complète ; le jeu n'a plus de mises à jour, pas d'autre version à prévoir) ; témoin installation_paquet.txt
  écrit en dernier. Sur la console, le dossier du .nro est pris s'il contient default.xex ou un paquet.
  Piège : AvatarAwards (fichiers du jeu) est aussi un conteneur STFS (PIRS, type 2). Le lecteur STFS du SDK plante
  sur un paquet tronqué : taille vérifiée avant (en-tête arrondi à 4 Ko + content_size).
- PC, clavier virtuel (XamShowKeyboardUI, saisie du nom) : plantage intermittent, assertion ImGui `buf != NULL` dans
  KeyboardInputDialog::OnDraw (timing dans le SDK, non confirmé).
- PC avec l'émulation Xenos : saccades à la compilation des shaders (cache store_shaders actif, s'améliore avec le
  temps ; -march=native sans effet).

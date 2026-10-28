# Projet : recompilation de Torchlight (Xbox 360 XBLA) avec ReXGlue

Objectif : version PC (Linux, faite) puis Switch (publiée : torchlight-nx 1.0.0, 2026-10-03 ; 1.0.1 préparée le
2026-10-13).
Torchlight = Ogre3D (D3D9RenderSystem porté Xbox), RTTI MSVC présents dans l'exécutable, shaders générés en HLSL à
l'exécution par le RT Shader System d'Ogre (assets/rtshaderlib).

## État et reprise (session du 2026-10-14, soir)
- REPRENDRE ICI (2026-10-28, soir). **1.0.3 publiée** (commit et push de torchlight-nx le 2026-10-28 ; release v1.0.3
  à créer par l'utilisateur avec dist/torchlight-nx-1.0.3.zip, MD5 259af13c, .nro 3e94a750 =
  dist/torchlight-joueurs-1.0.3-20261028-2037.nro, ELF switch/elf/torchlight-joueurs-1.0.3-20261028-2037.elf ; texte
  dist/RELEASE-1.0.3.md). Profil PGO de pgo/ : session 5 du 2026-10-28 (combats compris) ; précédents dans
  pgo-20261028/ et pgo-20261021/. Vérifiée sur la console XJE1 sans torchlight.toml (switch-logs-archive/
  20261028-joueurs-final/) : ville 242 dessins 18,25-18,4 ms (42 images/s), combat ~370 dessins ~32 ms (37 ms avec
  le jeu d'origine), mode boost pendant tous les chargements et jamais en combat ; chargements plus courts
  (utilisateur). Contenu depuis la 1.0.2 : voir dist/RELEASE-1.0.3.md et la fin du journal (blocs hors écran,
  liaisons sautées, natives d'après torchlight-recomp ; boucle des contrôleurs en liste plate ; particules sur
  plusieurs cœurs, src/torchlight_particules.cpp ; natives de la logique et du rendu ; mode boost des chargements,
  src/torchlight_boost.cpp). Crédits ajoutés : torchlight-rekindled (README, THIRD_PARTY_NOTICES, release).
  Pistes restantes (journal, 2026-10-28) : sommets des billboards en parallèle (~1,5-2 % en combat, étude faite) ;
  délai de sortie du mode boost à 1 s (les chargements apparaissent en plusieurs morceaux dans le journal). Le jeu
  est limité par son thread principal (GPU sans effet, CPU à 1224 MHz : +12 % en ville).
  Piège : build_joueurs.sh refait toujours l'édition des liens (le .nro change) ; pour refaire seulement le zip,
  le reconstruire à la main avec le .nro testé.
  Code généré modifié (non versionné, à refaire après une régénération) : appels remis en sub_X pour les
  interceptions des particules (sub_821CCDD0, sub_8258F8F8, sub_825A4C68, sub_8247A588, sub_821A1CA8) et des natives
  (sub_821D7D38, sub_82312D98, sub_821CC998, sub_821CF100), en plus de ceux du 2026-10-27 ci-dessous.
  Fait le 2026-10-27 (commits 5cc256b, 5d60883 ; détail : fin du journal) : boucle des contrôleurs en liste plate,
  dormants sautés (--torchlight_controleurs 1 par défaut, 0 = référence A/B) ; préchargement par vagues retiré (il
  ralentissait) ; natives de la logique sub_821A1510 (recherche par nom) et sub_822F92A0 (rejet du segment contre les
  objets). Console (fréquences de base, sans relevé), contre la 1.0.3 refaite : mine 16,3 -> 14,7-14,8 ms par image
  (48 -> 50-51 images/s), ville 19,2 -> 18,1-18,4 ms (41 -> 42) ; combat ~20-30 images/s selon la charge. Dernière
  build mesurée : dist/torchlight-essai-natives-logique.nro (8ced7bbf, ELF
  switch/elf/torchlight-essai-natives-logique-20261027-2030.elf ; profil pilas-archive/rex_pilas-20261027-2102.txt).
  Code généré modifié (non versionné, garder tel quel : la build PGO s'appuie dessus) : appels figés remis en sub_X
  pour sub_821A1510 (~90 fichiers), sub_822F92A0 (6, 64), createController sub_824B4C78 (9 fichiers), ~Controller
  sub_824B5A10 (216). Écarté : poses des squelettes en parallèle (torchlight-rekindled ; la logique de Runic pose
  chaque unité pendant sa mise à jour, le report serait risqué) ; dessin trop plat pour une réécriture rentable
  (notre relevé natif::Render ~4,8 % en combat : cache des objets d'Ogre par dessin possible, ~1 %).
  Recouvrir logique et rendu (~30 %) : hors de portée (scène partagée).
- Point de reprise précédent (2026-10-21, fin de session). Version 1.0.3 refaite, NON PUBLIÉE : dist/torchlight-nx-1.0.3.zip
  (MD5 febc143e, .nro MD5 7e0d5f10, ELF switch/elf/torchlight-joueurs-1.0.3-20261021-0039.elf ; profil PGO de pgo/
  du 2026-10-20, session 3 : UpdateControllers, ControllerDestination et Outside compilées sans profil, modifiées
  depuis), texte dist/RELEASE-1.0.3.md (sans chiffres). Premier zip 1.0.3 (sans les changements du 2026-10-21)
  gardé : dist/torchlight-nx-1.0.3-premier.zip (eec6c786). Contenu depuis la 1.0.2 : blocs de géométrie statique
  hors de l'écran retirés (src/torchlight_blocs.cpp ; console : mine ~+30 % d'images/s, ville ~+5 % ; boîtes
  transformées gardées depuis le 2026-10-21), liaisons de tampons sautées, écouteur du RT Shader System et
  getKeyFramesAtTime en natif (d'après torchlight-recomp de YaPeL, crédité dans README et THIRD_PARTY_NOTICES) ;
  préchargement des contrôleurs et systèmes de particules arrêtés en natif (2026-10-21, à nous).
  ATTENTION : la build des joueurs refaite (7e0d5f10) est plus lente dans la mine que le premier zip (19,0-19,2 ms
  contre 17,6-17,7, même endroit ; ville sans gain ; journal, 2026-10-21). Prochaine étape (décidée) : build
  instrumentée PGO (OUT=switch/out-pgo, -DTORCHLIGHT_PGO=generer -DTORCHLIGHT_LTO=OFF), session de jeu,
  console.sh pgo, build des joueurs, nouvelle mesure ville + mine immobile ; si la mine reste lente : A/B par options
  (préchargement des contrôleurs, systèmes arrêtés) dans un même .nro. Ne rien publier avant ; repli possible :
  dist/torchlight-nx-1.0.3-premier.zip. La console XJE1 a la build refaite (7e0d5f10). torchlight-nx : commit local
  5c064de (synchronisé avec la build refaite, à refaire si le code change), push à la demande ; release v1.0.3 à
  créer par l'utilisateur. Zips publiés gardés : dist/torchlight-nx-1.0.1-publie.zip (ff78834e),
  dist/torchlight-nx-1.0.2-publie.zip (f6f94267). Profil de la console de la 1.0.3 (2026-10-20) :
  pilas-archive/rex_pilas-20261020-2334.txt ; piste suivante : particules de ParticleUniverse (sub_821C8618 et
  dessous, ~10 % du thread dans les donjons, surtout des attentes de la mémoire).
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
  reste au choix du joueur) et sans rien changer au rendu (pas d'ombres une image sur deux, pas de mise à jour
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
  switch/CMakeLists.txt (1.0.3).
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
Fait aussi (2026-10-14) : appels indirects promus (-13/-15 %). Pistes restantes (ville, console, test n°23) : textures de rendu 207 (ombres, 5 dessins) et 208 (lumière, 40
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

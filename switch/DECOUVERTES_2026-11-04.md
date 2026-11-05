# Découvertes de performance du 2026-11-04 : port Switch recompilé (ReXGlue)

Document de transmission à une autre session, sur un autre projet : il se lit sans connaître celui-ci. Le journal complet, essai par essai, est dans `switch/RENDU_NATIF.md` (fin du fichier).

**Contexte.** Torchlight (Xbox 360, moteur Ogre3D, interface CEGUI) recompilé par ReXGlue pour la Switch, avec le SDK modifié du port Need for Speed MW (nfsmw-nx), un rendu natif Vulkan par NVK et la libnx. Mesures aux fréquences de base, 1020 MHz pour le processeur, sauf mention contraire. Le jeu est limité par un seul thread, le thread principal.

**Statuts.** Console = mesuré sur la Switch. PC = validé sur PC seulement. Estimé = calculé, pas mesuré. Les gains sont d'une session à l'autre, sauf mention « A/B » (alternance dans la même session).

## Résumé

| Découverte | Effet | Statut | Portée |
|---|---|---|---|
| Fenêtre mémoire du jeu jamais alignée sur 2 Mo | Combat -15 à -17 %, ville -7,9 %, temps d'image de la mine -20 % | Console | Tout port issu de ce SDK |
| Thread de rendu coupé : le thread principal accélère | -14 à -18 % (plafond, A/B) | Console | Méthode réutilisable |
| 98-99 % de la géométrie renvoyée pour rien | Ville -7,4 %, mine -7,0 %, combat -4,7 à -6,3 % | Console, A/B | Rendu natif |
| Verrou global des sections `mfmsr` / `mtmsrd` inutile | 2,2 % du thread principal, 6,4 % pendant les chargements | Relevé de piles ; correctif en place, gain non isolé | Tout code ReXGlue |
| Code du .nro en grandes pages | Combat -2 à -4 %, ville -2,3 % | Console | Tout .nro homebrew de grande taille |
| « 0 coverage mismatch » ne prouve rien pour le code généré | Contrôle faux | Vérifié | Tout build GCC + PGO de ce SDK |
| Natives sans profil PGO | Aucun gain, puis ville -9 %, mine -6 % avec leur profil | Console | Toute réécriture native |
| Interface CEGUI reconstruite à chaque image sans raison | Coût du parcours : 5,9 % en combat, 18,4 % au menu. Évité sur 27 % des images d'une session, peu en combat ; gain pas chiffré | Console : 0 différence sur 14 400 images | Jeux CEGUI |
| Attente active à 60 images/s | Thread 99,5 -> 80 %, aucune image gagnée | Console | Jeux avec attente de fence |

Résultat cumulé, sur la console, sans relevé actif : ville 60 images/s (image 12,35 ms) et mine 60 images/s (image 9,71 ms), contre 16,53 et 13,51 ms avec le recompilé du profil 1.0.4 (-25 % et -28 %). Ces chiffres datent d'avant le GPU à 460,8 MHz. En combat lourd, le jeu reste sous 60 images/s (24 à 29 ms entre images, relevé actif) : c'est le thread principal qui limite.

## 1. Applicable à tout port issu de ce SDK

### 1.1 La fenêtre du jeu n'a jamais été en grandes pages (à vérifier en premier)

`virtmemFindAslr(size, guard_size)` : **le second paramètre est une garde, pas un alignement.** `RexGmInit` lui passait 2 Mo en croyant aligner. La fenêtre n'était donc jamais alignée, et le noyau n'y employait jamais de blocs, malgré les morceaux de 2 Mo du SDK. Toute la mémoire du jeu était en pages de 4 Ko, dans toutes les versions publiées.

- **Correctif :** demander 2 Mo de plus et arrondir la base (`guest_memory_switch.cpp`, voir `switch/patches/nfsmw-sdk.patch`). Le code généré ne change pas, donc le profil PGO reste valable.
- **Le même bug existe dans le SDK de nfsmw-nx** (`RexGmInit`).
- **Effet :** coût d'un défaut de TLB mesuré à +77 ns, soit autant qu'un accès à la RAM. Principales zones du jeu de +140/+165 ns à ~+8 ns. Ville 19,21 -> 17,70 ms, combat -15 à -17 % à dessins égaux.
- **Comment le mesurer :** parcourir 2048 lignes en ordre *aléatoire*, dispersées puis groupées, et comparer. Un motif régulier mesure le cache L2, pas le TLB : ma première version était fausse à cause de ça. `svcQueryPhysicalAddress` refuse le code et les vues du jeu, il ne sert donc pas de preuve.

### 1.2 Le code du .nro est aussi en pages de 4 Ko

Le chargeur le place à une adresse quelconque. Reprojection à la même adresse virtuelle, tôt au démarrage, depuis une copie de même reste sur 2 Mo (`src/torchlight_code_blocs.cpp`). Exécution +115 -> +7 ns, lecture +81 -> +5 ns. Gain modeste : combat -2 à -4 %.

Coût : 83 Mo perdus, et plus de retour possible à hbloader. Ici, le jeu ne revient jamais à hbloader, donc rien n'est à rétablir. À revérifier si votre port y revient.

### 1.3 Les vues décalées de 4 Ko restent en petites pages

La vue 0xE0000000 vise 0x100001000. En décalant un morceau, on la passe de +132 à +12 ns, mais les vues 0xA0000000, 0xC0000000 et 0x7F000000 perdent leurs blocs. Gain de 0 à -2 %, dans la marge. Gardé parce que sans régression : ce n'est pas un chantier prioritaire.

### 1.4 Le verrou global de `mfmsr` / `mtmsrd` ne protège rien

Le générateur traduit ces instructions par un `std::recursive_mutex`, avec barrières et compteurs. Les 185 sections de ce jeu ne contiennent que `lwarx`/`ldarx`, `stwcx.`/`stdcx.` et des opérations de registres, et `stwcx.` est déjà un vrai compare-and-swap. `switch/tools/atomiques_sans_verrou.py` vide ces lignes sur place, en refusant toute section qui contiendrait autre chose. À revérifier pour chaque jeu : l'outil le fait à sa place.

### 1.5 Le thread principal attend la mémoire, et les autres threads le gênent

- **Par instruction** (`switch/tools/profil_instructions.py`) : 66,5 % des échantillons sur une lecture entière, 4 % sur une lecture flottante, 10,6 % sur une écriture entière. C'est une attribution d'échantillonnage, pas une mesure de cycles.
- **Test à faire sur votre port :** alterner dans la session un état où le thread de rendu ne fait rien (écran figé, images jetées). Ici, temps de l'image -14 à -18 %, alors que le thread principal n'attend jamais le rendu (0,02 ms). Ce plafond compte tout le thread de rendu et tout le GPU, sans en séparer les parts (cache L2 partagé de 2 Mo, trafic du GPU).

### 1.6 Géométrie : presque rien ne change d'une image à l'autre

En combat, 70 Ko copiés et 9913 Ko repris par image : le thread de rendu reconvertissait et repoussait 2 à 5 fois la taille du L2 à chaque image. Tampons persistants, 3 arènes de 32 Mo, clé sur la plage et un numéro de version (`src/natif/rendu.cpp`). Validation : 14 670 plages reconverties et comparées, 0 différente.

### 1.7 Écritures sur la carte SD depuis les threads critiques

- Un `fflush` du journal sur le thread principal : blocage de 94 ms, et 1,1 s sur un bloc de chargement de 2,3 s. Bilans périodiques coupés par défaut.
- Un avertissement de Mesa répété à chaque image vide le journal depuis le thread de rendu (`flush_level = warn`). Lien probable, non prouvé, avec des séries de 2-3 images de 110-150 ms toutes les 5 s environ. Limité à une ligne sur 1000. Disparition des séries pas encore vérifiée.
- Enregistrement du cache de pipelines sur le thread de rendu : 134 et 172 ms. Déplacé sur le thread de compilation.

### 1.8 Mode boost et GPU

- **Mode boost (`appletSetCpuBoostMode`) :** il bride le GPU à 76,8 MHz. Avec un seuil trop bas (250 ms), une image de 270 ms en combat passait pour un chargement : ~18 images/s. Seuil relevé à 400 ms, et pas de boost en plein jeu.
- **GPU à 460,8 MHz en portable :** profil officiel par `apmSetPerformanceConfiguration`, pas un surcadençage. Le SDK le sait déjà faire (`switch_apm.cpp` : mémoire inchangée, sinon GPU laissé tel quel) mais rien ne l'appelait ici. À demander **avant** tout boost : pendant un boost, le SDK lit 76,8 MHz et refuse le profil. Confirmé sur la console : retour à 460,8 MHz après les boosts (veille non vérifiée). Non mesurés : batterie, chauffe, gain chiffré.
- **Piège des overlays :** sur la console de test, le boost affiche 2397 MHz de processeur au lieu de 1785, et la mémoire reste à 1600 MHz là où le profil annonce 1331 (Horizon-OC ou sys-clk, non vérifié). Les temps de chargement et l'effet de la mémoire n'y valent donc pas pour une console d'origine.

### 1.9 Attente active au plafond

La fence du Direct3D du jeu tourne dans une boucle dont le `db16cyc` est recompilé en rien : 17,85 % du thread dans la mine. Une pause de 100 µs par tour fait passer le thread de 99,5 à 80 % à 60 images/s. Aucune image gagnée, moins de charge. **À l'arrêt, un cœur à 100 % sur l'overlay ne veut donc pas dire saturé.**

### 1.10 PGO et natives : ce qui trompe

- **« 0 coverage mismatch » ne prouve rien pour le code généré** : `DEFINE_REX_FUNC` vient d'un en-tête système, et GCC y masque les avertissements. `switch/tools/audit_profil.py` (avec `-Wsystem-headers`) compte les vraies pertes.
- **`-Wno-missing-profile` fait taire un .gcda absent.** Comparer les noms des objets et de `pgo/`.
- **« source locations ... have changed » garde le profil.** Seul « number of counters » le perd. Insérer une ligne en haut d'un fichier ne coûte donc rien.
- **Une native sans profil ne gagne rien :** aucun gain mesuré sans profil, puis ville -9,0 % et mine -6,1 % avec le leur. Un A/B natif contre recompilé avec un profil de l'alternance est biaisé : ce profil ralentit le recompilé de ~3 % à l'arrêt.
- **Ninja ne suit pas les .gcda** : supprimer les objets plus anciens que le profil avant de recompiler.
- **Ajouter une native :** les appels déjà figés en `__imp__sub_X` dans le code généré l'ignorent (`switch/tools/appels_figes.py`).

## 2. Propre au moteur

### 2.1 CEGUI : qui lève le drapeau de redessin ?

Version de CEGUI non vérifiée, mais le code correspond à ce qui suit. `System::renderGUI` ne reconstruit la liste de quadrilatères que si `d_gui_redraw` est levé. Sur ce jeu, il l'était à chaque image. Une pose matérielle de point d'observation sur ce booléen a donné la réponse en quelques minutes : 1502 images sur 1503, un seul chemin (`Window::onMoved`). Ce sont 14 petits boutons animés de pages de menus chargées mais pas sous la feuille active : ils oscillent en permanence sans jamais rien dessiner.

**Sur la console, ça n'a pas suffi.** Après avoir filtré ces fenêtres : encore 85 % d'images avec une demande, jusqu'à 96 % en fin de session. 107 fonctions lèvent le drapeau (sans doute `requestRedraw` en ligne). D'où une règle plus générale, quel que soit le demandeur : on ne reconstruit que si une fenêtre *dessinée* a son besoin de redessin levé, a bougé, ou si la liste des fenêtres dessinées a changé (`--torchlight_interface=3`, fin de `src/torchlight_ogre_natif.cpp`).

- **Validation (mode 2) :** liste refaite à chaque image et comparée à l'octet. PC, 7200 images en ville et menus : 2600 demandes, 876 utiles, 0 différence.
- **En service (mode 3), sur PC, mêmes scènes :** 7200 images, 1793 demandes, 711 reconstructions (90 % d'images sans reconstruction), sans plantage ; pages identiques sur les captures, hors scène 3D animée et petits éléments d'interface animés. Rappel : cette scène ne vaut pas un vrai combat (voir section 3).
- **Console, vrai combat (mode 2, 22:29-22:35, sans plantage) : 14 400 images, 12 076 demandes, 10 486 utiles, 0 différence.** La règle est juste, et le mode 3 est maintenant le défaut. Son gain en temps n'est pas encore chiffré.
- **Gain limité en combat :** 31 % d'images à reconstruire sur la première tranche de 3600 images, puis 80 %, 80 % et 100 %. Les fenêtres responsables changent vraiment : barre de mana et de vie, chiffres de dégâts (« Coup critique ! », 202...), qui sont des fenêtres de CEGUI. Le filtre ne peut rien pour elles : aller plus loin demanderait de rejouer les quadrilatères des fenêtres inchangées pendant la reconstruction (estimé : 2 à 3 % en combat, davantage au menu), chantier non commencé.
- **Limite connue :** au menu, des boutons animés sont réellement à l'écran, donc la reconstruction reste due. Une seule fonction, probablement le dessin du texte, y pèse 10,4 % sur 14,2 %.
- **Faux positif à connaître :** les 3 octets de remplissage après un booléen du quadrilatère ne sont jamais initialisés. Une comparaison à l'octet qui ne les masque pas signale des centaines de différences qui n'en sont pas.

### 2.2 Temporaires de chaînes et décompression (Ogre et jeux de Runic)

- Une `std::wstring` construite, donc allouée et libérée, à chaque recherche de propriété pour chaque unité. Clé posée sur la pile, qui montre le texte de l'image (PC, gain non mesuré).
- `zzip_file_read` : un squelette se lit par 4, 16 puis 12 octets par image clé, donc `inflate` passe par son chemin lent, octet par octet. Lecture par blocs de 64 Ko, plus zlib de la console. Première rencontre de monstres 286 -> 217 ms.
- `RibbonTrail::updateTrail` : un élément par longueur parcourue alors que l'anneau n'en garde que `total`. Saut ajouté, 181 ms évités sur un cas relevé.

### 2.3 Pas encore résolu

Pics de ~55-60 ms, en une image, quand le familier du joueur invoque trois archers squelettes avec un effet de particules. Aucun fichier de modèle n'est relu, contrairement à ma première hypothèse : les « ouvertures » sont des tests de dossiers à ~50 µs. Le nombre de dessins n'explique pas le pic.

Déroulé d'une image de 60 ms, avec un seuil de piles abaissé à 5 ms (`--torchlight_images_longues_ms`, 5 images, 119 piles) : **création des trois archers de 0 à ~25 ms, environ 9 ms chacun**, puis la logique normale, puis **~12 ms sous un écouteur de la scène (`sub_8219C328` -> `sub_823F5F60`, non identifié, 0,1 % d'une image normale)**, enfin le rendu normal. La création est un profil plat, sans poste dominant. Pas de correctif simple : la seule façon nette de lisser serait d'étaler les créations sur plusieurs images, ce qui toucherait à la logique du jeu. Laissé tel quel à ce stade.

## 3. Méthode réutilisable

1. **Mesurer le coût réel avant d'optimiser.** Module de mesure du TLB, instruction par instruction, thread coupé. La piste du TLB, jamais mesurée avant ce jour, a donné le plus gros gain, sur un bug présent depuis la première version.
2. **Une alternance dans la session (A/B par fenêtres de 2 s)** bat les comparaisons entre sessions, surtout quand le plafond de 60 images/s masque l'écart : regarder alors le temps de l'image, ou comparer par régression à dessins égaux.
3. **Chien de garde des images longues.** Dès qu'une image dépasse un seuil, un thread lit la pile du thread principal toutes les 2 ms, et note les fichiers ouverts. Il coûte peu (pause médiane 8 µs). Il a nommé la plupart des blocages que le profileur normal ne montrait pas : celui-ci ralentit d'environ 4 fois pendant ces images-là. Limite : avec un seuil à 40 ms, on ne voit rien des 40 premières millisecondes.
4. **Point d'observation matériel sous gdb** sur le drapeau ou la variable suspecte : la question « qui écrit ça à chaque image ? » se règle tout de suite.
5. **Mode de validation à l'octet** pour tout saut d'un calcul : faire les deux, comparer, compter les écarts. Il a trouvé un vrai bug (adresse d'une table fausse d'un chiffre) sur les primitives du chargement.
6. **Pièges rencontrés aujourd'hui :**
   - Une boucle d'attente avec `pgrep -f <motif>` se détecte elle-même, et un `pkill -f` tue le shell qui le lance.
   - `ptrace_scope = 1` interdit d'attacher gdb à un processus non descendant : lancer le jeu par un petit programme qui appelle `prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY)` puis `exec`.
   - Le jeu lancé sous gdb démarre plus lentement : une manette de script à horaires fixes se décale et ouvre le mauvais menu. Le script dépend aussi de la machine.
   - Dans un relevé de piles, ajouter des catégories incluses compte les choses deux fois (ici 12,4 % au lieu de 7,8 %). Vérifier les inclusions avant d'annoncer un gain.
   - **Un chiffre mesuré dans une scène sur PC ne vaut pas en vrai combat sur la console :** 74 % d'images sans reconstruction annoncés sur PC, 15 % sur la console.
   - Avec le relevé des piles actif, les bilans de `rex_perfil.log` s'étirent (~15-18 s pour un « 10 s » affiché) : leurs images/s et leurs % de CPU sont faux d'autant.

## 4. Essayé sans gain, ou écarté

ZCULL (aucun gain pour le thread principal), mémoire du jeu sans `volatile` (pas de gain, risque d'exactitude), sections critiques en ligne (0,28 %), changements de mode flottant (0,30 %), `dcbt`, poses du rendu (p99 pire en combat), logique et rendu en parallèle (scène partagée).

## 5. État au moment d'écrire

- **Commits sur `main`, rien de publié :** `3dd44b8`, `117f5cf`, `4dd755d`, `15b0d05`. Code figé en attendant une session PGO.
- **Validés sur la console :** sections 1.1 à 1.9, sauf les réserves indiquées.
- **Validé sur la console, gain en temps non mesuré :** règle d'interface (mode 3, défaut).
- **Validés sur PC, gain non mesuré sur la console :** 13 natives, verrous à courte attente des threads d'aide (plafond estimé 1 à 2 % en combat). Une nouvelle session PGO est nécessaire pour que les natives gagnent quelque chose.
- **Outils :** `switch/tools/` (`atomiques_sans_verrou.py`, `audit_profil.py`, `profil_instructions.py`, `verif_zip.py`, `ab_absolu.py`, `ab_regression.py`). Module TLB : `src/torchlight_tlb.cpp`.

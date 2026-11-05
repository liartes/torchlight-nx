# Moteur de rendu natif pour la version Switch

Décidé le 2026-09-30 après trois essais sur console : l'émulation du GPU Xenos sature le Tegra X1
(menu ~3 images/s, ville 1,2 image/s, puis gel complet de la console). Même constat que nfsmw-nx,
qui a remplacé l'émulation par un rendu Vulkan propre au jeu (`~/workspace/nfsmw-nx/docs/native-renderer.md`).

## Ce qu'on sait de Torchlight (et qui change l'approche par rapport à NFS)

- **Le jeu est bâti sur Ogre3D** (probablement 1.7) : `pak.zip` contient des `.material`, `.mesh`,
  `.skeleton` ; le rendu passe par le `D3D9RenderSystem` d'Ogre adapté à la Xbox 360
  (classes `D3D9RenderSystem`, `D3D9Device`, `D3D9Texture`, `D3D9RenderTexture`...). Le code
  source d'Ogre est public (licence MIT).
- **L'exécutable contient les RTTI MSVC** (`.?AV...@Ogre@@`) : chaque classe est liée à sa table de
  fonctions virtuelles, donc on peut nommer les méthodes Ogre dans le code recompilé en suivant
  l'ordre des virtuelles des en-têtes d'Ogre. Les messages d'exception d'Ogre donnent aussi des noms
  de fonctions (`RenderSystem::_setVertexTexture`, `UnifiedHighLevelGpuProgram::loadFromSource`...).
- **Une partie des shaders est générée à l'exécution** par le RT Shader System d'Ogre, à partir des
  sources HLSL de `assets/rtshaderlib/`, puis compilée par le compilateur HLSL du SDK Xbox (recompilé
  avec le jeu). Le reste vient des matériaux et de `programs/shaders.fxobj` (compressé).
  Conséquence possible : récupérer le HLSL source et le compiler directement en SPIR-V avec DXC, au
  lieu de traduire du microcode Xenos avec XenosRecomp.
- Mesures de l'émulation (rex_perfil.log) : menu ~130 dessins + ~170 transferts de render targets
  par image ; ville ~234 dessins, 250 transferts, 103 pipelines nouveaux par image.

## Deux niveaux d'interception possibles

1. **Commandes GPU (anneau PM4)**, comme nfsmw-nx : on laisse tourner le Direct3D du jeu et on lit ce
   qu'il écrit. Avantage : ~28 000 lignes réutilisables (`nfsmw-nx/app/src/nfsmw_nativo_*`, GPL-3).
   Inconvénients : shaders à traduire depuis le microcode ; le coût CPU du Direct3D du jeu reste.
2. **Le RenderSystem d'Ogre** : remplacer les méthodes virtuelles du `D3D9RenderSystem` (`_render`,
   `_setTexture`, `bindGpuProgram`, création des textures et tampons) par une implémentation Vulkan.
   Avantages : interface documentée (en-têtes Ogre), shaders disponibles en HLSL, pas de Direct3D
   du jeu ni d'EDRAM. Inconvénients : tout à écrire ; les textures et tampons restent décrits au
   format Xbox (tiling, boutisme) dans la mémoire du jeu.

Le choix se fera après la phase 1, sur des données.

## Phases

0. **Développer sur PC.** Compiler Torchlight PC (Linux) avec le SDK de nfsmw-nx (celui de la Switch),
   pour écrire et tester le rendu natif avec Vulkan sur le PC, et ne passer sur la console qu'aux
   jalons (pas de carte SD à chaque essai, pas de gel de la console).
1. **Cartographie, sans rien changer au rendu.**
   - RTTI -> tables virtuelles des classes Ogre de rendu ; nommer les méthodes du `D3D9RenderSystem`.
   - Fonctions Direct3D du SDK Xbox utilisées (création de shaders et textures, Draw*, Resolve, Swap).
   - Journal des appels (comme `nfsmw_d3d_trace.cpp`) : confirmer chaque adresse avant de remplacer quoi que ce soit.
   - Récupérer les sources HLSL générées par le RT Shader System et le contenu de `shaders.fxobj`.
2. **Choix du niveau d'interception** (PM4 ou Ogre) avec ces données.
3. **Shaders** : HLSL -> SPIR-V (DXC) ou microcode -> HLSL -> SPIR-V (XenosRecomp), en bibliothèque préparée à l'avance.
4. **Premières images en rendu natif sur PC**, puis textures, render targets, présentation.
5. **Switch** : mesures sur console (voir `nfsmw-nx/docs/measuring.md`), puis optimisations.

## Journal

- 2026-09-30 : plan, découverte d'Ogre3D et des RTTI.
- 2026-09-30 : phase 1 commencée.
  - `tools/rtti_vtables.py` : retrouve les 2192 tables virtuelles de l'image (`/tmp/image.bin`) par les RTTI
    (vtable[-1] -> Complete Object Locator -> TypeDescriptor). Classes de rendu : `D3D9RenderSystem`
    0x82005794 (127), `D3D9HLSLProgram` 0x82004734, `D3D9Texture` 0x820049f4, `D3D9HardwareVertexBuffer`
    0x8200118c, `D3D9HardwareIndexBuffer` 0x820f6914, `D3D9RenderTexture` 0x8200394c, `D3D9RenderWindow`
    0x82003fdc, `D3D9VertexDeclaration` 0x820f6640, `D3D9GpuProgram` 0x820f5b14.
  - `tools/vtable_slots.py` : premières instructions de chaque case (repérer accesseurs et motifs).
  - `switch/ogre/D3D9RenderSystem.txt` : table nommée, alignée sur Ogre 1.7.0 (`~/workspace/ogre-1.7`) ;
    le jeu a une méthode de plus en case 89. `_render` = case 87 (sub_821C4058), `bindGpuProgram` = 90,
    `bindGpuProgramParameters` = 91, `_setTexture` = 38, `_setRenderTarget` = 114, `clearFrameBuffer` = 107,
    `_beginFrame`/`_endFrame` = 55/58.
  - Phase 0 faite : Torchlight PC compilé avec le SDK nfsmw-nx (`switch/out-pc`), démarre et atteint le menu
    (émulation Xenos, 45 s sans FATAL). Commandes :
    `cmake -S switch -B switch/out-pc -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 -DGIT_EXECUTABLE=/bin/false -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2 -DREXSDK_DIR=$HOME/workspace/nfsmw-nx/sdk`
    puis `cmake --build switch/out-pc -j 6` ; lancement :
    `LD_LIBRARY_PATH=$HOME/workspace/nfsmw-nx/sdk/out/linux-amd64 ./switch/out-pc/torchlight --game_data_root=$PWD/assets --gpu_plugin=xenos --license_mask=1`.
    Il a fallu ajouter les chemins d'imgui/renderdoc à la cible (switch/CMakeLists.txt), comme nfsmw-nx.
- 2026-09-30 : traceur `src/torchlight_trace.cpp` (option `torchlight_trace`, script `switch/trace_pc.sh`).
  - Adresses confirmées sur le jeu en marche : `this` = D3D9RenderSystem (0x404740A0), `_setRenderTarget` reçoit
    le `D3D9RenderWindow`, `setVertexDeclaration` un `D3D9VertexDeclaration`, `_render` une `RenderOperation`
    (type 4 = OT_TRIANGLE_LIST), `_setSceneBlending(7, 9)` = SOURCE_ALPHA / ONE_MINUS_SOURCE_ALPHA.
  - `D3DXCompileShader` = sub_82777F10 (appelé par `D3D9HLSLProgram::loadFromSource` = sub_82587FB0) :
    r3 = source, r4 = longueur, r7 = entrée, r8 = profil (`vs_3_0`, `ps_3_x` propre à la Xbox).
  - Menu sur PC : 29 shaders compilés, tous générés par le RT Shader System d'Ogre (HLSL D3D9 avec
    `#include "FFPLib_*.hlsl"` de assets/rtshaderlib) ; ~17 `_render`, 21 `bindGpuProgram` et 1 cible de rendu
    par image. L'émulation voyait ~130 dessins + ~170 transferts par image sur Switch : le coût vient de l'émulation.
  - `tools/str_refs.py` : fonctions qui référencent une chaîne (retrouver les fonctions Ogre par leurs messages).
- 2026-09-30 : trace d'une vraie partie (ville puis donjon, `switch/out-pc/trace/`, gardée hors git).
  - 208 shaders compilés, **tous** générés par le RT Shader System d'Ogre (aucune autre origine).
  - En jeu, par image : ~54 `_render`, ~74 `bindGpuProgram`, ~95 `bindGpuProgramParameters`, ~44
    `_setSceneBlending`/`_setCullingMode`, **une seule** cible de rendu, un seul viewport et un seul
    `clearFrameBuffer` (pas de rendu vers texture vu à ce niveau : ni ombres ni post-traitement).
    127 programmes GPU distincts ; 108-146 images/s sur le PC avec l'émulation.
  - Piège : MSVC range les surcharges virtuelles dans l'ordre INVERSE : `_setTexture(TexturePtr)` = case 39
    (sub_821C8E40), case 38 = surcharge String. `SharedPtr` d'Ogre a une vtable : pointeur à +4.
    Après correction : 52 textures distinctes et ~33 `_setTexture` par image au menu.
  - **Les 208 shaders se compilent en SPIR-V** avec glslang (HLSL, `-D -V`), après `switch/shaders/prep_hlsl.py` :
    fins de ligne LF + retour final (sinon le `#include` suivant se colle) et `FFP_SampleTexture` renommée par
    type de sampler (glslang ne distingue pas sampler1D/2D/3D/CUBE en surcharge). glslangValidator compilé depuis
    `nfsmw-nx/sdk/thirdparty/glslang` (cible `glslangValidator`, -DENABLE_OPT=OFF).
  - Recommandation pour la phase 2 : **intercepter au niveau du RenderSystem d'Ogre**. Le rendu du jeu est
    simple (une passe, ~54 dessins, pas de RTT), les shaders arrivent en HLSL compilable directement en SPIR-V,
    et on évite à la fois le Direct3D Xbox, l'EDRAM et la traduction de microcode.
    Reste à cartographier : `D3D9Texture` (données et format des textures en mémoire invitée),
    `D3D9HardwareVertexBuffer`/`IndexBuffer` (verrouillage et contenu), `D3D9VertexDeclaration`, et
    `bindGpuProgramParameters` (valeurs des constantes).
- 2026-09-30 : cartographie des textures, tampons et programmes (vidages `trace/objets.txt`, menu PC).
  - **Texture** : `D3D9Texture` (vtable 0x820049F4) : nom (std::string) en +0x2C, largeur/hauteur/profondeur en
    +0xBC/+0xC0/+0xC4 ; base `D3D9Resource` en +0x120 ; `mMapDeviceToTextureResources` (std::map MSVC) en-tête
    en +0x128, taille en +0x12C. Nœud racine = `_Parent` de l'en-tête (+4) ; pair<device, TextureResources*>
    en +0x0C/+0x10 (device = 0x4000A480). `TextureResources` = { pNormTex, pCubeTex, pVolumeTex, pBaseTex, pFSAASurface }.
    Objet Direct3D Xbox : en-tête (Common 0x00100003, refcount...), **fetch constant GPU en +0x1C (6 mots)**.
    Exemple décodé : pitch 1024, non tuilé, format 0x13 (DXT2/3), boutisme 8-in-16, adresse physique
    0x1D75F000, 1024x1024 (mot 2 = largeur-1 / hauteur-1). Décodable avec le code de textures Xenos du SDK.
  - **Tampon de sommets** : `VertexBufferBinding` (0x8200139C) : std::map<ushort, SharedPtr> en-tête en +8,
    valeur SharedPtr en +0x10 du nœud (pRep à +0x14) -> `D3D9HardwareVertexBuffer` (0x8200118C) : taille en +4,
    nombre de sommets en +0x20, taille d'un sommet en +0x24 ; base D3D9Resource en +0x30, map par device en +0x38
    -> `BufferResources` { IDirect3DVertexBuffer9*, ... } -> objet Xbox (Common 0x00100001) avec la **fetch constant
    de sommets en +0x18** (2 mots) : type 3 + adresse physique (mot 0), boutisme + taille en mots (mot 1).
  - **RenderOperation** { VertexData* +0, OperationType +4, bool useIndexes +8, IndexData* +0xC, Renderable* +0x10 }.
    **VertexData** { mMgr +0 (champ privé en tête !), vertexDeclaration +4, vertexBufferBinding +8, bool +0xC,
    vertexStart +0x10, vertexCount +0x14 }.
  - **VertexDeclaration** : les éléments sont dans un **std::vector** (+4 début, +8 fin, +0xC capacité), et non une
    std::list comme dans Ogre 1.7.0 ; élément de 0x14 octets { source, offset, type, semantic, index }.
  - **Programmes** : l'objet `D3D9HLSLProgram` (this de loadFromSource) pointe en +0x188 vers le programme
    assembleur que reçoit `bindGpuProgram` (vérifié sur 27/27 programmes) -> relie chaque programme lié à sa
    source HLSL, donc à son SPIR-V.
  - **GpuProgramParameters** : `mFloatConstants` = std::vector<float> en +0 ; constantes entières en +0x10 ;
    SharedPtr des tables logique->physique et des noms vers +0x20..+0x48 (à détailler). Plan : relier par nom
    les constantes d'Ogre aux membres du bloc uniforme SPIR-V.
  - Pièges : ne jamais lire la mémoire invitée au-delà d'un objet connu (lire 0x400 octets après un objet
    HLSL a bloqué le thread de rendu) ; /dev/shm plein de xenia_memory_* => « Erreur du bus » au lancement.
  - Tampons d'indices : voir l'entrée suivante.
- 2026-09-30 : fin de la phase 1 (tampons d'indices), essais automatiques.
  - **Manette virtuelle** `src/torchlight_manette.cpp` (d'après nfsmw-nx) : `--torchlight_manette="20:a,24:a,..."`.
    Des appuis sur A toutes les 4 s de 20 à 80 s amènent le jeu dans TOWN1 sans intervention. Toujours lancer ces
    essais sur une **copie** des données utilisateur (`--user_data_root=<copie de ~/.local/share/torchlight>`).
    Les outils de développement ne sont compilés que par switch/CMakeLists.txt (macro TORCHLIGHT_OUTILS).
  - **Lectures sûres** dans le traceur : `BaseHeap::QueryRangeAccess` (via `rex::system::kernel_memory()->LookupHeap`)
    avant toute lecture au-delà des arguments ; sans cela, le thread de rendu se bloquait selon la disposition du tas.
  - **Les RenderOperation des objets 3D sont sur la pile** (0x7xxxxxxx), celles de l'interface dans le tas.
  - **IndexData** { HardwareIndexBufferSharedPtr (0x10 octets : vtable, pRep, pUseCount, ...), indexStart +0x10,
    indexCount +0x14 }. **D3D9HardwareIndexBuffer** (0x820F6914) : taille +4, mIndexType +0x20 (0 = 16 bits),
    mNumIndexes +0x24, mIndexSize +0x28 ; map par device en +0x34 -> BufferResources -> objet Xbox
    (Common 0x20100002, type 2) : **adresse en +0x18** (0xFxxxxxxx, physique = & 0x1FFFFFFF), **taille en +0x1C**.
    Indices 16 bits big-endian.
  - En jeu (TOWN1, PC) : ~35-54 `_render` par image, la plupart indexés.

Phase 1 terminée : on sait lire, pour chaque dessin, programmes (-> HLSL -> SPIR-V), constantes, textures
(fetch constant), sommets (adresse, taille, déclaration) et indices. Phase 2 : premier rendu natif sur PC.
- 2026-09-30 : phase 2, étape A : **capture d'une image** (`src/torchlight_capture.cpp`, `--torchlight_capture_s=T`).
  Méthode de la phase 2 : (A) capturer sur disque tout ce qu'il faut pour redessiner une image, (B) un outil
  Vulkan hors ligne la redessine en PNG ; on ne branche le rendu dans le jeu qu'une fois l'image juste.
  - Format : `trace/capture/frame.txt` (un bloc par `_render`, décrit en tête du fichier) + vb_/ib_/tex_/cst_.
  - **Adresses GPU du Direct3D du jeu** : celles de la vue 0xE0000000 gardent leurs bits hauts ; physique =
    adresse - 0xE0000000 + 0x1000 (`PhysicalHeap::GetPhysicalAddress`). Sans ce décalage, données décalées de 4 Ko.
  - `TextureInfo` du SDK garde la convention xenia : width/height = dimension - 1.
  - **Validé** : `SHADOWFADE.DDS` capturée = niveau 0 du DDS de pak.zip octet pour octet (DXT3 128x128) ;
    sommets cohérents (modèle animé : position + normale unitaire, UV dans [0,1], indices d'os UBYTE4 et poids
    de somme 1). Le détilage (textures tuilées) reste à valider.
  - Table des noms des constantes : GpuNamedConstants { floatBufferSize, intBufferSize, ?, map en-tête +0xC,
    taille +0x10 } ; valeur = pair<String (0x1C), GpuConstantDefinition { type, physicalIndex, logicalIndex,
    elementSize, arraySize, variability }>.
- 2026-09-30 : phase 2, étape B : **premier rendu natif d'une image du jeu** (niveau de la mine, 104 dessins sur 110),
  `switch/images/2026-09-30_mine_rendu_natif.png` (textures rendues par le jeu remplacées par du blanc).
  - Outil `switch/tools/rendu_hors_ligne` (Vulkan sans fenêtre, PNG via zlib) ; bibliothèque de shaders
    `switch/shaders/build_lib.sh` (sources HLSL réunies dans `switch/out-pc/hlsl`, 215 shaders).
    Outils à compiler depuis `nfsmw-nx/sdk/thirdparty` : glslangValidator (cible glslangValidator),
    spirv-opt, spirv-val, spirv-dis (SPIRV-Tools, -DSPIRV-Headers_SOURCE_DIR=.../spirv-headers).
  - Shaders : `--hlsl-dx9-compatible` (sampler2D D3D9 = texture + échantillonneur combinés), puis
    `spirv-opt --legalize-hlsl` (fonctions qui reçoivent un échantillonneur) et `spirv-val`. Sémantiques système
    D3D9 traduites (POSITION en sortie -> SV_Position, COLORn du shader de pixels -> SV_Target{n}), emplacements
    tirés des sémantiques. Bindings : VS set 0, PS set 1, $Global = 0, échantillonneur unité N = 16 + N.
  - **Rangement des matrices** (déduit des données) : les float4x4 arrivent transposées (registres = colonnes,
    défaut HLSL) ; les tableaux float3x4 de l'animation arrivent par lignes (3 registres) -> `row_major` sur les
    seules déclarations globales float3x4. Avec tout en row_major, le décor partait en traînées.
  - Constantes : registre r de l'élément e -> décalage de réflexion + e * pas + r * 16 (taille du registre selon
    le type GL, pour ne pas écraser les scalaires voisins).
  - Textures : écrire les blocs visibles (dimensions réelles), lire avec le pas aligné de TextureExtent.
  - Reste : (1) textures 207/208 (512x512, 8888, tuilées), projetées sur le décor (`gTexViewProjImageMatrix0`) :
    rendues par le jeu, contenu absent de la mémoire avec l'émulation ; aucun `_setRenderTarget` vers elles dans
    l'image capturée -> trouver qui les dessine. (2) 8 shaders écrivent dans une constante globale (interdit en
    Vulkan). (3) textures non 2D de l'unité 1 (cubemaps ?). (4) test alpha (`_setAlphaRejectSettings`) non rendu.
- 2026-09-30 : **image complète du jeu, avec ses textures rendues** (`switch/images/2026-09-30_mine_rendu_natif_complet.png`,
  166 dessins sur 172).
  - Une image du jeu = un appel à `_updateAllRenderTargets` (case 101, ~25/s) ; à l'intérieur, une paire
    `_beginFrame`/`_endFrame` par cible (~5, d'où ~125 `_endFrame`/s). Les « 1 `_setRenderTarget` par image »
    des premières traces étaient par cible, pas par image du jeu. Toutes les méthodes du RenderSystem sont désormais
    comptées (`TL_SLOT`, généré depuis la table).
  - Image capturée (mine) : cible `rtt/<adresse>/207` 512x512 effacée en gris 0,5 = **carte d'ombres** d'Ogre
    (ombres par texture, `ShadowTextureManager`) ; `rtt/.../208` 512x512 (contient les particules de l'écran de
    chargement : rôle à comprendre) ; puis la fenêtre `Game` 1280x720 en trois passes (couleur + profondeur, puis
    profondeur seule deux fois : objets par-dessus, interface).
  - RenderTarget : vtable +0, mName +4, largeur +0x24, hauteur +0x28. clearFrameBuffer(buffers r4, ColourValue* r5,
    profondeur f1, stencil r6).
  - Capture : du début à la fin de `_updateAllRenderTargets` ; événements `target`, `clear`, `begin_frame`,
    `end_frame` dans frame.txt. Outil : une image Vulkan par cible, passes « effacer/conserver » selon les clear,
    viewport dynamique ; une texture d'Ogre dont le nom correspond à une cible déjà rendue (« rtt/.../<nom> ») est
    remplacée par l'image rendue (échantillonneur en bord bloqué) ; les cibles intermédiaires sont écrites à côté du PNG.
  - Reste : ellipse pâle sur l'image (élément du jeu ou bord de projection ?) à comparer avec l'émulation ; rôle de 208 ;
    personnage absent (partie scriptée ?) ; 8 shaders qui écrivent une constante ; textures non 2D ; test alpha.
- 2026-09-30 : **image du jeu redessinée pratiquement à l'identique de l'émulation**, interface comprise
  (`switch/images/2026-09-30_mine_rendu_natif_complet.png` contre `..._mine_emulation.png`, 171/171 dessins).
  - Capture d'écran de l'émulation enregistrée avec chaque capture (`emulation.ppm`, via
    `Presenter::CaptureGuestOutput`, 0,5 s après) : comparaison directe.
  - **Test alpha** (`_setAlphaRejectSettings`) : ajouté en fin de chaque shader de pixels (`torchlight_alpha_test`,
    seuil / 255 et CompareFunction d'Ogre). Le masque de la minicarte en dépend (dessin sans couleur, test alpha
    GREATER 150, écriture de profondeur).
  - **Viewports d'Ogre** : mots 10 à 13 de l'objet Viewport = mActLeft/Top/Width/Height (minicarte : 1045,466 187x187).
    Effacements limités au viewport (`vkCmdClearAttachments`), comme Direct3D.
  - **Rangement des matrices par programme** : option D3DX de chaque compilation (r9 de D3DXCompileShader :
    8 = lignes, 16 = colonnes), relevée dans `hlsl/options.txt`. Règle de secours (vérifiée sur 105/105) :
    programmes animés (`world_matrix_array_3x4`) par lignes, autres par colonnes.
  - **Cubemaps** (reflets des personnages) : faces via `texture_util::GetGuestTextureLayout`
    (`array_slice_stride_bytes`) ; type d'échantillonneur noté dans les `.io` (vue cube obligatoire).
  - **Interface** : sommets CEGUI en z = -1 ; `depthClampEnable` (profondeur bloquée, sans découpage en z) la rend.
  - Reste : 8 shaders qui écrivent une constante ; texte un peu plus gras que dans l'émulation (filtrage ?) ;
    rôle de la cible 208.

Phase 2 (rendu hors ligne) atteinte. Phase 3 : brancher ce rendu dans le jeu (PC d'abord), à la place de
l'émulation GPU.
- 2026-09-30 (fin de soirée) :
  - **Texte flou = pipeline fixe** : 28 dessins par image sont faits SANS programme (`unbindGpuProgram`, case 93),
    dont toute l'interface CEGUI (nom du niveau, compteurs) ; le portage Xbox d'Ogre émule le pipeline fixe de D3D9
    (probablement avec `programs/shaders.fxobj`). La capture les marque `prog 0000... 0000...` et enregistre pour
    eux `ffp_matrix world|view|proj` (Matrix4 d'Ogre, cases 28/30/31) et `ffp_stage` (LayerBlendModeEx de
    `_setTextureBlendMode`, case 43 ; opération, sources, arguments). Unités désactivées : cases 33/34.
    Nom du niveau : sommets noirs à 70 % (ombre) puis blancs à 50 % -> gris.
  - Le décalage d'un demi-pixel D3D9 (viewport + 0,5) est appliqué ; ce n'était pas la cause du flou.
  - Détilage des textures tuilées validé (police CEGUI `_cegui_ogre_1` parfaitement lisible).
  - **Blocages au démarrage avec les outils actifs** (thread principal du jeu qui attend le GPU émulé, sub_82774170,
    le GPU ne reçoit plus rien) : sans outils 6/6 réussis ; l'écriture des sources HLSL sur le thread de chargement
    en était une cause (5/5 réussis sans). Écriture désormais sur un thread dédié : ~1 blocage sur 4 restant, cause
    non trouvée (ni les vidages d'objets, désormais optionnels via `torchlight_trace_objets`, ni `unbindGpuProgram`).
    Contournement : relancer. Vérifier sans outils avec plus d'essais pour savoir si l'émulation seule se bloque aussi.
    Diagnostic : lancer sous gdb (ptrace_scope=1 interdit de s'attacher après coup), `kill -INT` puis
    `thread apply all bt`.
  - `Readable()` du traceur et de la capture : `process_vm_readv` (sans verrou) sous Linux.
- 2026-09-30 : **émulation du pipeline fixe** : `switch/shaders/ffp/ffp_vs_3_0_main.hlsl` (tl_wvp = P * V * W, Matrix4
  d'Ogre, row_major) et `ffp_ps_3_x_main.hlsl` (deux étages LayerBlendModeEx : opérations LBX_*, sources LBS_*,
  arguments manuels ; test alpha), compilés par build_lib.sh (hash « ffp »). L'outil les utilise pour les dessins
  `prog 0 0`, remplit leurs constantes comme des constantes nommées d'Ogre (FfpConstants), n'active un étage que si
  son unité a une texture, et donne une couleur de sommet blanche quand la déclaration n'en a pas (règle D3D9).
  Résultat : 171/171 dessins, interface identique à l'émulation, texte net
  (`switch/images/2026-09-30_mine_rendu_natif_complet.png` contre `..._mine_emulation.png`).
  **Phase 2 terminée.** Prochaine étape : phase 3, brancher le rendu dans le jeu (PC d'abord).

- 2026-10-01 : **phase 3, étapes A et B : le rendu natif tourne dans le jeu sur PC** (`--torchlight_natif=true`,
  script `switch/natif_pc.sh`), image présentée à la place de celle de l'émulation, qui continue de tourner en dessous
  (synchronisation du jeu intacte). Mine : 171 dessins/image, aucun ignoré, image conforme à l'émulation.
  - **Cœur commun** `src/natif/rendu.{h,cpp}` : reçoit les dessins un par un (`DrawDesc` : valeurs d'Ogre et pointeurs
    vers la mémoire du jeu), sans dépendre du SDK (fonctions Vulkan par vkGetInstanceProcAddr). L'outil hors ligne s'en
    sert aussi : PNG identique à l'octet près à l'ancien outil sur trace-mine-6 (image finale et cibles 207/208).
    Deux images en vol (tampon de commandes « pre » pour les envois de textures + « main »), tampons d'envoi de 64 Mo,
    caches de shaders, pipelines et textures (clé = empreinte du contenu, libérée après 600 images sans usage).
  - **Jeu** `src/torchlight_natif.cpp` : suit l'état d'Ogre (mêmes interceptions que la capture), construit un
    `DrawDesc` par `_render`, présente à la fin de `_updateAllRenderTargets`. Lecture de la mémoire du jeu partagée
    avec la capture : `src/torchlight_invite.h`.
  - **Présentation** : ajout au `Presenter` du SDK (`switch/patches/nfsmw-sdk.patch`) : `SetGuestOutputFromApp(true)`
    fait ignorer les RefreshGuestOutput de l'émulation, `RefreshGuestOutputFromApp` passe. L'image de sortie
    (A2B10G10R10, sans usage « transfer dst ») est remplie par un triangle plein écran (`switch/shaders/sortie`,
    compilé dans `src/natif/present_spv.h`). Le plugin Xenos est une .so sur PC : un symbole faible fourni par
    l'exécutable n'aurait pas été fiable.
  - **Piège : l'interface (CEGUI) réécrit son tampon de sommets à la même adresse au cours d'une image.** Une
    conversion « une fois par image et par adresse » réutilisait les sommets du premier lot (icônes brouillées, de
    façon intermittente). L'identité d'un tampon de sommets ou d'indices est désormais l'empreinte XXH3 de son contenu.
  - Noms de textures et tables de constantes relus (pas de cache éternel par adresse : Ogre recrée des objets aux
    mêmes adresses). Tout appel hors du thread de rendu d'Ogre est ignoré (aucun observé).
  - Outils de diagnostic : `--torchlight_natif_ecrans=60,70,...` (captures d'écran de l'image présentée,
    `trace/ecran_<s>.ppm`) ; pendant une capture (`--torchlight_capture_s`), relevé `trace/capture/natif.txt`
    (empreintes FNV des données et états de chaque dessin) à comparer à frame.txt.
  - Défaut corrigé au passage dans la capture : valeurs par défaut de `depth_func`/`alpha_func` (EQUAL et GREATER
    au lieu de LESS_EQUAL et ALWAYS_PASS ; sans effet visible, le jeu les fixe avant de dessiner).
  - Outils de shaders recompilés dans `switch/out-pc/outils` (glslang/StandAlone/glslangValidator,
    spirv-tools/tools/spirv-opt|spirv-val), hors du scratchpad cette fois.
  - Blocage intermittent au démarrage (déjà connu avec les outils actifs) : vu une fois sur ~8 lancements.
  - **Mesures (PC, Intel UHD 620, émulation toujours active)** : mine 15 images/s ; par image 61 ms, dont 35 ms dans
    nos dessins (0,2 ms par dessin : lectures de la mémoire du jeu par process_vm_readv, conversion des sommets,
    empreintes) et 0,4 ms de présentation. Prochaines étapes : (C) ne plus transmettre les dessins au Direct3D du
    jeu (émulation à vide), (D) réduire le coût par dessin.
- 2026-10-01 : **étapes C et D : rendu natif seul, et coût par image ramené de 35 à 7 ms sur PC.**
  - **Piège : Ogre 1.7 met à jour ses tampons Direct3D paresseusement** (unlock marque les ressources périmées, la copie
    a lieu dans setVertexBufferBinding / getD3D9IndexBuffer, appelés par `_render`). Ne pas appeler `_render` cassait
    tout (interface fantôme, monde noir), et lire les tampons avant l'original donnait une image de retard.
    `_render` est donc toujours appelé ; la capture et le rendu natif lisent le dessin après lui, et
    `--torchlight_natif_seul` neutralise seulement les deux appels de dessin du Direct3D Xbox faits par `_render`
    (`sub_821CF830` DrawIndexedVertices, `sub_821D0A10` DrawVertices). Parcours titre -> chargement -> mine identique
    au mode normal, images justes.
  - Gains (mine, 171 dessins) : plages de sommets/indices du dessin seulement (plus d'empreinte de déduplication),
    conversion directe vers la mémoire d'envoi (le GPU intégré rend la relecture sur place très lente : 5,1 -> 2,0 ms),
    cache des pages vérifiées lisibles (appels système : ~11 -> 0,7 ms), vérification adaptative du contenu des
    textures (jusqu'à toutes les 32 images si inchangé : 2,4 -> 0,7 ms), shaders retrouvés par empreinte.
    Résultat : 25 ms par image de jeu dont 7,4 de rendu natif (sommets 2,2, constantes 0,5, textures 0,5 + 0,7,
    descripteurs 0,9, reste de l'enregistrement Vulkan ~1,8), 34 images/s (le jeu vise 30).
  - Outil hors ligne toujours identique à l'octet sur trace-mine-6 après ces changements.
  - Dessins vides (0 indice) écartés sans compter d'erreur.
  - **Version Switch compilée** avec le rendu natif (switch/out/torchlight.nro). `switch/torchlight.toml` active
    `torchlight_natif` et `torchlight_natif_seul` ; `switch/carte_sd.sh` assemble `switch/out/sd/` (.nro, réglages,
    dossier `shaders/` de 225 shaders) et garde l'ELF non strippé dans switch/elf/.
  - À surveiller sur console : l'émulation reçoit encore les effacements, résolutions EDRAM et présentations du
    Direct3D du jeu (3 cibles par image) ; sur Switch une résolution coûtait 3-4 ms de GPU à nfsmw-nx. À neutraliser
    aussi si les mesures le montrent.
- 2026-10-01 : **personnages animés corrigés** (signalé en jeu : personnage et familier flottant au menu, pieds dans
  le sol avec la silhouette bleue en jeu). Les indices d'os (VET_UBYTE4) doivent être retournés comme un mot de
  32 bits : le Xenos les lit gros-boutistes, et les shaders du portage Xbox d'Ogre prennent le premier os en
  composante [3] (`world_matrix_array_3x4[iBlendIndices_0[3]]`). Vérifié sur une capture de l'écran titre.
  - Bibliothèque complétée avec les sources HLSL relevées pendant une partie (265 sources, 267 shaders avec le
    pipeline fixe). `prep_hlsl.py` donne une copie locale aux constantes globales que le RT Shader System écrit
    (`FFP_Modulate(iColor_0.xyz, light_diffuse1.xyz, light_diffuse1.xyz)` : interdit en Vulkan) : 0 échec.
- 2026-10-01 : **test console n°4 (rendu natif)** : plantage avant l'écran titre (`std::terminate: std::system_error`).
  Symbolisé avec l'ELF : le traceur démarrait son thread d'écriture des sources HLSL (`std::thread`), impossible sur
  la console. Le rendu natif seul n'écrit plus aucun fichier de trace (seulement avec `torchlight_trace` ou la
  capture) ; création du thread protégée (écriture directe en cas d'échec). Aussi : lister le dossier `shaders/`
  échouait sur la console (« bibliothèque vide ») : le chemin de chaque shader est construit directement
  (`<empreinte>_vs_3_0_main.spv` / `_ps_3_x_main.spv`) si la liste est vide.
- 2026-10-01 : **test console n°5** : le jeu démarre en rendu natif, mais menu à 2-4 images/s, puis plantage au
  chargement de la ville.
  - rex_perfil.log au menu : thread du jeu à 12 % de CPU, console à 24 % : le jeu **attend** le GPU émulé, qui faisait
    encore par image 5 résolutions, 25 transferts de cibles et 117 dessins internes (effacements et résolutions EDRAM
    que le Direct3D du jeu continue d'envoyer). Notre rendu : 4-5 ms par image, présentation < 1 ms.
    Correctif SDK (`switch/patches/nfsmw-sdk.patch`, command_processor.cpp) : quand l'application fournit l'image
    (`IsGuestOutputFromApp`), le paquet de dessin est lu et compté comme exécuté sans appeler `IssueDraw` (qui fait
    aussi les copies/résolutions). Événements et registres de synchronisation inchangés. PC : parcours identique.
  - Plantage : `std::__ios_failure` (basic_filebuf::underflow) en lisant un shader sur la SD : std::ifstream lève une
    exception sur erreur de lecture. Lectures en stdio, trois essais, jamais d'exception.
- 2026-10-01 : **test console n°6 : le jeu est jouable sur Switch** (« très fluide », ville et mine atteintes), portable.
  Menu 28-34 images/s (30 ms par image dont 4,8 de rendu natif) ; ville 12-16 images/s (51-70 ms, dont 14-19 de rendu
  natif : Vulkan 8-10, textures 2,5-3,6). Le thread principal du jeu est à 99 % : il fait la logique, Ogre et
  maintenant notre rendu. Pistes : enregistrement Vulkan sur un autre cœur (3 cœurs disponibles, presque libres),
  cache de pipelines sur la SD (jusqu'à 51 pipelines créés en 10 s aux chargements : 0-1 image/s), puis fonctions
  du jeu en natif comme nfsmw-nx.
  - Shaders absents de la bibliothèque, vus seulement sur console : 0b6308257c08cfce_vs, 72e4b520a4f43445_vs,
    ff2e6769883b82f3_ps (8 à 34 dessins ignorés par image dans certaines scènes) : à relever en jouant sur PC.
  - Plantage à la fermeture seulement (`std::system_error` dans `MnkInputDriver::~MnkInputDriver`, SDK, pendant
    `Runtime::Shutdown`) : sans effet en jeu.
- 2026-10-01 : **shaders compilés par la machine qui fait tourner le jeu, console comprise** (demande de
  l'utilisateur : ne pas distribuer de shaders compilés). Plus de dossier `shaders/` sur la carte SD.
  - À chaque `D3DXCompileShader` du jeu (source générée par le RT Shader System), `natif::ShaderSource` compile la
    source si elle n'est pas en cache : `<données utilisateur>/cache/natif/<empreinte>_<profil>_main.spv/.refl/.io`
    (mêmes fichiers que build_lib.sh ; le rendu ne change pas). Pipeline fixe : sources de switch/shaders/ffp
    embarquées (`src/natif/ffp_sources.h`, `switch/shaders/ffp/build.sh`), compilées au démarrage.
  - `src/natif/hlsl_prep.cpp` : portage C++ de prep_hlsl.py, identique à l'octet sur les 265 sources et les 9 fichiers
    de rtshaderlib (`switch/tools/prep_test.cpp`). `src/natif/hlsl_compiler.cpp` : glslang en bibliothèque, mêmes
    options que glslangValidator, légalisation par les passes SPIRV-Tools de glslang, validation. rtshaderlib lu
    dans les fichiers du jeu (noms connus : lister un dossier échoue sur la console), dossiers créés par mkdir.
  - SDK : option `REXGLUE_GLSLANG_HLSL` (thirdparty/CMakeLists.txt, dans nfsmw-sdk.patch) : glslang avec HLSL et
    optimiseur, SPIRV-Tools compilé. Activée par switch/CMakeLists.txt ; ENABLE_HLSL doit aussi être défini pour
    nos fichiers qui incluent ShaderLang.h (l'interface de TShader en dépend).
  - PC, à froid : 179 shaders compilés en arrivant dans la mine, 9,8 ms en moyenne (18,5 au plus), 0 échec, images
    justes ; second lancement : rien de recompilé. `--torchlight_natif_shaders=<dossier>` permet encore d'utiliser une
    bibliothèque construite par build_lib.sh (développement).
  - **Cache de pipelines Vulkan** gardé entre deux lancements : `cache/natif/pipelines.bin` (vkGetPipelineCacheData),
    relu à la création du rendu, réécrit au bilan périodique une fois la scène stable (aucun pipeline créé pendant
    10 s : pas d'écriture sur la SD en plein chargement). Temps de création des pipelines dans le bilan
    (« N pipelines créés (x ms) »). Mesure sans intérêt sur PC (Mesa y a son propre cache disque) ; sur la console,
    nfsmw-nx mesurait ~60 ms par pipeline sans cache.
- 2026-10-01 : **tests console n°7 et n°8 (compilation sur la console, à froid puis avec cache).**
  - À froid : 241 shaders compilés par la console, 0 échec, plus aucun dessin ignoré ; mais 204 ms par shader en
    moyenne (médiane 190, max 465), 49 s au total réparties sur les chargements (PC : 10 ms). Pipelines sans cache :
    ~70 ms chacun (34 en 2 469 ms).
  - Second lancement : 0 shader recompilé, cache de pipelines relu (1,3 Mo) : 46 pipelines en 4 ms. Une fois
    chargé : menu 26-38 images/s, ville 11-17, mine ~21. Les chargements restent à 0-1 image/s (le jeu charge).
  - Plantage à la fermeture toujours (MnkInputDriver, voir tâches de CLAUDE.md).
  - Pour la compilation à froid : un seul fichier par shader dans le cache (`<nom>.tlsh`, src/natif/shader_pack.h :
    en-tête, SPIR-V, réflexion, .io ; fichier incomplet détecté par sa taille) au lieu de trois fichiers écrits via
    temporaire + renommage (une quinzaine d'opérations sur la SD par shader). Le journal donne la répartition de
    chaque compilation (préparation, analyse, liens, SPIR-V, écriture) ; PC : 2 / 3 / 0,1 / 4,7 / 0,2 ms.
    Les anciens .spv/.refl/.io restent lisibles (bibliothèque de build_lib.sh).
- 2026-10-01 : **test console n°9 : blocage au démarrage** (écran noir, ou chargement figé sans musique), 3 fois.
  Piles relevées par le profileur du runtime (fichier vide `perfil_pilas.flag` dans le dossier de rex_perfil.log) :
  le thread principal du jeu attend le **verrou global de glslang** (`SetupBuiltinSymbolTable` -> pthread_mutex_lock)
  pendant la compilation du premier shader, alors qu'aucun autre thread ne le tient. Cause : le SDK initialise aussi
  glslang, à la demande (`CompileGlslToSpirvInternal`, ses shaders GLSL), sur un autre thread ; et `InitGlobalLock`
  de glslang (OSDependent/Unix) refait `pthread_mutex_init` à chaque InitializeProcess : verrou réinitialisé pendant
  qu'un autre thread le tenait. Course : les tests n°7 et 8 étaient passés par chance.
  Correctif : initialisation du verrou une seule fois (pthread_once), `switch/patches/glslang-verrou.patch` (glslang
  n'est pas suivi par git dans nfsmw-nx, à réappliquer si on refait sdk/thirdparty). Aussi : la compilation ne se
  fait plus en tenant le verrou du traceur (le thread de rendu le prend à chaque liaison de programme).
- 2026-10-01 : **test console n°10 : correctif du verrou de glslang validé.** À froid : 155 shaders compilés, 0 échec,
  pas de blocage ; second lancement : 0 recompilé, cache de pipelines relu (54 pipelines en 5 ms). Répartition par
  shader sur la console : préparation 3,6 ms, analyse 11,1, liens 0,4, SPIR-V (optimisation + validation) 37,6,
  écriture sur la SD 41,1 (total ~94 ms, contre 204 ms avant le fichier unique .tlsh et le verrou).
- 2026-10-01 : **sauvegarde sur Switch** (tâche de CLAUDE.md) : le jeu écrit save.tmp puis le renomme en 0.tsv par
  NtSetInformationFile sur le fichier encore ouvert ; Horizon refuse de renommer un fichier ouvert
  (`RenameEntryInternal: failed to rename ... save.tmp -> 0.tsv`), le personnage restait dans save.tmp. Correctif SDK
  (host_path_entry.cpp, nfsmw-sdk.patch) : si le renommage échoue, copie vers le nouveau nom puis suppression de
  l'ancien si possible. À vérifier sur console.
- 2026-10-01 : **compilation des shaders en arrière-plan** : l'interception de D3DXCompileShader met la source en
  file ; un thread dédié compile (créé dans SetPaths, sur le thread principal de l'application et non sur un thread du
  jeu : c'est depuis un thread du jeu que std::thread avait échoué ; pthread_create avec une pile de 1 Mo, regex de
  libstdc++ et analyseur de glslang étant récursifs). Sur la console : priorité 0x3C (sous celle du jeu, 0x3B, qui ne
  partage le temps qu'entre threads de même priorité : le compilateur ne prend jamais le processeur au jeu mais occupe
  un cœur libre), cœurs 0-2. Le rendu recherche à nouveau un shader absent quand le compilateur en a ajouté
  (`Context::shader_generation`) : un objet dont le shader n'est pas prêt apparaît un peu plus tard au lieu de
  rallonger le chargement. Sans thread, repli sur la compilation dans le thread du jeu. PC à froid : 179 shaders,
  3 dessins seulement en attente d'un shader, images justes.
- 2026-10-01 : **test console n°11** : sauvegarde corrigée (« save.tmp copié vers 0.tsv », personnage relu au
  lancement suivant), chargements plus rapides avec la compilation en arrière-plan (au plus 5 shaders en attente,
  très peu d'objets en retard). Un shader non écrit (« écriture du cache impossible ») : collision probable avec le
  rendu qui ouvrait le fichier pour voir s'il était prêt -> écriture dans <nom>.tlsh.tmp puis renommage, 3 essais.
  Défaut signalé : **ombres des personnages étirées derrière eux, par intermittence** (vu sur Switch).
  - Rôle de la cible 208 compris : **carte de lumière** (éclairages dynamiques vus de dessus : cristaux, lanternes),
    projetée sur la scène ; 207 = carte d'ombres (fond gris 0,5, silhouette au centre).
  - Adressage des textures d'Ogre maintenant suivi (`_setTextureAddressingMode`, case 47 ; `_setTextureBorderColour`,
    case 48) : un échantillonneur par mode et couleur de bordure. Mesuré : 207 et 208 sont en « bloquer » (comme
    avant), les autres surtout en « répéter » ; pas la cause des traînées, mais plus fidèle (relevé natif.txt :
    « adresse u v w bordure b »). Pas encore dans la capture / l'outil hors ligne.
  - **Décalage de profondeur** (`_setDepthBias`, case 68, ~28 appels par image) jusqu'ici ignoré : piste retenue pour
    des ombres/lumières posées sur le sol qui gagnent ou perdent le test de profondeur selon le GPU. Le portage Xbox
    envoie constantBias * K (K = -4e-6 = -1/250000, lu en mémoire à 0x820AA720, comme l'Ogre D3D9 d'origine) et
    -slopeScaleBias ; en Vulkan : état dynamique, constante * 2^24 (unité d'une profondeur 24 bits), pente telle
    quelle. À vérifier sur console.
  - **Confirmé sur console : plus de traînées d'ombre** avec le décalage de profondeur. C'était bien la cause.
- 2026-10-01 : **enregistrement Vulkan sur un thread dédié** (fluidité : en ville, le thread du jeu était à 99 %, dont
  15-19 ms de rendu natif). Le thread du jeu relève chaque image (Frame : commandes cible/viewport/effacement/fin de
  cible/dessin, DrawDesc, copies des plages de sommets et d'indices et des valeurs de constantes dans une zone par
  blocs ; tables de noms des constantes partagées par shared_ptr) ; le thread de rendu la rejoue dans Renderer
  (inchangé), présente et fait le bilan. Trois images en circulation : le jeu a au plus une image d'avance et attend
  sinon (« attente du rendu » dans le bilan). Thread créé dans SetPaths (pthread, pile 1 Mo), priorité 0x3B sur la
  console, cœurs 0-2. `--torchlight_natif_thread=false` : rejeu sur le thread du jeu (comparaison).
  PC (mine) : thread du jeu 14 ms par image dont 2,1 de relevé (au lieu de ~7,5 de rendu natif), rendu 2,8 ms en
  parallèle, 57-59 images/s (au lieu de ~50). Images justes.
- 2026-10-01 : **test console n°12 (thread de rendu)** : en ville (~315 dessins), image côté jeu 62 ms au lieu de ~70
  (12 images/s au lieu de 11) ; relevé 11 ms (dont empreintes de textures 5,3), rendu 10 ms en parallèle, attente
  du rendu ~0. Le reste (~50 ms) est le code du jeu lui-même (logique, Ogre, Direct3D du jeu).
  Relevé allégé : identité des textures en cache par objet Ogre tant que sa fetch constant ne change pas ; empreinte
  du contenu calculée sur le thread de rendu (TextureRef::ident/content, avant Draw) ; descriptions de dessins et
  copies de constantes réutilisées d'une image à l'autre (DrawDesc::Clear) ; chargeur de texture partagé
  (shared_ptr) au lieu d'être recopié. PC : relevé 2,1 -> 1,3 ms (textures 0,55 -> 0,18).
  Blocage intermittent au démarrage (déjà connu, ~1 lancement sur 3 avec la manette de script sur PC) : thread
  principal du jeu dans sub_82774170 (attente de place dans l'anneau de commandes : compare le pointeur de lecture
  recopié en mémoire, device+11024, à la dernière valeur vue), appelé par la présentation (_swapAllRenderTargetBuffers),
  thread de commandes GPU inactif. Cause toujours inconnue ; pas vu sur console récemment.
- 2026-10-01 : **test console n°13 (relevé allégé) et profil du thread principal en ville** (perfil_pilas.flag).
  Relevé 11 -> 5,2 ms (textures 4,3 -> 0,8) ; image côté jeu 55-59 ms (13 images/s).
  Profil (temps propre, ~370 échantillons) : **~17 % dans des verrous** (mutexLock/Unlock, pthread_once, call_once)
  venant de nos interceptions (traceur : call_once dans On(), compteurs atomiques à chaque appel du RenderSystem ;
  capture : verrou à chaque setter même sans capture ; verrou du traceur à chaque bindGpuProgram). Chemin rapide :
  On() sans call_once une fois démarré, compteurs/détails seulement avec g_write, capture qui sort avant son verrou
  si inactive (g_enabled atomique), empreinte des programmes en cache par thread (ProgramHash). Ensuite : memcpy
  6,7 %, itérateur de std::map du jeu (sub_8262D6C8) 4,6 %, sub_821C7158 4,9 %, bindGpuProgramParameters 2,9 %.
  Piles du profileur tronquées à 10 niveaux : SDK modifié (switch_perf.cpp, kFrames 48, échantillons bruts dans
  rex/rex_pilas.txt : « thread|pc retours... » relatifs à l'image) pour un temps inclusif complet.
- 2026-10-01 : **test console n°14 (interceptions sans verrous)** : en ville, image côté jeu 47-50 ms au lieu de 55-59
  (15-16 images/s au lieu de 13). Profil complet (rex_pilas.txt, 2514 échantillons du thread principal, seconde
  moitié = ville) : _updateAllRenderTargets 72 % ; nos interceptions et relevé ~10 % (natif::Render 3,7, Enter du
  traceur 2,9, BindParameters 1,3, DrawDesc::Clear 0,9) ; Direct3D du jeu ~15 % (_setTextureUnitSettings 4,7,
  _render 4,5, bindGpuProgramParameters 2,6, _setTextureBlendMode 1,6, _setTexture 1,4...) ; D3DXCompileShader
  7,9 % (179 sources distinctes par session, chacune compilée une fois : nouveaux shaders rencontrés en ville ;
  piste : garder le résultat d'un lancement à l'autre, mais Ogre a besoin de la vraie table de constantes) ; lectures
  de fichiers du jeu sur son thread principal (pread, svcSendSyncRequest) 3,3 %.
  Rendu natif seul : l'original des méthodes qui ne font que transmettre un état à Direct3D (vérifié dans Ogre 1.7,
  aucun membre modifié) n'est plus appelé pendant une image : _setTextureBlendMode, _setTextureUnitFiltering (x2),
  _setTextureLayerAnisotropy, _setTextureAddressingMode, _setTextureBorderColour, _setTextureMipmapBias,
  _setSceneBlending (simple et séparé), _setAlphaRejectSettings, _setDepthBias, _setPointParameters, setAmbientLight.
  Gardés : _setTextureMatrix, _setTextureCoordSet, _setTextureCoordCalculation (mTexStageDesc), _setTexture (touch()
  charge la texture), bindGpuProgramParameters (paramètres partagés). PC : images justes du titre à la mine.
  Outil : switch/console.sh (MTP).
- 2026-10-01 : **test console n°15** (états Direct3D non transmis) : ville 46,5-47,7 ms au lieu de 47-50 (16 images/s).
  Interceptions allégées : sans traceur ni capture, aiguillage direct vers le rendu natif (NativeOnly, sans le corps
  générique d'Enter ni les fonctions de la capture) ; type de programme (sommets/pixels) en cache ; valeurs des
  constantes copiées brutes (gros-boutistes) dans l'image et converties en flottants par le thread de rendu
  (Constants::raw_be) ; DrawDesc::Clear ne remet plus à zéro matrices et étages du pipeline fixe (remplis par Render
  pour ces dessins). Hors ligne identique à l'octet ; PC : images justes du titre à la mine.
- 2026-10-01 : **test console n°16** (aiguillage direct, constantes converties sur le thread de rendu) : en ville
  (280-307 dessins), image côté jeu 44,5 ms (17 images/s), relevé 4,7, rendu 9,8 en parallèle. Bilan de la journée
  en ville : ~70 ms (11 images/s) avec le rendu natif sur le thread du jeu -> 44,5 ms (17 images/s).
  Reste sur le thread principal : surtout Ogre et la logique du jeu (~50 %), D3DXCompileShader pour les shaders
  nouveaux (~8 % en ville, beaucoup plus aux chargements), lectures de fichiers du jeu (~3 %).
- 2026-10-01 (soir) : **essai d'allègement de D3DXCompileShader : échec, désactivé.** Idée : donner au compilateur
  Xbox une source réduite (mêmes déclarations, corps qui ne fait que référencer chaque constante, premier et dernier
  élément des tableaux), son microcode ne servant plus en rendu natif seul (natif::MakeStubSource, interception de
  sub_82777F10, repli sur la source d'origine si échec). PC : 10,8 ms par compilation au lieu de 27 (-60 %), mais le
  décor n'est plus dessiné (interface intacte) : la table de constantes qu'Ogre en tire n'est pas équivalente
  (probablement tailles/registres selon l'usage réel : à étudier dans D3D9HLSLProgram::processParamElement du
  portage, sub_82587FB0). Code gardé derrière `--torchlight_natif_d3dx_allege` (faux par défaut).
- 2026-10-02 : **résultats de D3DXCompileShader gardés d'un lancement à l'autre** (piste 1 de CLAUDE.md).
  Dans le compilateur Xbox (sub_82777F10 -> 82777D88 -> 82777848 -> 82777528 -> 827A8350 -> 827A73F0), la table de
  constantes est tirée d'un **code intermédiaire** par sub_82778058 (équivalent de D3DXGetShaderConstantTable :
  code, ppConstantTable), puis ce code est converti en microcode final par sub_8278D688 (r4 code, r5 taille). La
  table ne garde pas de référence au code (tampon libéré ensuite). Tampon D3DX (D3DXCreateBuffer = sub_82776E20,
  taille, ppBuffer) : 16 octets, taille en +8, données en +12. Ogre (sub_82587FB0) passe ppShader = this+496,
  ppConstantTable = this+500, ppErrors sur sa pile (ignoré en cas de succès), 11e argument = structure mise à zéro.
  - Vraie compilation : relevé du code passé à sub_8278D688 et de l'adresse donnée à sub_82778058 (même code
    vérifié, une seule conversion et une seule table, sinon pas de cache) ; microcode final + code intermédiaire
    ajoutés à `cache/natif/d3dx.bin` (un seul fichier complété en fin, écrit par le thread de compilation, lu en
    entier au démarrage ; fin illisible tronquée). Clé : FNV de la source, des defines, de l'entrée, du profil et
    des options.
  - Lancement suivant : tampon créé par le jeu (sub_82776E20) et rempli avec le microcode, table reconstruite par
    sub_82778058 sur le code intermédiaire : résultat identique à une vraie compilation, sans le compilateur.
  - Option `--torchlight_natif_d3dx_cache` (vrai par défaut). Inactif avec la source allégée (expérimentale).
  - PC (manette de script jusqu'à la mine) : à froid 175 compilations, 34,7 ms en moyenne (6,1 s au total),
    fichier de 260 Ko ; second lancement 175/175 trouvées, 0,1 ms par appel (19 ms au total). Images identiques
    (seule la pose du personnage diffère). À mesurer sur la console (chargements, saccades en ville).
- 2026-10-02 : **piste 3 (méthodes du RenderSystem appelées pour rien), premiers relevés dans le portage Xbox.**
  - `_setTextureCoordSet` (821CAD88) et `_setTextureCoordCalculation` (821CB0F0) : un ou deux stores dans
    mTexStageDesc, sans Direct3D : rien à gagner, gardés.
  - `_setTextureMatrix` (821CA930) : calcule la matrice (inverse, transposée, frustum) puis ne la garde ni ne la
    transmet (pas de pipeline fixe sur Xbox) : coupé en rendu natif seul (TL_SLOT_D3D).
  - `_setTexture` (821C8E40) : touch(), recherche de la texture Direct3D du device, et seulement si elle change,
    écriture de la fetch constant dans le périphérique (sub_821CEB60, quelques opérations) : gardé.
  - `bindGpuProgramParameters` (821C2118) : _copySharedParams (gardé : lu par le rendu natif), paramètres actifs
    (gardé), puis copie de chaque constante dans le périphérique : sub_821BFB68 / sub_821C24D8 (flottants, vecteurs),
    sub_8276D9F0 / sub_8276D998 (entiers). Ces quatre-là sont neutralisés pendant bindGpuProgramParameters en rendu
    natif seul (t_skip_d3d_consts, comme les dessins dans _render). PC : images justes jusqu'à la mine ; gain noyé
    dans le bruit sur PC, à mesurer sur la console.
- 2026-10-02 : **profil du PC avec perf** (`perf record --call-graph dwarf` sur le thread « Main XThread », noms des
  fonctions générées `sub_X` ; classes et cases des fonctions virtuelles depuis les RTTI : image mémoire relue dans
  /proc/<pid>/mem à 0x182000000 par un script parent du jeu, puis tools/rtti_vtables.py). Mine, avant :
  natif::Render 9,8 % du thread principal (memcpy du relevé 5,1, invite::Readable 2,2 dont process_vm_readv) ;
  **verrouillage des tampons de sommets d'Ogre 7 %, dont 4 % d'attente du GPU émulé** : BillboardSet (particules,
  billboards) réécrit ses tampons à chaque image -> D3D9HardwareVertexBuffer::updateBufferResources (821A71E8) ->
  Lock du Direct3D Xbox (821A70D8 sommets, 8276BA90 indices -> 821A6D90) -> sub_821A5C10 (attente de la fence du
  tampon, boucle sur sub_82774170).
  - **Rendu natif seul : Lock sans attente** (`--torchlight_natif_verrou_sans_attente`, vrai par défaut). Pendant
    ces deux Lock, sub_821A5C10 est remplacée : si la fence est celle du tampon de commandes en cours, envoi au GPU
    comme l'original (sub_821F2910), mais sans attendre. Premier essai qui sautait aussi l'envoi : GPU émulé bloqué
    sur WAIT_REG_MEM et jeu figé.
  - PC, mine : image côté jeu ~14 ms -> 6-8 ms (60 images/s, plafond) ; le thread principal attend maintenant
    surtout la présentation (_swapAllRenderTargetBuffers -> sub_821F31C8 -> sub_821A5C10, 32 %). Images justes.
  - Blocage au démarrage (connu) mesuré : 2 lancements sur 6, avec comme sans l'option (même endroit : présentation
    -> sub_821A5C10 -> sub_82774170, GPU émulé sur WAIT_REG_MEM). Pas causé par ce changement.
- 2026-10-02 : **relevé allégé : géométrie reprise d'une image à l'autre, pages vérifiées sans collisions.**
  - Toute écriture du jeu dans un tampon de sommets ou d'indices Direct3D passe par son Lock (821A70D8, 8276BA90,
    déjà interceptés) : `natif::BufferLocked` compte les Lock par adresse physique. Le relevé garde ses copies de
    plages (adresse, début, taille ; plus grand indice pour les indices) dans `g_geometry` et les reprend tant que
    le tampon n'a pas été verrouillé depuis ; l'image garde les copies reprises en vie (`Frame::kept`, shared_ptr)
    jusqu'au rejeu. Copies inutilisées depuis 120 images oubliées. Bilan : « relevé de la géométrie, Ko par image :
    N copiés, M repris ».
  - `invite::Readable` : table de bits des pages vérifiées pour tout l'espace invité (128 Ko par thread qui lit,
    allouée au premier usage) au lieu de 4096 entrées indexées par page & 4095 (tas et piles se chassaient) ;
    getpid une seule fois (PC).
  - PC, mine : 26 Ko copiés par image, 4999 Ko repris ; relevé 1,4 -> 0,42 ms ; image côté jeu ~5,5 ms. Images
    justes (écran de chargement, interface, mine).
- 2026-10-02 : **test console n°17** (cache D3DX, piste 3, Lock sans attente, géométrie reprise). Journaux :
  switch-logs/torchlight_002.log (à froid), _003.log (second lancement).
  - Cache D3DX validé : à froid 117-138 ms par shader sur la console ; au second lancement 0,1 ms (88 entrées
    lues, toutes trouvées), soit ~11 s de compilation en moins sur les chargements jusqu'à la ville.
  - Ville : image côté jeu 37-43 ms (17-20 images/s) au lieu de 44,5 (17) ; relevé 3,0-3,7 ms au lieu de 4,7 ;
    géométrie : 50-70 Ko copiés par image, 5,4-5,8 Mo repris. Rendu juste.
  - Le Lock sans attente change peu sur la console : le thread « GPU Commands » n'y prend que 1,8 % (le GPU émulé
    suit), alors que sur PC l'attente pesait 4 %. Le thread principal est à 98 % d'un cœur : limite = code du jeu.
  - rex_perfil.log : **processeur à 1020 MHz** (portable, fréquence par défaut), GPU 537,6 MHz.
- 2026-10-02 : **profil des piles de la console en ville** (rex_pilas.txt, test n°17 bis ; outils switch/tools/pilas.py,
  profil_arbre.py, profil_descente.py, profil_noms.py ; thread « Main XThread (F », seconde moitié = ville).
  Rendu d'Ogre 74 % (fenêtre 48,7, **textures de rendu 207/208 : 25,4**), logique du jeu 25,8. Dans le rendu :
  renderSingleObject (sub_821C5FA0) ~32 % (paramètres automatiques sub_821C26D0 8,4, écouteur du RT Shader System
  sub_821C4F50 5,9, natif::Render 6,4, _render 4), recherche des objets visibles (octree, sub_821A6070) 15,3,
  particules ParticleUniverse ~9, interface CEGUI 5,6. Profil plat : la plus chaude des fonctions fait 2,6 % en
  temps propre. Notre code ~10 % (natif::Render 7, dont CopyRange 2,7 ; interceptions ~2).
  - memcpy (sub_82860A50) et memset (sub_82861140) du jeu remplacés par ceux de l'hôte (src/torchlight_crt.cpp,
    `--torchlight_crt_natif`, méthode de nfsmw-nx) : memcpy du jeu 1,85 % en temps propre.
  - CopyRange : compteurs de Lock en table fixe de compteurs atomiques (plus de verrou ni de map) ; TextureRef du
    relevé réutilisées d'une image à l'autre (DrawDesc::reused_textures : plus d'allocation de nom par texture et
    par dessin) ; ProgramIsVertex sans map. PC : images justes.
  - Compteurs d'appels du RenderSystem retirés de 5 fonctions partagées avec tout le jeu : sub_825BC790 (fonction
    vide `blr`, 1591 cases de tables virtuelles et 243 appels directs : notre compteur s'exécutait à chaque méthode
    vide du jeu, 1,2 % du thread principal), sub_828AD748 (`return 0`), sub_828AD7C0 (`return 1`), sub_821910A8,
    sub_821F1790 (accesseurs).
  - sub_821C28D8 = XMemCpy (memcpy vectorisé du SDK Xbox, copies de paramètres de shaders) : par l'hôte aussi.
  - **Première fonction d'Ogre en natif** (src/torchlight_ogre_natif.cpp, `--torchlight_ogre_natif` 0/1/2) :
    sub_821C74A0, test boîte englobante contre les 6 plans du frustum (variante Xbox de Frustum::isVisible, en
    VMX128 ; 2,6 % du thread principal en temps propre sur la console, appelée par le parcours de l'octree). Mode 2
    (validation contre le recompilé) sur PC du titre à la mine : 0 différence sur ~10 millions d'appels.
- 2026-10-02 : **test console n°18** (frustum natif, XMemCpy, fonctions partagées sans compteur) : ville à ~300
  dessins, image côté jeu 36-37 ms au lieu de 42-44 (19-21 images/s au lieu de 17-18), relevé 2,65 au lieu de 3,8.
  Profil des piles : test du frustum 2,6 -> 0,9 % en temps propre, CopyRange 2,7 -> 1,4 %.
  - Deuxième fonction d'Ogre en natif : sub_821C2CC8, produit de matrices 4x4 (Matrix4::concatenate ; paramètres
    automatiques, écouteur du RT Shader System ; 1 % en temps propre). Arrondis reproduits comme le code recompilé
    (fmuls : float(a*b) ; fmadds : float(fma(a, b, c)) en double, même ordre). Validation PC : 0 différence au bit
    près.
- 2026-10-02 : **compilation : appels directs et ordre des fonctions** (étape 6 de nfsmw-nx, docs/performance-history.md :
  appels directs + LTO + PGO + ordre des fonctions, de 30,3 à ~34 images/s ; options de compilation déjà en -O3
  pour le Cortex-A57).
  - `switch/tools/appels_directs.py` (portage de llamadas_directas.py) : chaque fonction recompilée sub_X est un alias
    faible de __imp__sub_X (pour les interceptions), que le compilateur ne peut jamais intégrer ; les appels aux
    fonctions sans interception passent à __imp__sub_X : 369 756 appels dans 218 fichiers, 332 adresses
    interceptées respectées (toute adresse 82/83xxxxxx citée dans src/, le SDK ou les .toml ; `--verifier` contrôle
    avec nm les sub_ définis par nos objets). Lancé par build_switch.sh (à refaire après chaque génération du code).
  - `switch/tools/ordre_fonctions.py` -> `switch/ordre_fonctions.ld` (1640 fonctions vues dans les profils des piles
    de la console, toutes threads), passé par `-Wl,--section-ordering-file` (option TORCHLIGHT_ORDRE_FONCTIONS,
    Switch seulement). KEEP (*(.crt0)) en tête ; build_switch.sh vérifie que _start reste à 0.
- 2026-10-02 : **test console n°19** (appels directs, ordre des fonctions, copie vers la mémoire graphique par
  l'hôte) : ville à ~300-310 dessins, image côté jeu 33-34 ms au lieu de 36-37 (22-23 images/s au lieu de 19-21).
  Depuis le matin (test n°16) : 44,5 -> 33,5 ms, 17 -> 22-23 images/s, à 1020 MHz, rendu inchangé.
  - Ordre des fonctions régénéré avec ce profil (1756 fonctions).
  - Relevé : plus de shared_ptr par plage de géométrie et par dessin (incréments atomiques sans LSE sur l'A57) ;
    les copies remplacées ou oubliées sont libérées 8 images plus tard (g_retired).
  - Essai du LTO lancé (`TORCHLIGHT_LTO`, un seul processus LTO, dossier switch/out-lto).
  - `thread_local` non triviaux (unique_ptr de la table des pages vérifiées, unordered_map de ProgramHash) passés en
    pointeurs bruts : chaque accès appelait leur routine d'initialisation (vu dans le profil de la console, avec
    __aarch64_read_tp : -mtp=soft, chaque accès thread_local est un appel).
  - **bindGpuProgramParameters, chemin court** (rendu natif seul, `--torchlight_ogre_natif=1`) : plus de parcours de
    toutes les constantes (il ne faisait plus que des copies neutralisées, ~2 % du thread principal) ; seuls
    _copySharedParams (sub_82467600), l'affectation des paramètres actifs (sub_821C25B0, this+60 / +92) et la
    destruction de l'argument SharedPtr par valeur (vtable 0x8200091C, compteur - 1, seulement si le compteur
    dépasse 1) sont faits. PC : l'original et son itérateur ont disparu du profil ; images justes.
  - Relevé des constantes : table des noms passée au rendu par pointeur brut (Constants::names_ptr) au lieu d'un
    shared_ptr copié deux fois par dessin ; tables remplacées libérées 8 images plus tard ; petit cache à
    correspondance directe (256 cases) devant g_constants. Hors ligne identique à l'octet (trace-mine-6), PC juste.
  - Appels directs étendus aux fonctions d'aide des prologues et épilogues (__savegprlr_N, __restgprlr_N,
    __savefpr_N... : 43 803 appels ; 1,5-2 % du thread principal sur la console, jamais intégrées jusqu'ici car
    appelées par leur alias faible). PC : images justes.
  - Relevé chronométré par armGetSystemTick sur la console (steady_clock::now : chaîne d'appels).
  - LTO : premier essai interrompu en phase LTRANS (pic mémoire 7,2 Go, une partition à la fois) ; relancé avec
    -flto=4 et tout le code du jour.
- 2026-10-02 : **« Return to Game Library »** : le jeu termine son titre (XamLoaderLaunchTitle(NULL) /
  XamLoaderTerminateTitle -> KernelState::TerminateTitle), son thread principal finit, et le SDK détruisait le runtime
  (QuitFromUIThread -> OnDestroy -> Runtime::Shutdown) : MnkInputDriver::~MnkInputDriver appelle
  CallInUIThreadSynchronous alors que la boucle de l'interface est arrêtée (std::system_error), avec des threads du
  jeu encore vivants. TorchlightApp::OnGuestThreadExit poste au thread de l'interface la même sortie que
  ReXApp::OnClosing (journal vidé ; console : __nx_applet_exit_mode = 1 et __libnx_exit, retour au menu ; PC :
  std::_Exit). Compatible avec le SDK officiel 0.10 (version PC d'origine).
- 2026-10-02 : **boucles inutiles (Xbox Live, succès, notifications) ?** Aucune trace dans les piles du thread
  principal en ville (aucun appel XAM/XNet) ; les autres threads du jeu (audio surtout) dorment plus de 96 % du temps
  et ne prennent que 3 à 8 % d'un autre cœur. Seul appel notable au noyau émulé : KeTlsGetValue, 1 % (TLS du jeu).
- 2026-10-02 : **KeTlsGetValue direct** (SDK, xboxkrnl_threading.cpp, dans nfsmw-sdk.patch) : appelée par
  l'allocateur mémoire du jeu (sub_821CD9A8, sub_821CDA90 : cache par thread) et le CRT, ~100 000 fois par seconde,
  1 % du thread principal sur la console. Version brute (REX_FUNC) : thread invité courant lu dans son PCR (r13 ->
  prcb_data.current_thread -> X_KTHREAD.tls_address), sans adaptateur HostToGuestFunction ni recherche du XThread
  par le TLS de l'hôte ; cases >= 32 : chemin d'origine. Validé sur PC : 23,2 millions d'appels, 0 différence.
- 2026-10-02 : **test console n°20 (LTO + KeTlsGetValue direct + bindGpuProgramParameters court + relevé allégé)** :
  ville à ~290-310 dessins, image côté jeu 31,5-34 ms (22-24 images/s) au lieu de 33-34 (22-23) ; relevé 2,2 au lieu
  de 2,6. LTO : gain modeste (~3 % avec le reste). « Return to Game Library » : retour propre au menu, confirmé.
  - Profil : trace::On 1,1 % en temps propre = compteurs d'appels (CountSlot) incrémentés atomiquement à chaque appel
    des 78 méthodes comptées, même sans traceur : comptés seulement avec le traceur désormais.
- 2026-10-02 : **registres PowerPC en variables locales** (options du générateur, comme nfsmw-nx, docs/toolchain.md
  « Registers as C++ locals ») : cr_as_local, xer_as_local, ctr_as_local, non_volatile_as_local, ajoutées par
  switch/gen_manifest.sh dans [entrypoint] du manifeste Switch seulement (le manifeste PC d'origine n'est pas touché).
  Les registres restaient jusqu'ici dans ctx en mémoire ; avec r14-r31 en variables, plus de __savegprlr/__restgprlr.
  - Constat au passage : toutes les lectures et écritures en mémoire du jeu sont volatile dans le code généré
    (REX_LOAD/REX_STORE) : le compilateur ne peut ni supprimer une relecture ni garder une valeur en registre, même
    pour la pile du thread. Piste pour plus tard (accès relatifs à r1 non volatils), à manier avec soin (boucles
    d'attente du jeu sur des variables partagées).
  - Fonctions découpées : switch/tools/registres_lus_avant.py (portage de lee_antes.py) liste les fonctions qui
    lisent r14-r31/f14-f31/cr/ctr avant de les écrire ; 35 morceaux (dont setjmp, sub_82864230, qui sauvegarde tous
    les registres non volatils), puis 5 relais qui transmettent un registre non écrit à une fonction marquée :
    switch/registres_partages.txt (share_registers, fusionné par gen_manifest.sh). Point fixe au second passage.
    Fausses alertes écartées : xer seul (initialisé à zéro, seul le bit SO est lu, non transmis par l'ABI),
    sauvegardes de v64-v127 en prologue. Nos interceptions ne lisent aucun de ces registres (vérifié dans src/).
  - **Premier essai planté** : registres_lus_avant.py (comme lee_antes.py) est textuel ; dans une boucle, une lecture
    placée avant l'écriture dans le texte vient après à l'exécution. 39 fonctions marquées à tort share_registers
    (toutes avec un prologue qui sauvegarde ces registres) : elles travaillaient sur ctx sans restaurer (plus de
    __restgprlr), leur appelante reprenait leurs valeurs (sub_82737F60 rendait son code d'erreur dans r31 ->
    lecture de 0xD4 dans sub_827380E0, thread audio). L'outil écarte désormais les fonctions dont le prologue
    sauvegarde des registres ; seule sub_82864230 reste marquée. PC : du titre à la mine, images justes, image côté
    jeu 4,3 ms au lieu de 5,5-6.
  - **Blocage au démarrage et test faussé** : le journal est remis à zéro à chaque lancement ; gels.sh comparait le
    nombre de bilans avant/après et concluait à tort au blocage dès que le lancement précédent en avait plus. Les
    taux mesurés ces derniers jours étaient faux. Test corrigé : 1 blocage sur 8 sans correctif. Blocage réel
    observé (lecture de /proc/<pid>/mem) : fence courante 0x19, GPU à 5, anneau vide, présentation dans
    sub_821A5C10 : le Direct3D n'envoie le tampon de commandes avant d'attendre que pour la fence courante.
    Correctif `--torchlight_envoi_avant_attente` (vrai) : envoi (sub_821F2910) avant d'attendre une fence non
    atteinte ; 0 blocage sur 8 + 3 (peu de lancements, mais sans danger).
- 2026-10-02 : **accès à la pile sans volatile** (switch/tools/pile_non_volatile.py, après chaque génération,
  appelé par build_switch.sh) : 183 106 accès « r1 + déplacement » passent par REX_LOAD_STACK_* / REX_STORE_STACK_*
  (définies dans l'en-tête précompilé généré) : accès ordinaires par types may_alias/aligned(1) avec
  REX_STACK_NON_VOLATILE (option CMake TORCHLIGHT_PILE_NON_VOLATILE, ON), sinon les macros volatile d'origine.
  Le reste de la mémoire du jeu reste volatile (boucles d'attente entre threads). PC : images justes.
- 2026-10-02 : **Direct3D Xbox, suite** (rendu natif seul) :
  - _render : la lecture d'une surface du périphérique (device+12944 / +12940, sub_821C0A08 / sub_821E0088) avec
    AddRef, aussitôt relâchée (sub_821CEDD8), ne sert qu'à tester qu'elle n'est pas nulle : pendant _render, lue sans
    AddRef et Release de cette même surface sauté (2 lwarx/stwcx. émulés sous le verrou global, ~0,9 % sur la
    console).
  - Coupés en rendu natif seul (TL_TRACE_D3D ; aucune écriture hors de la pile dans le portage Xbox, rien de
    mémorisé dans Ogre 1.7) : _setDepthBufferParams, _setDepthBufferCheckEnabled / WriteEnabled / Function,
    _setColourBufferWriteEnabled, setScissorTest, clearFrameBuffer, setVertexDeclaration. Gardés :
    _setCullingMode (mCullingMode), setVertexBufferBinding (recopie paresseuse des tampons ; SetStreamSource,
    sub_821C3D58, gère des références : laissé).
  PC : images justes du titre à la mine, aucun accès fautif.
- 2026-10-02 : **test console n°21 (registres en variables locales, pile sans volatile, envoi avant attente,
  Direct3D allégé ; LTO)** : ville à ~290-313 dessins, image côté jeu 25-26,4 ms (28-29 images/s) au lieu de
  31,5-34 (22-24) : environ -22 %. « Return to Game Library » toujours propre. Code de 94,4 à 81,2 Mo.
  Bilan de la journée en ville, à 1020 MHz, rendu inchangé : 44,5 ms (17 images/s) -> ~25,5 ms (28-29 images/s).
  Profil : toujours plat (natif::Render 2,1 % en tête, memcpy 2 %, renderSingleObject 1,9 %).
- 2026-10-02 : **skip_lr et non_argument_as_local** (rejetées par nfsmw-nx, raisons absentes ici) :
  - skip_lr : plus d'écriture de ctx.lr à chaque appel ni de lecture par mflr. 34 750 mflr de prologue (r12, sans
    effet) ; 7 autres lisent vraiment le registre de lien : 5 le sauvegardent dans r31 et le restaurent (mtlr),
    setjmp et longjmp (sub_82864230, sub_828636E0, déjà inutilisables avec les registres en variables locales). Nos
    interceptions ne lisent ctx.lr que pour les journaux du traceur ; le runtime : XexGetProcedureAddress (appelant,
    pour information) et le rapport de plantage (registre de lien périmé).
  - non_argument_as_local (r0, r2, r11, r12, f0, v32-v63 en variables) : l'obstacle de nfsmw-nx (__savevmx qui
    reçoit r12) a disparu, plus aucun appel à __savevmx/__savefpr. registres_lus_avant.py étendu à ces registres :
    r12 lu avant d'être écrit = sauvegarde du registre de lien (mflr vide avec skip_lr), sans effet ; 17 + 2
    fonctions lisent r12/r11 de leur propriétaire : des gestionnaires d'exception (funclets), appelés par aucun code
    (seulement dans la table des appels indirects ; les exceptions ne sont pas émulées).
  PC : images justes, image côté jeu 3,85 ms (4,3 avec les seuls registres cr/xer/ctr/r14-r31, avant pile et
  Direct3D).
- 2026-10-02 : **blocage au démarrage RÉSOLU** (connu depuis le 2026-10-01 : ~1 lancement sur 3 sur PC, présentation
  qui attend une fence, sub_821F31C8 -> sub_821A5C10 -> sub_82774170).
  - Cause : course dans l'émulateur du GPU (SDK, command_processor.cpp). Le jeu réinitialise l'anneau de commandes
    (VdInitializeRingBuffer) pendant le démarrage ; CommandProcessor::InitializeRingBuffer, appelée depuis le thread
    du jeu, remettait read_ptr_index_ à 0 pendant que le thread du GPU exécutait l'anneau : « read_ptr_index_ =
    ExecutePrimaryBuffer(...) » écrasait la remise à zéro par l'ancienne position (31). Le jeu repartait de 0
    (pointeur d'écriture 22, 25, 28, 31…), le GPU restait à 31 : ses commandes suivantes n'étaient jamais exécutées,
    la fence attendue jamais atteinte (fence courante 0x19, GPU à 5).
  - Correctif (nfsmw-sdk.patch) : drapeau atomique ring_reset_pending_ posé par InitializeRingBuffer (avant de remettre
    le pointeur d'écriture à zéro), appliqué par le thread du GPU (seul à écrire read_ptr_index_) en début de boucle,
    avant et après chaque exécution. PC : 18 lancements sur 18 (3 blocages sur 8 juste avant).
  - Piège qui a faussé toute l'après-midi : le journal est tantôt remis à zéro, tantôt complété au lancement ; les
    tests qui comptaient les bilans par numéro de ligne voyaient des blocages qui n'en étaient pas, et les
    conclusions sur la pile sans volatile et sur skip_lr / non_argument_as_local sont à refaire. Test fiable : journal
    supprimé avant chaque lancement et lu après la fin du processus. Autre piège : le greffon GPU chargé est la
    copie switch/out-pc/librexgpu-xenos.so, rafraîchie seulement quand l'exécutable est réédité (une modification du
    SDK seule n'y arrive pas : recopier à la main).
  - L'essai « envoi du tampon avant d'attendre » (--torchlight_envoi_avant_attente) reposait sur une mauvaise
    hypothèse : désactivé par défaut.
- 2026-10-02 : **sommets des billboards en natif** (sub_821DAB40, BillboardSet::genVertices, src/torchlight_ogre_natif.cpp).
  Trois chemins réécrits : sans rotation, rotation des coordonnées de texture (BBR_TEXCOORD), rotation des sommets
  (BBR_VERTEX : ~85 % des appels, normale normalisée + matrice angle-axe de sub_821DBB20). cos/sin : fonctions
  recompilées du jeu (approximations maison) ; fmadds/fmsubs = float(fma en double) comme le code recompilé ; pas de
  contraction fma du compilateur (#pragma). Seul le rendu en points reste à l'original (jamais vu). Validation
  (--torchlight_ogre_natif=2) jusqu'à la mine : ~107 000 billboards par 10 s, 100 % réécrits, 0 différence au bit
  près. Piège : table de constantes du jeu en 0x820AA2C0 (lis -32245 = 0x820B0000, pas 0x8209…).
- 2026-10-02 : **animation des squelettes, cœur de Node en natif** (src/torchlight_ogre_natif.cpp) : Node::_update,
  _updateFromParent, updateFromParentImpl (quaternions, Quaternion * Vector3 de sub_821C9BF8), accesseurs dérivés,
  needUpdate (Node et Bone ; ensemble mChildrenToUpdate vide vidé en natif), NodeAnimationTrack::applyToNode (nlerp,
  rotation locale, translation, échelle ; getInterpolatedKeyFrame et Slerp restent ceux du jeu). Dévirtualisation :
  appel natif si la case de la table virtuelle est la fonction de Node, sinon la fonction du jeu (SceneNode, TagPoint).
  Validation (mode 2) : updateFromParentImpl ~30 000 appels/s et applyToNode ~24 000/s (état du nœud rejoué), 0
  différence. Piège : appels_directs.py avait déjà changé les appels directs à ces fonctions en __imp__ (sans
  interception) : après toute nouvelle interception, remettre sub_X dans le code généré (ou --annuler puis relancer,
  mais tout se recompile).
- 2026-10-02 : **pile sans volatile réactivée** (TORCHLIGHT_PILE_NON_VOLATILE de nouveau ON par défaut) : sur PC,
  10 lancements sur 10 sans blocage après le correctif de la course de l'anneau (les blocages qui l'avaient fait
  désactiver venaient de cette course). Les 690 fonctions à boucle sans appel qui lit la pile restent volatile.
  skip_lr / non_argument_as_local : à retester de même.
- 2026-10-02 : **matrices des os en natif** : Skeleton::_getBoneMatrices (sub_821CB3C0), Bone::_getOffsetTransform
  (sub_821C9A18 ; quaternion -> matrice sub_821CB318, produit 3x3 sub_821CB110, makeTransform sub_821CB210).
  Validation : ~30 000 matrices/s, 0 différence.
- 2026-10-02 : **getInterpolatedKeyFrame en natif** (sub_821CBDC0) : t nul et interpolation linéaire (nlerp natif,
  slerp du jeu), accesseurs de TransformKeyFrame dévirtualisés ; getKeyFramesAtTime, écouteur et splines restent
  ceux du jeu. Appelée directement par applyToNode natif. Validation : ~23 000 appels/s, 0 différence.
- 2026-10-02 : **test console n°22** (billboards, nœuds, applyToNode, images clés, matrices des os, pile sans
  volatile, correctif de la course ; LTO) **aux fréquences de base de la RAM et du GPU** (tests n°1 à 21 : RAM et GPU
  surcadencés, CPU à 1020 MHz) : ville ~285 dessins, image côté jeu ~28,7 ms (27 images/s), relevé 2,5 ms. Pas
  comparable au n°21 (25,5 ms) : tout le code non modifié ralentit de ~25 % (natif::Render 1,14 -> 1,66 ms, memcpy
  0,51 -> 0,67, octree 4,0 -> 5,0), alors que le squelette baisse en valeur absolue (Skeleton[63] 1,61 -> 1,02 ms,
  _update + needUpdate 1,21 -> ~0, matrices des os 0,58 -> 0,47). Stable, rendu correct, sortie propre.
  Piège : noter les fréquences (CPU, RAM, GPU) de chaque test ; profil de la console : les ~30 premiers % des
  échantillons peuvent être le chargement (découper par tranches de temps).
- 2026-10-02 : **relevé allégé pour la RAM d'origine** (profil du test n°22 : notre code ~2,8 ms par image en ville,
  dispersion des accès mémoire plus que calcul) :
  - géométrie reprise : table plate à adressage ouvert (GeometryMap) au lieu d'une unordered_map (CopyRange 0,46 ms) ;
  - liaison de programme : empreinte et type dans une table plate par thread (LookupProgram) au lieu d'une
    unordered_map + vérification des pages à chaque liaison (HeapPtr 0,19 + ProgramHash 0,15 ms) ;
  - trace::HeapPtr par la table des pages vérifiées (invite::Ptr) au lieu de LookupHeap/QueryRangeAccess ;
  - invite::Readable : chemin rapide intégré (table de bits d'abord, kernel_memory() et le reste hors ligne) ;
  - trace::On intégrée (0,15 ms), OwnThread par un booléen par thread au lieu de get_id() (pthread_self) ;
  - billboards : plus de copie du PPCContext (plusieurs Ko) pour chaque appel du jeu (memcpy 0,13 ms).
  PC (mine) : relevé ~0,23 ms au lieu de 0,30-0,33 au mieux aujourd'hui (charge de la machine variable). À mesurer
  sur la console.
- 2026-10-02 : **concatenateAffineMatrices en natif** (sub_8219DF38, OptimisedUtilGeneral [3] : matrices des os des
  entités × monde à chaque image, 1,1 % du thread en ville) ; recouvrements (dont en place) laissés à l'original.
  Validation : 0 différence. Recherche des objets visibles (sub_821A6070, 18 % du thread, ~5,3 ms en ville, RAM
  d'origine) : particules ParticleUniverse 4,6 % (presque tout dans genVertices, déjà natif), géométrie statique
  2,7 % (RenderQueue::addRenderable sub_821C3730 : appels virtuels, map des groupes), test des boîtes 2,2 % (natif,
  latence mémoire), entités 2,1 %, parcours lui-même 2 %.
- 2026-10-02 : **_updateAutoParams en natif** (sub_82201EB0, GpuProgramParameters) : le jeu n'utilise que 10 types
  de paramètres automatiques (relevé sur PC : 0, 1, 3, 4, 6, 14, 21, 22, 66, 122) parmi les ~126 du switch : boucle
  et écritures dans mFloatConstants en natif (transposition si +98, tableau 3x4 des os, Vector4), accesseurs de
  la source de données du jeu ; tout autre type présent : l'original. Appel direct depuis
  updateGpuProgramParameters (torchlight_recomp.108.cpp) remis sur sub_X (appels_directs.py l'avait figé en
  __imp__). Validation : tampon des constantes comparé, ~10 000 appels/s, 0 différence.
- 2026-10-02 : **clavier virtuel sur la console** (SDK, xam_ui.cpp, nfsmw-sdk.patch) : XamShowKeyboardUI ouvre
  l'applet swkbd de libnx (titre et description du jeu, texte par défaut, longueur maximale du tampon) sur le
  thread de répartition du noyau (xeXamDispatchHeadlessEx : l'applet bloque jusqu'à la validation) ; annulation
  (B) ou échec : X_ERROR_CANCELLED comme la boîte ImGui du PC. Avant : boîte ImGui inutilisable sans clavier.
  Demande le mode application (title override), comme le reste.
- 2026-10-02 : **lectures de fichiers (chargements)** : profil de la console (test n°22) : ~17 % du thread
  principal pendant le chargement dans pread (HostPathFile::RellenarVentana), ~5,5 s. Trace des lectures sur PC
  (jusqu'à la mine) : 50 592 lectures de pak.zip (140 Mo, toutes les données), médiane 46 octets, 32 Ko au plus,
  27,7 Mo demandés dont 15,6 distincts. La fenêtre de 256 Ko de nfsmw-nx se rechargeait à chaque saut : 601
  lectures, 157 Mo lus sur la SD. Simulation (modèle 1 ms + 25 Mo/s calé sur la console) : fenêtre 256 Ko ~6,9 s,
  fenêtre 64 Ko ~3,9 s, blocs de 64 Ko LRU 32 Mo ~1,1 s (317 lectures, 20,8 Mo), fichier entier ~5,6 s.
  Fait (SDK, nfsmw-sdk.patch) : taille de bloc du cache par blocs réglable (nfsmw_io_cache_bloque_kb, 256 par
  défaut) et verrou relâché pendant la lecture sur la SD (défaut qui avait fait abandonner ce cache à nfsmw-nx) ;
  switch/torchlight.toml : fenêtre désactivée, cache 32 Mo, blocs 64 Ko. PC : 17,3 Mo lus en 280 lectures,
  99,4 % des lectures servies depuis la RAM, 0 expulsion. À mesurer sur la console (durée des chargements).
- 2026-10-02 : **skip_lr + non_argument_as_local abandonnées** (retest après le correctif de la course, dans un
  git worktree à part pour ne pas toucher au code généré de la build PGO) : le jeu se fige ~1 s après le démarrage
  (après TRANSLATION.DAT), écran noir à chaque lancement ; gdb : le thread principal et deux autres threads du jeu
  attendent dans RtlEnterCriticalSection une section critique que personne ne tient (jamais relâchée) : code
  généré faux dans au moins une fonction (candidats : lecture du registre de lien par mflr hors prologue, setjmp /
  longjmp). Bissection (une génération + compilation par essai) jugée trop chère pour un gain incertain.
- 2026-10-02 : clavier virtuel (swkbd) vérifié sur la console (création du personnage, nom du familier).
- 2026-10-02 : **session PGO** (30 min sur la console, build instrumentée sans LTO, switch/out-pgo) : 234 profils
  .gcda (219 du code généré), 9 vidages de ~22 s (écriture sur la SD). Cache des lectures pendant la session (32 Mo) :
  98,2 % servies depuis la RAM, 145 Mo livrés pour 86 Mo lus sur la SD, cache plein (872 blocs expulsés) : passé
  à 64 Mo (cache de plages coupé, inutile pour Torchlight). Build finale : OUT=switch/out-lto
  CMAKE_OPTIONS="-DTORCHLIGHT_LTO=ON -DTORCHLIGHT_PILE_NON_VOLATILE=ON -DTORCHLIGHT_PGO=utiliser". Le profil vaut
  pour ce code généré exact : toute régénération (gen_manifest, codegen) demande une nouvelle session.
  Ordre des fonctions : GCC met les fonctions chaudes du PGO en .text.hot.<nom> ; ordre_fonctions.py liste les deux
  noms. Piège : copier rex_pilas.txt (échantillons bruts) avant le `journaux` suivant, sinon l'ordre des fonctions
  ne peut plus être régénéré (ceux du test n°22 ont été perdus).
- 2026-10-02 : **appuis fantômes sur A** (sélection du personnage validée toute seule, cinématique de la mine
  coupée) : les pilotes de manette du SDK (Switch et SDL, hérités de Xenia) fabriquent les frappes
  (XInputGetKeystroke) en comparant l'état des boutons au dernier état transmis ; la répétition d'une touche
  maintenue était traitée avant, sans vérifier que la touche était encore enfoncée. Touche relâchée pendant un
  écran qui ne lit pas les frappes : au premier appel suivant, appui répété fantôme. Correctif (nfsmw-sdk.patch) :
  répétition annulée si la touche n'est plus enfoncée (le relâchement est alors signalé normalement).
- 2026-10-02 : **test console n°23** (LTO + PGO, réécritures du soir, cache des lectures ; fréquences de base, CPU
  1020 / GPU 307,2 / mémoire 1600 MHz) : ville ~300 dessins avec le profil des piles 25,0 ms, 29-30 images/s
  (test n°22 : 28,7 ms, 27) ; sans le profil 21,7-22,5 ms, 31-35 images/s ; mine 17-19 ms, 39-47 images/s ;
  relevé 1,6-1,9 ms (2,5). Lectures au chargement : 65 échantillons du profil au lieu de 148 ; cache : 21 Mo lus
  sur la SD en 341 appels, 99,3 % servies depuis la RAM. Profil (ville) : rendu d'Ogre ~18,5 ms dont ~6,75 ms de
  cibles de rendu secondaires (sub_82198608 : ombres / reflets), logique ~6,25 ms, parcours de l'octree 1,1 ms en
  propre. À voir : ~7 ms par image hors de la mesure « image » (35 images/s pour 22 ms).
- 2026-10-02 : appuis fantômes sur A : correctif vérifié sur la console (sélection du personnage normale).
- 2026-10-02 : **suppression d'un personnage sans effet** : la sauvegarde est un seul conteneur (torchlight.sav :
  sharedstash.bin, 0.tsv, 1.tsv… un fichier par personnage, save.tmp, backup.tmp). Supprimer un personnage =
  DeleteFile sur <n>.tsv, c'est-à-dire NtSetInformationFile(FileDispositionInformation) : le SDK posait le drapeau
  delete_on_close sur l'entrée sans jamais l'appliquer (NtDeleteFile n'est pas importé par le jeu). Correctif
  (nfsmw-sdk.patch, xfile.cpp) : à la destruction de l'XFile, après la fermeture du fichier hôte (un fichier
  ouvert ne peut pas être supprimé sur la console), Entry::Delete(), avec une ligne de journal. Aussi :
  ContentManager (XamContentDelete, copie de repli des sauvegardes) supprime les dossiers par ListFiles
  (opendir/readdir) au lieu de std::filesystem::remove_all, qui liste mal les dossiers sur Horizon.
- 2026-10-02 (soir) : **parcours de l'octree, mesures avant réécriture** (walkOctree sub_821C7158, version Xbox
  itérative : pile explicite en 0x83582C00, drapeau « octant entièrement visible » dans le bit 0 ; visibilité des
  octants sub_821C7000 + sub_821C0E78 (6 plans par getFrustumPlane, case 364) ; par nœud BoxVisible (natif) si
  octant partiel ; par objet isVisible [26], _notifyCurrentCamera [18], _updateRenderQueue [23]). Ville : ~3,95 ms
  dont ~2,6 ms dans les _updateRenderQueue des objets (inchangés par une réécriture), ~1,1 ms propre, ~0,3 ms de
  visibilité des octants : gain réaliste ~0,5 ms. Piège à reproduire : r18 (visible) n'est recalculé que pour les
  octants partiels (valeur précédente gardée dans les octants complets). Textures de rendu (diagnostic PC) : 207
  ombres 5 dessins, 208 lumière 40 dessins ; la mise à jour des contrôleurs (sub_821C8C00, appelée directement
  depuis torchlight_recomp.214.cpp) se fait pendant la première cible rendue (207). Non commencé.
- **Bilan de la session du 2026-10-02** (aux fréquences de base) : ville 28,7 ms (test n°22) -> 21,7-25 ms (test n°23,
  LTO + PGO), 31-35 images/s ; mine 39-47 images/s ; chargements : lectures 2,3 fois plus courtes. Corrigés : blocage
  au démarrage (course de l'anneau, SDK), appuis fantômes (pilotes de manette), clavier virtuel (swkbd),
  suppression d'un personnage (à vérifier). Tâches ouvertes : CLAUDE.md, « Problèmes ouverts et tâches ».
- 2026-10-03 : suppression d'un personnage vérifiée sur la console. Boutons ABXY laissés tels quels (choix de
  l'utilisateur).
- 2026-10-03 : **installation depuis le paquet XBLA** (src/torchlight_installation.cpp, PC et Switch, sans toucher
  au code généré : le profil PGO reste valable) : ReXApp::OnFinalizePaths peut renvoyer std::nullopt et appeler
  resume plus tard ; une fenêtre ImGui (ImGuiDialog, manette et écran tactile gérés par le SDK) s'affiche avant la
  construction du runtime, l'extraction se fait dans un std::thread créé depuis le thread de l'interface. Le paquet
  de l'utilisateur (206 135 296 octets) contient exactement les 57 fichiers de assets/ (204,7 Mo). Lecture : blocs
  de chaque StfsContainerEntry::block_list() regroupés quand ils sont contigus, lectures de 1 Mo ; SHA-256 calculé
  pendant l'écriture (libnx sur la console, instructions ARMv8 ; implémentation portable sur PC), fichier écrit en
  .part puis renommé. Tests PC : installation complète (1,3 s, fichiers identiques à assets/), relance (pas de
  fenêtre), paquet tronqué, paquet abîmé (un octet changé : pak.zip refusé, pas de .part laissé), dossier vide,
  reprise (fichier modifié de même taille réécrit, fichier de mauvaise taille réécrit, .part nettoyé), « Garder » et
  « Supprimer le paquet » (dossiers 58410A7E/000D0000 vides supprimés). Pièges : AvatarAwards est aussi un
  conteneur (PIRS) : filtre sur le type de contenu 0x000D0000 ; StfsContainerDevice::Initialize plante sur un
  paquet tronqué : taille contrôlée avant avec ReadPackageHeader.
- 2026-10-03 : installation, premier essai sur la console : plantage immédiat (std::terminate, std::system_error
  dans Demarrer) : std::thread créé depuis le thread de l'interface échoue aussi sur la console. Remplacé par
  pthread_create (pile de 1 Mo), comme les threads du rendu natif ; message d'erreur au lieu d'un plantage si la
  création échoue.
- 2026-10-03 : **installation vérifiée sur la console** (sdmc:/switch/torchlight avec seulement le .nro, le .toml et
  le paquet) : identification 0,4 s, contrôles 12,2 s (un fseeko(SEEK_END) par bloc pour le contrôle de troncature,
  ~50 000 appels : corrigé, taille lue une fois par fichier), extraction et SHA-256 des 195 Mo en 21,9 s
  (~9 Mo/s, lecture et écriture de la SD en série), paquet supprimé, jeu démarré normalement ensuite.
- 2026-10-03 : **test de la version joueurs sur une autre console** (mode TV : 720p mis à l'échelle, satisfaisant ;
  veille de ~4 h et bouton HOME : OK). Bug : au retour en ville (mode TV), une partie de la ville reste noire, puis
  après un aller-retour mine-ville la console entière se fige (redémarrage forcé). Journal (switch-logs/,
  torchlight_001.log, ~15 min de jeu en tout) : à 16:26:27, dix échecs `AllocFixed: no se pudo confirmar 0x2000 à
  0x27000 bytes ... Fallo 0x0000CE01; respaldo 393-395 MB, mapeado 452-454 MB; 1128-1161 trozos`, puis
  `PhysicalHeap::Alloc unable to alloc physical memory` : allocations physiques du jeu refusées (textures et
  tampons absents : ville noire). 0xCE01 = noyau, OutOfResource : plus de blocs mémoire du noyau (KMemoryBlock),
  pas un manque de RAM. Déjà vu (granule de confirmation porté de 4 Ko à 64 Ko, guest_memory_switch.cpp) ; ici,
  le tas physique du jeu distribue de petits blocs contigus de haut en bas (adresses des échecs), chacun devenu un
  morceau (trozo) séparé : morceau de sauvegarde + alias de code + une projection par vue, plus les découpes du
  tas. Hypothèse pour le gel de toute la console : sans ressource système propre (SystemResourceSize du NPDM du
  titre remplacé : le homebrew tourne dans son processus), les blocs viennent d'une réserve partagée avec le
  système ; la console principale (autre titre remplacé ?) n'a jamais eu le problème. Dernières lignes du journal :
  avertissements NVK « channel 2 hit native submission pressure » en série.
  Correctif (nfsmw-sdk.patch) : granule de confirmation de 2 Mo (kBlockSize) au lieu de 64 Ko : au plus 256
  morceaux pour les 512 Mo du 360, descripteurs de blocs du noyau ; coût : au plus 2 Mo par plage confirmée.
  Diagnostic ajouté au profil (rex_perfil.log) : programme du processus (titre remplacé) en tête, et toutes les
  10 s « nucleo: N bloques de memoria (heap, sombras, vistas, otros) | recurso de sistema del proceso X/Y KB »
  (svcQueryMemory sur tout l'espace d'adressage, InfoType_SystemResourceSize*). À vérifier sur la console :
  nombre de bloques et de trozos après plusieurs allers-retours ville-mine, en mode TV.
- 2026-10-03 : la session suivante de l'autre console tournait encore avec l'ancienne build (build de dev de 09:01,
  switch/out/sd, pas celle des joueurs) : mêmes échecs 0xCE01 à 19:11 (1103-1114 trozos, 405 Mo), sans le mode TV.
  Journaux dans switch-logs-archive/20261003-1907-ancienne-build/. **Avec le granule de 2 Mo** (build des joueurs,
  MD5 bbb0cd41, ~8 min, 5 chargements ville/mine ; switch-logs-archive/20261003-nouvelle-build/) : aucun 0xCE01,
  148-164 trozos pour 384-416 Mo, ~1950 blocs du noyau au total (heap ~1290 : découpes du tas par les allocations
  Vulkan de NVK, une par texture, et les sauvegardes des morceaux ; ombres 164 ; vues 249 ; autres 271).
  Processus du titre 0100073011382000 (Fitness Boxing 2) : ressource système 0/0 Ko, les blocs viennent bien de la
  réserve partagée avec le système (d'où le gel de toute la console). Prochain consommateur si besoin : une
  allocation Vulkan par texture (rendu.cpp, sans sous-allocation). Ville affichée correctement (confirmé par
  l'utilisateur). Console V1 Erista, mémoire à 1331 MHz : ville 25-29 images/s (31-35 sur la console principale).
- 2026-10-03 : **livrable vérifié sur l'autre console** (zip des joueurs de 20:00, .nro MD5 3e8f995a, sans chemins de
  la machine de build) : lancement depuis l'album -> nouveau message du mode applet ; session en title override
  (Fitness Boxing 2) avec retours en ville : aucune zone noire, aucun 0xCE01, au plus 160 trozos et 2078 blocs du
  noyau, aucune erreur (switch-logs-archive/20261003-livrable/).
- 2026-10-03 : démarrage sans torchlight.toml vérifié sur la console (valeurs intégrées au .nro appliquées :
  mesa_env, rendu natif, cache des lectures de 64 Mo par blocs de 64 Ko, journaux krnl au niveau error).
- 2026-10-03/04 : gels de 5-6 s sur la console XJE1 (thread principal à 100 %, rendu inactif, ni E/S ni mémoire) ;
  non reproduits en 74 min avec le relevé des piles (switch-logs-archive/20261003-2203-gels/, 20261004-0908-longue/).
  Particules roses du sort de protection : effet normal (émulation et rendu natif hors ligne identiques sur PC).
- 2026-10-11 : **plantage « device lost » sur la console** (XJE1, build 1.0.0, session 014 ;
  switch-logs-archive/20261011-plantage-device-lost/). Scène lourde (463 dessins/image, 20 Mo envoyés par image),
  « channel 2 hit native submission pressure » en rafale (fréquent : 500 à 2800 par session, sans conséquence
  d'habitude), puis « channel submit failed: no queue space » -> vkQueueSubmit DEVICE_LOST -> abort() du SDK
  (VulkanCommandProcessor::BeginSubmission -> CheckSubmissionFenceAndDeviceLoss, thread GPU Commands).
  Cause dans NVK (mesa-switch, nouveau_horizon_channel.c) : nouveau_horizon_channel_exec et enqueue_waits ne
  réservent dans la GPFIFO (0x800 entrées) que leurs entrées + FINAL_SKID, pas la fin ajoutée par submit_locked
  (1-2 entrées + 1 pour l'attente de l'ordre total) ; file remplie à 2042-2043 entrées -> la soumission suivante
  échoue en NO_SPACE, sans reprise (exec_submit, lui, réservait déjà cette fin). Correctif
  (switch/patches/mesa-switch-gpfifo-reserve.patch) : réserver cette fin aussi dans exec, enqueue_waits et
  full_barrier ; NO_SPACE arrive alors à la mise en file, où nvkmd_switch_ctx_exec soumet le lot et réessaie.
  Ancienne libvulkan.a gardée dans ~/workspace/mesa-switch/nvk-sdk-sauvegarde-20261011/.
- 2026-10-11 : correctif de la GPFIFO vérifié sur la console par l'utilisateur (session sans problème).
- 2026-10-13 : **texture du poisson de la pêche couverte de bruit** (journaux et capture d'un autre joueur, user-logs/,
  non versionné ; rien dans les journaux : aucune texture ignorée). Poisson au bout de la ligne : media/models/fish/
  fish.dds, DXT1 32x16. Xenos : côté le plus court <= 16 texels avec mips compactées -> le niveau de base est dans le
  bloc des mips compactées, décalé (GetPackedMipOffset du SDK, mip 0 : 32x16 -> 16 texels en Y, 16x16 -> 16 en X).
  Le rendu natif et la capture lisaient le niveau de base à partir de (0, 0). Correctif : décalage ajouté aux
  coordonnées (torchlight_natif.cpp, torchlight_capture.cpp). Validation sur PC (capture pendant le sort de
  protection, sauvegarde de la console) : avant, SPUNK.DDS (particule 16x16 DXT1) différait du niveau 0 du DDS de
  pak.zip ; après, les 65 textures DXT de l'image sont identiques à l'octet à leur DDS, et le chargeur du rendu
  natif donne la même empreinte que la capture. Comparaison : switch/tools/textures_dds.py <capture> assets/pak.zip
  (chaque tex_*.bin DXT de frame.txt contre le niveau 0 du DDS de même nom).
- 2026-10-13 : **langue du jeu d'après celle de la console.** Le jeu n'appelle que XGetLanguage (langue du tableau de
  bord ; resources.cfg : « Only the localized zip for the current dashboard language will be searched ») ; le SDK
  renvoyait toujours l'anglais. SDK (nfsmw-sdk.patch, xam_info.cpp) : XGetLanguage renvoie le réglage user_language
  (déjà existant, 1 = anglais par défaut). Switch (torchlight_app.h, OnPostInitLogging) : SetDefault de
  user_language d'après setGetSystemLanguage (fr et fr-CA -> 4, de -> 3, es et es-419 -> 5, le reste -> 1 : le jeu n'a
  que translations/{de,fr,es}) ; torchlight.toml peut le fixer (user_language = 4). Vérifié sur PC avec
  --user_language=3/4/5 : menus en allemand, français, espagnol (fichiers lus dans game:\translations\<langue>\).
- 2026-10-13 : langue vérifiée sur la console (« langue de la console 2 -> langue du jeu 4 », menus et jeu en
  français). **Écran des succès** (entrée du menu principal) : XamShowAchievementsUI ouvre une fenêtre ImGui du SDK
  (liste de l'AchievementManager, noms traduits) ; validé sur PC (captures par xwd de la fenêtre : les captures du
  rendu natif n'ont pas les fenêtres ImGui ; la manette de script n'agit que sur le jeu, pas sur ImGui).
  **Fond du menu d'après le dernier personnage dès le démarrage** : src/torchlight_menu_fond.cpp (voir CLAUDE.md) ;
  validé sur PC avec la sauvegarde de la console (mines à l'écran « Begin », puis « Continuer » sans erreur).
  Au passage : en-têtes précompilés du SDK (switch/out-pc) périmés après une mise à jour des en-têtes système
  (« has been modified since the precompiled header ») : supprimer les cmake_pch.hxx.pch.
- 2026-10-13 : écran des succès et fond du menu **vérifiés sur la console** (XJE1) : liste affichée, défilement et
  fermeture par B ; menu_dernier_personnage.txt = SAVE:\2.TSV, puis au démarrage suivant « fond du démarrage
  d'après SAVE:\2.TSV : trouvé », « Continuer » monte ensuite la vraie sauvegarde normalement.
- 2026-10-13 : **essai des limites d'intégration du LTO** (idée de sa2-nx 1.0.1, docs/cpu-rounds.md de ce projet) :
  --param=max-inline-insns-auto=64 et inline-unit-growth=12 sur torchlight_recomp, avec LTO et profil
  (TORCHLIGHT_INLINE_AUTO, TORCHLIGHT_INLINE_UNIT_GROWTH dans switch/CMakeLists.txt ; vides = valeurs de GCC). Même
  profil PGO. ELF : .text 59,26 -> 62,24 Mo (+5 %), .nro 76,6 -> 79,6 Mo, bl 651 017 -> 657 237 (compte statique,
  sans valeur pour le gain). Essai : dist/torchlight-essai-inline64.nro (MD5 3f9dd0c6), ELF
  switch/elf/torchlight-essai-inline64-20261013.elf. À mesurer sur la console contre la 1.0.1, même console, mêmes
  fréquences, même endroit. Autres pistes de sa2-nx retenues pour plus tard : appels indirects promus (17 220 bctrl),
  -fomit-frame-pointer (55 725 cadres ; casserait la pile de rex_crash.log et du profileur), -fno-math-errno.
- 2026-10-14 : **essai des limites d'intégration abandonné.** Console XJE1, CPU 1020 / GPU 307,2 / mémoire 2400
  pendant le jeu dans les deux sessions (le profileur lit 1600 au menu puis 2400 en jeu : profil de sys-clk ?).
  Temps « image » : ville, essai 20,5-21,6 ms (243-284 dessins) contre 1.0.1 18,8-19,4 ms (269-277) ; mine, essai
  16,9-18,3 ms (263-279) contre 14,7-16,9 ms (222-228), non comparable. Aucun gain, plutôt une perte en ville
  (code +5 % : cache d'instructions ?). Options gardées, vides par défaut. Journaux :
  switch-logs-archive/20261014-essai-inline64/ et 20261014-reference-101/.
- 2026-10-14 : **appels indirects promus en appels directs gardés** (idée et outils de sa2-nx, GPL-3.0, adaptés) :
  switch/tools/appels_indirects.py (marquage des 17 220 sites bctrl, promotion d'après une liste ; lancé par
  build_switch.sh AVANT appels_directs.py), src/torchlight_appels_indirects.cpp (PC : relevé par site, option
  torchlight_releve_appels_indirects=<fichier> ; vérification de chaque appel promu contre la table de
  répartition), switch/tools/appels_indirects_promus.py (liste switch/appels_indirects_promus.txt).
  Relevé PC (deux parties de 2 min dans les mines, sauvegardes console et PC) : ~4 millions d'appels indirects par
  seconde (~70 000 par image, trois fois SA2), 4 760 sites, 94,8 % vers la cible la plus fréquente du site.
  381 sites promus (26 à deux ou trois cibles) prennent 90,2 % des appels ; les plus chauds : petites fonctions
  d'Ogre (sub_821C4798 -> sub_821C47F0 : 4 % de tous les appels). PC : 391 millions d'appels promus vérifiés,
  0 différence. Le code de 122 fichiers change : nouveau profil PGO nécessaire (build instrumentée switch/out-pgo
  avec -DTORCHLIGHT_OUTILS=OFF, comme la build des joueurs).
- 2026-10-14 : **appels indirects promus, mesurés sur la console : -13 % en ville, -15 % dans la mine.** Nouveau
  profil PGO (session de 24 min, 7 vidages, pgo/ ; build instrumentée -DTORCHLIGHT_OUTILS=OFF), build des joueurs
  sans avertissement d'incompatibilité du profil (dist/torchlight-essai-indirects.nro, MD5 77b7262b, ELF
  switch/elf/torchlight-essai-indirects-20261014-0945.elf ; .text 59,26 Mo comme la 1.0.1). A/B sur XJE1, CPU
  1020 / GPU 307,2 / mémoire 2400 (profileur, les deux sessions), personnage immobile aux mêmes endroits :
  ville 277-281 dessins, 1.0.1 19,4 ms (39 images/s) contre 16,9 ms (44-45) ; mine 151-155 dessins, 1.0.1
  12,7-13,2 ms (50-51 images/s) contre 10,8 ms (59, plafond de 60). Thread de rendu inchangé (8,7-9 / 5,5 ms).
  Journaux : switch-logs-archive/20261014-essai-indirects/, 20261014-reference-101-immobile/, 20261014-session-pgo/.
  Piège de manipulation : ne jamais supprimer le .nro de la console avant d'avoir vérifié le fichier de
  remplacement (le dossier temporaire du zip extrait avait disparu entre deux tours).
- 2026-10-14 : **-fno-math-errno** (code du jeu et le nôtre, TORCHLIGHT_SANS_MATH_ERRNO) : 2 instructions de moins
  par racine carrée exécutée, -0,3 % d'instructions dans un fichier à 12 racines carrées. Piège : l'en-tête
  précompilé du code généré impose ses options ; mesuré sans .gch. Option aussi sur notre code : avec le LTO, GCC
  n'intègre pas entre fonctions qui diffèrent sur -fmath-errno. **-fomit-frame-pointer** mesuré (-0,85 à -1 %
  d'instructions sur trois fichiers) mais non appliqué : casserait la chaîne x29 du profileur et du rapport de
  plantage.
- 2026-10-14 : **constantes de .rdata repliées** (switch/tools/constantes_lecture_seule.py, d'après ro_constants.py
  de sa2-nx ; image : switch/tools/image_jeu.py -> switch/out-pc/image_jeu.bin, jamais versionnée ; lancé par
  build_switch.sh avant les appels indirects, sauté si l'image manque). 5 757 chargements flottants, 907 doubles,
  302 entiers (adresse construite par lis/addi/ori sur une ligne droite, dans .rdata 82000400-82148BC0 ; vectoriels
  non traités). PC : 94 millions de chargements vérifiés contre la mémoire en 2 min, 0 différence. Switch (fichier
  100, 28 replis) : -1,2 % d'instructions, -3,7 % de chargements. Code de 218 fichiers changé : nouveau profil PGO
  à faire avant de mesurer sur la console (à regrouper avec les étapes suivantes).
- 2026-10-14 : **chargements flottants (lfs) inversés dans l'unité vectorielle** (switch/tools/chargements_flottants.py,
  d'après float_loads.py de sa2-nx ; build_switch.sh, après les constantes) : ldr s + rev32 au lieu de ldr w + rev +
  fmov s, w (le transfert entier -> flottant coûte 5 cycles sur l'A57). 22 465 sites ; 4 414 copies (valeur aussitôt
  réécrite par un stfs) gardées en forme entière (sinon un fmov w, s de plus). Pièges : vset_lane_f32 sur un vecteur
  nul ajoutait un fmov s, s -> assembleur en ligne ; contrainte "Q" -> addition d'adresse en plus, "m" garde le
  décalage. Lecture volatile gardée (asm volatile), comme REX_LOAD_U32. Fichier 100 : 66 sites, -21 instructions,
  14 fmov s, w au lieu de 81. PC : forme entière (inchangé). Auto-contrôle au démarrage sur la Switch
  (« [auto-contrôle] chargements flottants », src/torchlight_appels_indirects.cpp) : forme ARM contre forme entière
  sur 16 valeurs (zéros, infinis, NaN, dénormaux).
- 2026-10-14 (soir) : **lot 2 mesuré sur la console** (constantes de .rdata, chargements flottants, -fno-math-errno,
  avec les appels promus ; nouveau profil PGO, session de 32 min ; dist/torchlight-essai-lot2.nro, MD5 61f32799, ELF
  switch/elf/torchlight-essai-lot2-20261014-2156.elf). Auto-contrôle des chargements flottants passé sur la console.
  A/B aux fréquences de base (CPU 1020 / GPU 307,2 / mémoire 1600, profileur, les deux sessions), mêmes endroits,
  contre l'essai des appels promus seuls : ville 269-273 dessins 21,3-21,8 ms (32-35 images/s) contre 262-266 dessins
  19,7-20,2 ms (37-38) ; mine 187-188 dessins 15,6-16,0 ms (46-48) contre 183-184 dessins 14,9-15,1 ms (50).
  Gain d'environ 4-6 % (un peu moins de dessins dans la session du lot 2 : 2-3 %). Attention : une première session
  du lot 2 tournait avec la mémoire à 2400 MHz (11,4 ms dans la mine contre 15,7 ms à 1600 au même endroit) :
  toujours vérifier la ligne « relojes » des deux sessions avant de comparer. Journaux :
  switch-logs-archive/20261014-essai-lot2-1600/ et 20261014-reference-indirects-soir/.
- 2026-10-15 : **registres en variables locales dans les fonctions feuilles** (switch/tools/registres_locaux_feuilles.py
  --toutes, d'après leaf_locals.py de sa2-nx ; build_switch.sh, avant appels_directs.py). Chez nous, les accès
  volatile à la mémoire du jeu empêchent le compilateur de garder en registres de l'hôte les registres restés dans ctx
  (aliasing) ; une fonction sans aucun appel peut les garder en variables locales. 5 026 feuilles, 29 854 registres
  (5 714 seulement lus). Switch, quatre fichiers : -0,9 à -1,9 % d'instructions, -4,6 à -7,7 % de chargements, -2 à
  -6 % d'écritures ; la variante « feuilles avec boucle seulement » (1 172) fait moins bien partout. PC : partie
  normale, vérifications à 0 différence. Exact par construction (rien d'autre ne lit ctx pendant une feuille).
- 2026-10-15 : **registres en variables locales le temps des boucles sans appel** (switch/tools/registres_locaux_boucles.py,
  d'après loop_locals.py de sa2-nx ; début de boucle = étiquette visée par un saut en arrière, faute de barrières
  chez nous ; std::isnan ajouté aux noms connus, boucles avec __sync_bool_compare_and_swap laissées). 8 522 régions
  dans 3 379 fonctions, 43 170 registres, 6 471 passerelles. Switch, quatre fichiers : -1,1 à -1,9 % de chargements,
  -1,3 à -2,4 % d'écritures, instructions -0,6 à +0,9 % (passerelles hors des boucles). PC : partie normale,
  vérifications à 0 différence.
- 2026-10-15 : **divisions (sdiv/udiv) et conversions (fcvtzs) en assembleur : essayé, abandonné.** Résultats exacts
  (simde fait déjà une conversion C saturante sur ARM) mais gain statique de -1,7 à +0,7 % selon le fichier :
  l'assembleur empêche le compilateur de fusionner ou supprimer des conversions (215 : fcvtzs 37 -> 63) ; opérations
  rares dans le code chaud. Outil non gardé.
- 2026-10-15 : **profil de la console avec le lot 2** (dist/torchlight-essai-lot2.nro, pilas-on, ~6 min aux fréquences
  de base ; pilas-archive/rex_pilas-20261015-lot2.txt, switch-logs-archive/20261015-profil-lot2/). Avec cette build
  LTO, le thread principal se trouve par son nom (`pilas.py ... "Main XThread (F"`), pas par sub_821F8818. Profil
  toujours plat ; en jeu (ville et mine), temps propre : walkOctree sub_821C7158 4-7 %, natif::Render (notre relevé)
  ~3 % (+2 % dans ses appels), natives du squelette ~3 %, renderSingleObject sub_821C5FA0 2-3 %, memcpy ~2,4 %,
  contrôleurs sub_821C8C00 ~1,6 % (7,6 % inclusif avec les particules), setPass sub_821C5730 ~2 %.
  **Scène lente de la ville (18 images/s, ~52 ms, 177 dessins ; 14:46:19-14:47:19, PNJ donneur de quête)** : ~40 %
  du thread principal dans le texte de CEGUI (sub_821B3DA0, largeur d'un texte : Font::getTextExtent de la variante
  du jeu, boucle sur les caractères avec recherche des glyphes ; appelée par sub_825B37F8 et sub_825B3420, mise en
  page du texte avec retour à la ligne), 5,8 % du thread sur toute la session en temps propre.
- 2026-10-15 : **parcours de l'octree en natif** (src/torchlight_ogre_natif.cpp : sub_821C7158 walkOctree, avec
  sub_821C7000 _getCullBounds et sub_821C0E78 getVisibility, aussi interceptées). Même pile explicite en mémoire du
  jeu, même r18 gardé dans les octants complets, mêmes appels virtuels (isVisible, _notifyCurrentCamera,
  _updateRenderQueue ; cibles courantes appelées directement), préchargements aux endroits des dcbt du jeu.
  Validation (mode 2) : parcours à blanc (sans _notifyCurrentCamera ni _updateRenderQueue) avant l'original,
  comparé aux visibilités d'octants et tests de boîtes relevés pendant l'original (appels faits depuis le cadre du
  parcours) et au contenu de la pile : PC, deux zones (191 et 287 dessins), 42 000 parcours, 34 millions de mots
  comparés, 0 différence ; _getCullBounds et getVisibility aussi comparées à chaque appel. perf sur PC : parcours
  1,10 % -> 0,75 % des échantillons. Les appels de sub_821C7000 et sub_821C0E78 remis en sub_X dans le code généré
  (fichiers 17, 101, 107, 146, 190). Aussi : -ffp-contract=off pour torchlight_ogre_natif.cpp sur la Switch (GCC
  contracte a * b + c en fma par défaut en mode GNU : les natives validées sur PC, sans fma, auraient pu s'écarter du
  recompilé sur la console). À mesurer sur la console avec les natives suivantes.
- 2026-10-15 : **largeur des textes de CEGUI en natif** (sub_821B3DA0 getTextExtent et sub_821B3C88 getGlyphData,
  src/torchlight_ogre_natif.cpp). L'original convertit à chaque appel le texte et deux balises de la police en
  UTF-8 puis en std::wstring (allocations, ConvertUTF8toUTF16 d'Unicode Inc. dans sub_821B3780) ; pour des points
  de code de 1 à 0xFFFF l'aller-retour est l'identité, la native lit donc les points de code de la CEGUI::String
  (sinon l'original). Balises (police +1028, find_first_of sub_821A5CE0, wcsncmp sub_821B39E0) et arrondis
  (fadds, fmuls avec l'échelle en double, fmadds) comme le jeu ; pages de glyphes chargées à la demande comme
  l'original (bit en +720, rasterize [3] du jeu). Non reproduits : allocations temporaires et tampon UTF-8 des
  chaînes (cache refait par chaque c_str). Validation sur PC (mode 2, mine, interface) : 140 000 largeurs dont
  44 000 avec balises, 0 différence (résultat comparé au bit près) ; getGlyphData comparée à chaque appel. Appels
  remis en sub_X dans 21 fichiers du code généré. La scène du PNJ n'a pas pu être rejouée sur PC : à regarder sur la
  console (mise en page des dialogues).
- 2026-10-15 : **contrôleurs en natif** (sub_821C8C00 updateAllControllers) : boucle sur le std::set, source
  FrameTimeControllerValue, fonctions getAdjustedInput (avec ou sans échelle) et modificateur de coordonnées de
  texture en natif ; ParticleSystemUpdateValue et les cibles inconnues restent celles du jeu. Validation (mode 2) :
  chaque calcul natif comparé à la fonction du jeu sur le même état (sauvegardé puis remis), successeur de l'arbre
  comparé à sub_824C6960 : ~2 millions de comparaisons par 10 s, 0 différence (deux zones). TreeNext généralisé
  (isnil en +21 pour les std::map, +17 pour ce std::set). Appel remis en sub_X (fichier 214). Rendu vérifié sur PC
  en mode 1 (mine, capture).
- 2026-10-15 : **relevé (natif::Render) : attentes de la mémoire.** Profil de la console : ~2,9 % du thread
  principal en propre (4,3 % avec ses appels) sans boucle chaude ; points chauds = premières lectures du
  descripteur de dessin (920 octets, ~300 par image, 3 images en circulation), case de la table des géométries,
  compteur de Lock (ldar dans une table de 256 Ko), recherche des textures (unordered_map). Constantes mesurées sur
  PC : 138 Ko par image, toutes lues par le rendu (rien à gagner à en copier moins). Fait : préchargement du
  descripteur suivant et de ses tableaux, des cases du compteur de Lock et de la table des géométries dès que
  l'adresse d'un tampon est connue ; petit cache direct devant le cache des textures (préchargé en début de
  dessin) ; compteurs de géométrie du bilan dans l'image au lieu d'atomiques (~700 par image). Aucun changement de
  ce qui est relevé (PC : mêmes dessins, capture juste). perf sur PC : relevé ~1,6 % du thread du jeu, peu
  parlant (gros caches) : à mesurer sur la console.
- 2026-10-15 (soir) : **session PGO** avec la build instrumentée (lot 3, natives de l'octree, du texte et des
  contrôleurs, relevé préchargé ; dist/torchlight-pgo-instrumente.nro, MD5 d121a6af) : 22 min aux fréquences de base,
  237 profils (pgo/), journaux switch-logs-archive/20261015-pgo-session/. Dialogues des PNJ affichés correctement
  (native du texte vérifiée sur la console par l'utilisateur). Build finale LTO + PGO lancée (switch/out-lto).
- 2026-10-15 (soir) : **version 1.0.2 préparée** (build des joueurs, LTO + PGO du soir, aucune incompatibilité du
  profil) : dist/torchlight-nx-1.0.2.zip (MD5 f6f94267, .nro d6685a15), texte dist/RELEASE-1.0.2.md. Mesure de la
  build de test équivalente (aux fréquences de base) : ville 272-277 dessins 20,2-20,9 ms (36-38 images/s), mine
  198-199 dessins 15,6-15,9 ms (41-42, niveau plus chargé que pour le lot 2) ; journaux
  switch-logs-archive/20261015-essai-natives/. Aucune référence 1.0.1 valable aux fréquences de base (les sessions
  du 2026-10-14 tournaient à 2400 MHz de mémoire) : pas de chiffre publié. README : chiffres d'images/s retirés
  (variables selon les lieux et les réglages), sa2-nx dans les remerciements.
- 2026-10-20 : **revue de torchlight-recomp (YaPeL, https://github.com/YaPeL/torchlight-recomp, GPL-3.0, portage
  PC du même jeu)** pour la Switch. Mêmes options du générateur que nous ; leurs contrôleurs en natif et les dcbt du
  jeu : sans gain (comme chez nous). Retenu, dans l'ordre : liaisons des tampons sautées (fait, ci-dessous), blocs de
  géométrie statique (StaticGeometry::GeometryBucket) hors de l'écran non mis en file pour la caméra principale
  (boîte par bloc et par morceau ; chez eux +10 % en combat, ~+6 % en ville, images identiques), écouteur du RT
  Shader System en natif (sub_821C4F50), _setTextureBlendMode (sub_821C9100) et champs non initialisés des modes de
  mélange d'Ogre 1.7 (notre relevé recopie les 15 mots tels quels : faux états possibles).
- 2026-10-20 : **SetStreamSource (sub_821C3D58) et SetIndices (sub_821C39F8) du Direct3D Xbox sautés en rendu natif
  seul** (`--torchlight_natif_liaisons`, vrai par défaut), ainsi que _setPolygonMode (case 81) et
  _setPointSpritesEnabled (case 36), qui n'appellent que __SetRenderState (sub_821C5480). Appelants : setVertex-
  BufferBinding, _render, déliaisons du périphérique (sub_821CECF0, sub_827746B0) ; aucun ne lit r3 ensuite ; rien
  chez nous ne lit l'état du périphérique qu'ils écrivent. Mesures sur PC (avec --torchlight_natif_seul=true : sur
  PC il n'est pas activé par défaut, piège qui avait faussé un premier A/B) : il ne reste qu'une attente du GPU par
  image (présentation, retour 821F37CC) ; l'envoi du tampon de commandes pendant un Lock est rare avec comme sans ;
  temps « image » dans le bruit du PC (7,5-7,9 ms contre 7,5-8,6 ms en paires alternées). Captures identiques hormis
  ce qui bouge. À mesurer sur la console (A/B par l'option, même build).
- 2026-10-20 : **blocs de géométrie statique hors de l'écran non mis en file pour la caméra principale**
  (src/torchlight_blocs.cpp, `--torchlight_blocs` : 0 comme le jeu, 1 retirés (défaut), 2 validation ; rendu natif
  seul ; d'après le « bucket culling » de torchlight-recomp). Chaque StaticGeometry::GeometryBucket (vtable
  0x820016B4) reçoit une boîte (tous ses sommets, espace de sa région) et jusqu'à 16 boîtes de morceaux (composantes
  connexes de ses triangles, regroupées par coupes médianes), calculées au premier passage, oubliées à sa destruction
  (sub_8247F728) ; pendant _findVisibleObjects (sub_821A6070) du viewport de la scène (gardé par CreateViewports,
  sub_8220A470, +0x48 ; SceneManager +0xA8), un bloc dont aucune boîte (transformée par le nœud de la région, +0xE4,
  calculs en double et petite marge) ne touche le frustum de la caméra n'atteint pas RenderQueue::addRenderable
  (sub_821C3730). Coupé pour la session si un programme de sommets ne place pas la position comme le RT Shader
  System (transformation fixe ou animation matérielle). Validation (mode 2) : rien n'est retiré, la capture liste les
  dessins qui l'auraient été (trace/capture/retires.txt, seulement entre le parcours principal et le suivant : la
  carte de lumière dessine les mêmes blocs) ; l'outil hors ligne rend l'image avec et sans eux (EXCLURE). PC, deux
  zones de la mine : 286 -> 212 dessins (74 blocs sur 158 testés) et 192 -> 155 (37 sur 92), images **identiques à
  l'octet**. PC, A/B alterné au même endroit : 287 -> 212 dessins, image côté jeu 4,4-4,7 -> 3,8-4,0 ms (~-14 %).
  Ville (sauvegarde de la console copiée le 2026-10-20 dans switch/out-pc/donnees-console-20261020, personnage 3,
  manette « 15:a,20:a,25:a » : au-delà, les appuis sur A ouvrent le dialogue d'un PNJ voisin et la caméra zoome) :
  304 -> 283 dessins (21 blocs sur 132), identiques à l'octet ; la scène du dialogue zoomé aussi (237 -> 218).
  À mesurer sur la console (A/B par l'option, même build, après le PGO).
- 2026-10-20 : **A/B sur la console du tri des blocs et du saut des liaisons** (même .nro, dist/torchlight-essai-
  blocs.nro, MD5 41ee123e, LTO + PGO du 2026-10-20 ; ELF switch/elf/torchlight-essai-blocs-20261020-*.elf ; session 1
  avec torchlight.toml « torchlight_blocs = 0, torchlight_natif_liaisons = false », session 2 sans ; fréquences de
  base dans les deux sessions, CPU 1020 / GPU 307,2 / mémoire 1600 ; personnage immobile aux mêmes endroits ;
  journaux switch-logs-archive/20261020-ab-sans/ et 20261020-ab-avec/) :
  - ville : 277-284 dessins, 19,3-20,0 ms, 39-40 images/s -> 245-247 dessins, 18,7-18,9 ms, 41-42 images/s
    (22 blocs retirés sur 128 testés par parcours ; ~-4 %) ;
  - mine : 379-386 dessins, 22,5-23,6 ms, 33-35 images/s -> 206 dessins, 16,5-17,1 ms, 44-45 images/s
    (148 blocs retirés sur 229 ; ~-28 %, +30 % d'images/s).
  Le saut des liaisons n'est pas mesuré à part (les deux changements ensemble). Pendant la session PGO, la
  mémoire était à 2400 MHz (sans effet sur le profil) : vérifier le profil sys-clk du jeu avant chaque mesure.
- 2026-10-20 : **écouteur par objet du RT Shader System en natif** (sub_821C4F50, src/torchlight_ogre_natif.cpp ;
  d'après torchlight-recomp, relu dans notre code généré : propriétaire +4, drapeau +328, clé 0x8349D2A4, liaisons
  de la passe +368, SGPass +12/+20, sous-états +8/+12 appelés par la case 5, fin du vecteur relue à chaque tour
  comme le jeu). Recherche du SGPass (std::map<String, Any>, any_cast) en natif, sous-états du jeu ; attributs non
  alloués ou autre type : l'original. Appel promu de sub_821C4EE0 remis en sub_X (fichier 137). Validation (mode 2,
  getUserAny et any_cast du jeu comparés) : ville et mine, 2,2 millions de recherches, 0 différence. perf sur PC
  (ville, part du processus) : écouteur et recherche 0,79 % -> 0,64 % : gain faible (le reste est la boucle des
  sous-états et les nœuds de la map en mémoire). À mesurer sur la console avec la suite.
- 2026-10-20 : **_setTextureBlendMode (sub_821C9100)** : piste de torchlight-recomp déjà couverte chez nous (sautée
  en rendu natif seul depuis le 2026-10-02 ; ses seules écritures sont des membres +144 + 32 * unité, et un calcul de
  couleur VMX dont le résultat reste sur la pile). **AnimationTrack::getKeyFramesAtTime (sub_821CCC60) en natif**
  (fonction feuille, 0,84 % du thread principal sur la console en jeu) : temps ramené dans la durée, lower_bound sur
  les temps des images clés, facteur d'interpolation en float comme le jeu. Validation (mode 2, sorties et f1 au bit
  près) : ville et mine, 6,5 millions d'appels, 0 différence.
- 2026-10-20 : **version 1.0.3 préparée** (build des joueurs, LTO + PGO de la session 3 du 2026-10-20, aucune
  incompatibilité du profil) : dist/torchlight-nx-1.0.3.zip (MD5 eec6c786, .nro 50b23a21), texte
  dist/RELEASE-1.0.3.md ; README : torchlight-recomp dans les remerciements.
- 2026-10-20 : **essai de la build des joueurs 1.0.3 sur la console** (sans torchlight.toml) : démarrage, ville, mine,
  sortie normale, aucune erreur, fréquences de base (switch-logs-archive/20261020-joueurs-103/). L'utilisateur a vu
  le halo au sol du feu de la place s'éclairer d'un coup en partant vers la gauche. Sur PC (personnage 3, stick à
  gauche, captures de 38 à 46 s dont le feu en bord d'écran) : images avec et sans les blocs retirés identiques à
  l'octet. Sur la console avec `torchlight_blocs = 0` : le halo varie de même (session sans bilan [blocs],
  switch-logs-archive/20261020-halo-sans-blocs/) : effet du jeu (probablement la carte de lumière, cible 208, et sa
  propre caméra), pas du tri. torchlight.toml retiré ensuite.
- 2026-10-20 : **deux pistes de Torchlight Rekindled (GreenSwampDweller, https://github.com/GreenSwampDweller/torchlight-rekindled,
  GPL-3.0 ; moteur réécrit d'après le code Xbox, OGRE 14 et GL) mesurées sur le profil de la console du lot 2**
  (pilas-archive/rex_pilas-20261015-lot2.txt avec switch/elf/torchlight-essai-lot2-20261014-2156.elf ; les fichiers
  rex_pilas-20261015-1451/1846/1914 de l'archive sont des copies identiques de celui-ci). (1) Table des passes de la
  file de rendu (leur correctif OGRE 0019) : le visiteur groupé du jeu est sub_821CC700 (variante de Runic : pour
  chaque passe de la map, copie les renderables, les trie par l'échelle de lumière +0x1c et rappelle la passe quand
  elle change, comme le décrit leur light_override). Entrées vides sautées (début == fin du vecteur) ; leur parcours
  (lecture du vecteur de chaque nœud, 0x22670 de l'ELF, et incrément d'arbre en ligne) ~0,8 % du thread principal en
  jeu, temps propre total de la fonction 1,6 %. Pas de copie de shared_ptr comme dans OGRE 14 : gain plafonné à
  ~0,15 ms par image, pour une native avec suivi des entrées non vides depuis addRenderable : écarté. (2) Index du
  zip (leur 0010) : le jeu passe par zziplib (ZipArchive d'Ogre 1.7) ; ouverture d'entrée sub_82589D10 0,45 % de la
  session, presque tout en lecture ; aucune recherche de nom visible au chargement (lectures fsdev 7,6 %,
  décompression sub_823E31D8 3 %, allocateur sub_821CD7F8 3 %, texte de CEGUI) : écarté.
- 2026-10-20 : **profil de la console avec la 1.0.3** (build des joueurs, .nro 50b23a21, pilas-on, ~5 min aux
  fréquences de base, relojes vérifiés ; pilas-archive/rex_pilas-20261020-2334.txt avec
  switch/elf/torchlight-joueurs-1.0.3-20261020-1620.elf, switch-logs-archive/20261020-pilas-103/). console.sh
  journaux archive désormais le nouveau relevé, une seule fois (empreinte). Tranches de 5 % du thread principal :
  ville 2-8 (35-37 images/s, ~280 dessins), donjon 11-18 (mine 42-43 puis zone lourde 17-24 images/s, 300-395
  dessins). Répartition (ville / donjon) : renderOneFrame sub_821B07F0 73 / 74 % dont cibles de rendu sub_821F26B0
  40 / 33 % (renderSingleObject 23 / 18 %), recherche des objets visibles sub_821A6070 20 / 20 % (WalkOctree natif
  5,2 / 6,2 % propre ; Region::_updateRenderQueue de la géométrie statique sub_821CCE90 5,4 % dont notre test des
  blocs Outside 1,9 / 1,4 % propre), contrôleurs sub_821C8C00 7,9 / 15,6 % (ParticleSystem::_update sub_821C8618
  5,6 / 10 %, dont sub_821C81F8 -> sub_821C7B88 émetteurs/particules 4,2 / 6,5 %) ; logique du jeu sub_821F5CA0
  22 / 21 %, plate (squelettes natifs ~3 %). Pistes : (1) Outside : la géométrie statique ne bouge pas, garder les
  boîtes déjà transformées tant que la matrice du nœud est la même (~2,7 µs par bloc testé aujourd'hui) ;
  (2) particules du jeu en natif (gros morceau, surtout dans les donjons).
- 2026-10-21 : **boîtes des blocs gardées, préchargement des contrôleurs et systèmes de particules arrêtés en natif**
  (commit 4587fcb). Blocs : boîtes transformées (centre, demi-étendue) gardées par bloc tant que les 16 mots de la
  matrice du nœud sont les mêmes ; PC, ancien et nouveau calcul côte à côte : 260 000 tests, 0 écart. Contrôleurs :
  PC, ~700 systèmes de ParticleUniverse par image en ville (80 actifs), ~1 100 dans la mine (260 actifs) ;
  préchargement par vagues (contrôleurs, destinations, systèmes, techniques des actifs) puis parcours inchangé ;
  ParticleSystemUpdateValue d'un système arrêté (état 2 ou 3 : compteur +280 jusqu'à 900) en natif, compteur à 799
  laissé au jeu ; validation (mode 2) ville et mine : 0 différence. A/B sur la console (fréquences de base, XJE1,
  sans torchlight.toml ; A : build des joueurs 1.0.3, switch-logs-archive/20261021-ab-103/ ; B :
  dist/torchlight-essai-particules.nro, MD5 2ab596e4, ELF switch/elf/torchlight-essai-particules-20261021-0010.elf,
  switch-logs-archive/20261021-ab-essai/). Ville, même endroit (133/20 et 132/22 blocs testés/retirés) : A 271-277
  dessins, 20,96-21,40 ms (32-36 images/s) ; B 266-270 dessins, 20,61-20,83 ms (36-37) : ~-2 %, à la limite du
  bruit. Mine : A 195 dessins 17,6-17,7 ms (45 images/s) ; B sans fenêtre immobile comparable (personnage en
  mouvement, 208-293 dessins). Aucune erreur.
- 2026-10-21 : **essai sur la console de la 1.0.3 refaite** (build des joueurs, .nro 7e0d5f10, zip febc143e, ELF
  switch/elf/torchlight-joueurs-1.0.3-20261021-0039.elf ; profil PGO du 2026-10-20 : UpdateControllers,
  ControllerDestination et Outside compilées sans profil ; XJE1, sans torchlight.toml, fréquences de base ;
  switch-logs-archive/20261021-joueurs-103/). Démarrage, ville, mine, sortie normale, aucune erreur. **Mais la mine
  est plus lente** qu'avec le premier zip de la 1.0.3 (session A du même soir, même endroit : 162 blocs testés,
  195 dessins) : 19,0-19,2 ms (42 images/s ; « juego » 42,6-43,1) contre 17,6-17,7 ms (45 ; 45,1-45,6), ~+8 % ;
  ville 21,4-22,4 ms (268-272 dessins) contre 21,0-21,4 (271-277). Suspects : le préchargement (second parcours des
  ~1 100 contrôleurs avant le vrai), la perte du profil PGO sur les fonctions modifiées, une variation entre
  sessions. Suite décidée avec l'utilisateur : nouvelle session PGO (build instrumentée) pour couvrir ces
  fonctions, puis mesurer de nouveau ; sinon A/B par options dans un même .nro (préchargement, systèmes arrêtés).
- 2026-10-27 : **poses des squelettes sur d'autres cœurs (d'après torchlight-rekindled, GreenSwampDweller, GPL-3.0 :
  renderer_poses.cpp, patch OGRE 0025) : essayé, non retenu en l'état.** Leurs autres patchs OGRE du même commit
  (0026 masque avant _notifyCurrentCamera : déjà l'ordre de notre variante Xbox, isVisible sub_821C3660 teste le
  masque avant ; 0027 sous-arbres : sans objet avec l'OctreeSceneManager ; 0028-0030 : ordre des dessins, pas de
  performance) et leurs particules (index et réutilisation dans leur propre moteur) ne s'appliquent pas.
  Fait puis retiré (trop risqué pour le gain : décision de l'utilisateur ; ancienne option --torchlight_poses) : le parcours de l'octree
  relève les objets visibles, calcule en parallèle Entity::cacheBoneMatrices (sub_821B93B0) des entités admissibles
  (XHostThread du SDK, 2 threads + le principal ; os et points d'attache sans écouteur, objets attachés qui ne lisent
  pas leur position, animations linéaires, liste des temps sub_8246E800 construite avant), puis rejoue
  _notifyCurrentCamera et _updateRenderQueue dans l'ordre. Validation PC (mode 2) : ~4 300 poses, 0 différence ; menu
  animé (captures). Appels figés __imp__sub_821B93B0 remis en sub_X (fichiers 10, 136, 206). **Mais presque rien à
  gagner** : la logique de Runic calcule chaque unité au début de sa mise à jour (sub_821D5410 -> sub_821BB578 ->
  Entity::_updateAnimation sub_821B9478, drapeau +465) ; en jeu, aucune entité n'arrive au rendu sans sa pose.
  Diagnostic PC (retiré) : en ville 51 poses de la logique par image ; seules 2 entités (personnage, familier) ont
  leurs os relus avant le rendu (~34 lectures par image, par sub_821D9008 juste après l'animation) ; l'état d'une
  entité change après sa pose quelques fois par 10 s. Un report des poses de la logique jusqu'au rendu serait donc
  possible (recalcul immédiat à la lecture des os ou au changement d'animation), mais les lectures directes en
  mémoire ne se verraient pas.
  **Profil de la console en combat** (build des joueurs du 2026-10-21 11:33 : .nro 4a290135 = switch/out-joueurs,
  ELF switch/elf/torchlight-joueurs-1.0.3-20261021-1133.elf, .text identique ; XJE1, fréquences de base vérifiées ;
  pilas-archive/rex_pilas-20261027-1507.txt, switch-logs du 2026-10-27 15:07 ; le relevé couvre ~15:03:30-15:08:20,
  combats 15:05:20-15:08:10 à 21-28 images/s, 25-32 ms). Tranches 9-18 (combat), thread principal : rendu 67,6 %
  (cibles de rendu sub_821F26B0 29 % : renderSingleObject 15,4 %, visiteur sub_821CC700 14,3 % ; recherche des
  objets visibles 15,9 % : WalkOctree 3,8 % propre, billboards sub_821DA090 3,4 %, géométrie statique 2,4 %,
  entités 3,3 %) ; **contrôleurs 17,5 % (boucle 8,2 % propre, ParticleSystem::_update 8,7 %)** ; logique 27,8 %
  (plate : sub_821E6BD8 -> sub_821D8238 12,4 %, sous-fonctions de moins de 3,5 % ; animation des unités 4,9 %) ;
  **poses (cacheBoneMatrices) 6,0 % en tout**, dont 4,5 % par la logique. Temps propre de la boucle des contrôleurs :
  5-11 % (1-5 % dans le profil du 2026-10-20) : le travail des systèmes arrêtés y est passé de ParticleSystem::_update
  (natif depuis le 2026-10-21), total des contrôleurs du même ordre (14-21 %).
- 2026-10-27 : **boucle des contrôleurs en liste plate, contrôleurs dormants sautés** (src/torchlight_ogre_natif.cpp,
  --torchlight_controleurs : 1 par défaut, 0 = parcours du std::set comme le premier zip de la 1.0.3). Mesure PC en
  jeu : ~1 730 contrôleurs par image, dont ~1 320 de systèmes de ParticleUniverse arrêtés (état +284 = 2 ou 3) au
  compteur +280 déjà à 900, avec source FrameTimeControllerValue et fonction getAdjustedInput sans mDeltaInput :
  leur mise à jour ne fait rien, mais la boucle lisait 7-8 lignes de cache pour chacun. Liste plate dans l'ordre du
  set (adresses croissantes) : dormant sauté après lecture de la ligne du contrôleur (+52 actif, +24 destination
  inchangée) et de celle du système (+280/+284). Liste tenue à jour par createController (sub_824B4C78, rend le
  contrôleur : insertion) et ~Controller (sub_824B5A10 : retrait), faits sur le thread de la boucle hors de celle-ci ;
  sinon (pendant la boucle, autre thread) refaite à l'image suivante, ou si la taille du set (+12) diffère. Appels
  figés __imp__ remis en sub_X (createController : 9 fichiers ; ~Controller : 216).
  **Piège (plantage sur la console en combat, première version)** : la mise à jour d'un système peut créer ou
  détruire d'autres contrôleurs (effet terminé) ; le jeu, qui avance dans le set, ne les visite pas, la liste les
  gardait (lecture de mémoire libérée). Correctif : drapeau testé en tête de chaque tour ; s'il est levé, suite dans
  le set à partir du premier contrôleur d'adresse supérieure à la dernière entrée traitée (UpperBound, comparaison
  d'adresses), comme l'itérateur du jeu. Validation PC (mode 2) : liste comparée au set à chaque image, UpperBound
  vérifié pour chaque contrôleur, dormants vérifiés sans effet, reprise forcée à chaque image (essai) : 0 différence.
  **A/B sur la console** (XJE1, fréquences de base vérifiées dans chaque session, mêmes endroits immobiles ; boucle
  des contrôleurs = ligne « [contrôleurs] » du journal, particules actives comprises) :
  - variante 0 (premier zip, sans préchargement) : ville 1,76-1,98 ms, image 19,2-19,4 ms ; mine (1 355 contrôleurs)
    2,69-2,78 ms, image 15,1-15,3 ms ;
  - variante « 1.0.3 refaite » (préchargement par vagues) : ville 2,12-2,17 ms, image 19,2 ms (41 images/s) ; mine
    (1 448) 4,02-4,10 ms, image 16,3 ms (48) : **le préchargement ralentissait** (cause probable de la lenteur de la
    1.0.3 refaite dans la mine, 2026-10-21) ;
  - liste plate : ville 1,20-1,26 ms, image 18,1-18,4 ms (42) ; mine (1 445) 2,04-2,05 ms, image 14,7-14,8 ms (50-51) ;
  - liste plate + préchargement des seuls systèmes actifs : ville 1,37-1,44 ms ; mine 2,50-2,58 ms : plus lent,
    retiré avec l'autre préchargement.
  En combat (non comparable d'une session à l'autre) : 1 600-2 000 contrôleurs, boucle 6-9 ms, 19-24 images/s ;
  liste refaite 2-5 fois par 10 s (contrôleurs changés pendant la boucle), 23 reprises dans le set sans incident.
  Journaux : switch-logs-archive/20261027-ab-controleurs-{0,1,2,2b,3}/ ; builds d'essai dist/torchlight-essai-
  controleurs*.nro et ELF switch/elf/torchlight-essai-controleurs-*.elf. Suite : profil de combat (pilas-on) avec
  cette boucle, pour choisir entre particules actives, dessin et recherche des objets visibles.
- 2026-10-27 : **deux fonctions de la logique en natif** (src/torchlight_ogre_natif.cpp, validation PC en mode 2 :
  0 différence). (1) sub_821A1510, recherche d'un élément par nom (std::wstring) dans un conteneur : nom = valeur de
  l'identifiant de l'élément dans la std::map de son objet (sub_821A1608, sub_821A1670), comparaison sub_821A1478 ;
  570 appels figés remis en sub_X (~90 fichiers). PC : 189 000 appels comparés. (2) sub_822F92A0, segment contre les
  objets de deux listes (intersection la plus proche) : le rejet de sub_822815E8 (case [60] : accesseurs +484, +488,
  +1056 ; boîte du segment élargie du rayon contre la boîte +384, minimum +32, maximum +44, sinon Vector3::ZERO) est
  natif pour tous les objets ; si aucun ne passe, rien n'est écrit et le résultat est 0 comme dans le jeu, sinon la
  fonction du jeu est appelée en entier (PC en ville : 27 appels sur 20 000). Appels figés de sub_822F92A0 remis en
  sub_X (fichiers 6 et 64).
  Console (dist/torchlight-essai-natives-logique.nro 8ced7bbf, ELF switch/elf/torchlight-essai-natives-logique-
  20261027-2030.elf ; pilas-archive/rex_pilas-20261027-2102.txt, switch-logs-archive/20261027-profil-natives-logique/ ;
  fréquences de base ; combats ~2 min, 3 793 échantillons) : sub_821A1510 1,90 -> 1,24 % du thread principal en combat
  (13,5 -> 9,2 % de sub_821D3FD8) ; sub_822F92A0 avec sub_822815E8 2,35 -> 1,24 % (16,7 -> 9,2 %). Environ 1,8 % du
  thread principal en combat. Reste : parcours natif de la recherche par nom 1,2 % (attentes de la mémoire : élément,
  objet, arbre, chaîne), rejet natif 0,66 %, calcul fin du jeu 0,53 %.
- 2026-10-27 (soir) : **analyse du dessin et du multi-cœur, préparation de la session PGO.** Dessin en combat (profil
  rex_pilas-20261027-2102) : cibles de rendu 29 % du thread principal, aucune fonction du jeu au-dessus de 2 % en
  propre (renderSingleObject 2,0 %, paramètres des programmes sub_821C26D0 4,2 % inclusif dont _updateAutoParams
  natif 1,8 %, écouteur du RT Shader System 2,4 %, setPass 1,3 %, interface sub_821AFEA0 3,6 %, overlays
  sub_821D2F80 2,5 %) ; notre code 4,8 % (natif::Render 2,7 % propre, ~2,7 µs par dessin : attentes de la mémoire
  sur VertexData, déclaration, liaisons, tampons). Multi-cœur : logique ~31 % et rendu ~64 % du thread principal en
  combat ; aucun bloc isolé assez gros pour 20 -> 30 images/s ; recouvrir logique et rendu demanderait une copie
  figée de la scène (mémoire du jeu partagée) : hors de portée ; particules de ParticleUniverse en parallèle
  (~10 %) possibles seulement avec des tirages aléatoires dans un autre ordre (générateur global) : décision de
  l'utilisateur attendue. Build instrumentée pour une nouvelle session PGO (combats compris) :
  dist/torchlight-pgo-instrumente-4.nro (bbff7c56), envoyée sur la console ; ancien profil gardé dans pgo-20261021/.
- 2026-10-28 : **nouveau profil PGO (session 4, combats compris) et build des joueurs.** Session de l'utilisateur
  (~11 min 30, fréquences de base ; switch-logs-archive/20261028-session-pgo-4/) : 3 vidages seulement (toutes les
  3 min ; les ~2 min 30 après le dernier, dont le combat le plus lourd, perdues à la fermeture par HOME). **Piège :
  ninja ne suit pas les .gcda** : la première build des joueurs ne refaisait que l'édition des liens avec les objets
  de la veille (ancien profil). build_joueurs.sh supprime maintenant les objets plus anciens que le dernier .gcda de
  pgo/. Build : switch/out-joueurs, .nro 9b76b23c (copie dist/torchlight-essai-pgo4.nro), zip
  dist/torchlight-nx-1.0.3.zip c608ea1c, ELF switch/elf/torchlight-joueurs-1.0.3-20261028-1217.elf ; zip précédent
  (9fce6dce, 2026-10-21 11:33) gardé dans dist/torchlight-nx-1.0.3-20261021-1133.zip. Console XJE1 sans
  torchlight.toml (switch-logs-archive/20261028-joueurs-pgo4/) contre la liste plate d'hier (20261027-ab-controleurs-2,
  ancien profil), mêmes endroits : ville 242 dessins 18,4-18,7 ms contre 18,3-18,6 (égal) ; mine 143-144 dessins
  14,3-14,5 ms (51-52 images/s) contre 14,8-14,9 (~-3 %) ; combat 17-25 images/s. Aucune erreur. Publication remise
  après l'analyse des particules (décision de l'utilisateur).
- 2026-10-28 : **particules de ParticleUniverse sur d'autres cœurs : analyse de l'état partagé.** L'utilisateur accepte
  des particules non identiques à l'octet tant qu'elles sont visuellement équivalentes. Version de ParticleUniverse du
  jeu (RTTI) : émetteurs, affecteurs, externs, renderers ; ni observateurs, ni gestionnaires d'événements, ni
  émetteurs émis. rand du CRT (sub_821BDDA8) : graine dans les données par thread (_getptd sub_821BDD68, +20) : pas de
  course, chaque thread a sa suite. Outil : switch/tools/suivi_memoire/ (preparer.py : copie du code généré dont le
  pch suit chaque lecture et écriture quand t_suivi_actif est posé, REX_RAW_ADDR classés d'après l'instruction ;
  _update sub_821C8618 encadré dans sub_821C8A58 ; blocs oubliés à chaque allocation sub_821CD7F8 ; écritures de la
  même valeur à part ; build PC à part, lancée avec --torchlight_ogre_natif=0 --torchlight_crt_natif=false). Pour
  chaque image, tout mot de 4 octets lu ou écrit par un système après l'écriture d'un autre (ou écrit après la
  lecture d'un autre) est noté. Session PC dans la mine avec combats (manette de script) : 11 069 images, 137 systèmes
  mis à jour par image ; résultat switch-logs-archive/20261028-suivi-particules/. Familles :
  (1) bénignes : compteurs de statistiques du ParticleSystemManager (singleton 0x83506798 ; +192..+208, seulement
  incrémentés dans tout le code) ; allocateur (verrou récursif sub_8241F8F8, tas par thread : sub_8241F6F8,
  sub_8241F820) ; rand et données par thread (r13) ; réécritures de la même valeur par Camera::isViewOutOfDate
  (sub_821C47F0) ;
  (2) caméra : ParticleSystem::_notifyCurrentCamera (sub_821C3A88, case 0xd4, appelé par _update ; sort tout de suite
  si la caméra est la même que +484) recalcule la caméra (Frustum sub_8219FDC0, isViewOutOfDate) et, par
  sub_821BA2A0 -> sub_821A1CA8, le code de Runic attaché à la caméra (+600) qui écrit des globales (0x834A4B18..
  0x834A5F4C : tampon mis à zéro, liste) : ~4 % des images, plus les recalculs du frustum ~5-9 % ;
  (3) Node::requestUpdate (sub_821CCDD0, case 0x100) : insertion dans le std::set des enfants à mettre à jour d'un
  parent partagé (arbre sub_823ECF88, sub_823ECDE8, rotations sub_8246CB00, sub_82447398) : toutes les images ;
  (4) réserve de particules du gestionnaire (sub_8258F8F8 : vecteur +228 du ParticleSystemManager, appelé par
  sub_821BF568 depuis la mise à jour des techniques) : ~4 % des images.
  Processeur en combat sur la console : ~150 % sur 300 (principal 99 %, rendu ~30 %) : un cœur et demi libre.
  Plan possible : groupe de threads du jeu (XHostThread, comme l'essai des poses) pour les systèmes actifs de la boucle
  des contrôleurs ; avant, sur le thread principal, caméra de chaque système notifiée (2) ; sur les threads de
  travail, requestUpdate mis de côté puis rejoué dans l'ordre (3), réserve sous verrou (4) ; vérification par le même
  suivi (accès croisés restants = familles bénignes). Gain estimé : ~10 % du thread principal en combat, au mieux
  ~5 % gagnés (~1 image/s à 20).
- 2026-10-28 : **particules en parallèle, première version** (src/torchlight_particules.{h,cpp},
  --torchlight_particules : 0 = comme le jeu (défaut pour l'instant), 1 = mises de côté puis en série sur le thread
  principal (validation), 2 = en parallèle ; --torchlight_particules_threads = 2). La boucle native des contrôleurs
  met de côté les systèmes actifs admis (ControllerDestination -> Prendre) et Executer les met à jour à la fin de la
  boucle (thread principal + XHostThread du SDK, pile de 256 Ko). Admis : état +284 = 1, renderers billboards
  (vtable 0x82001BBC), techniques préparées (sub_821DD4F8 sans travail : +680/+681/+682 nuls, renderer +66,
  +189..+192), pas de fin de durée (+436, +384 + +476 x dt contre +432). En série (verrou récursif commun, classe
  Serie) : _notifyCurrentCamera sub_821C3A88 (caméra, tri à base fixe aux tampons statiques), réserve du gestionnaire
  sub_8258F8F8 et sub_825A4C68, BillboardSet::setPoolSize sub_8247A588 (destruction des tampons du Direct3D du jeu),
  createController et ~Controller ; différé puis rejoué dans l'ordre : Node::requestUpdate sub_821CCDD0 ; avant les
  tâches : frustum de chaque caméra (+484) mis à jour par son test de visibilité (case 372) sur une sphère nulle.
  Appels figés remis en sub_X dans le code généré (non versionné) : sub_821C3A88 (fichier 161), sub_821CCDD0 (119),
  sub_8258F8F8 (103), sub_825A4C68 (74, 94), sub_8247A588 (64).
  **Piège** : un thread de travail démarre avec r1 au sommet de sa pile ; setValue écrit dans le cadre de son appelant
  (r1+156 après stwu -128) : page de garde, boucle d'erreurs « Unhandled guest access violation ». Cadre de 512
  octets réservé au démarrage du thread.
  Validation PC (switch/tools/suivi_memoire, TORCHLIGHT_SUIVI_TACHES=1, mode 1 : seules les tâches sont suivies,
  accès en série marqués) : mine avec combats de script, ~12 000 images, ~210 systèmes admis par image (refusés :
  ~1 rendu, ~1 préparation, ~0,4 fin) : plus aucun accès croisé hors des familles bénignes (statistiques, allocateur
  du jeu : artefact du mode 1 où toutes les tâches partagent le tas du thread principal). Mode 2 sur PC : 5 min sans
  erreur, images normales (le script ne provoque pas de vrai combat). Profil de la console du 2026-10-27 : dans
  _update, notification de caméra ~17 %, requestUpdate ~14 % (restent en série) : ~69 % parallèle, gain attendu
  4-5 % en combat. Essai : dist/torchlight-essai-particules-paralleles.nro (73fae506 ; LTO + PGO du 2026-10-28 ; ELF
  switch/elf/torchlight-essai-particules-paralleles-*.elf), A/B sur la console à faire avec torchlight.toml
  « torchlight_particules = 2 » puis sans.
- 2026-10-28 (après-midi) : **A/B des particules en parallèle sur la console, puis natives et consolidation.**
  A/B (dist/torchlight-essai-particules-paralleles.nro 73fae506, même .nro ; B : torchlight.toml
  « torchlight_particules = 2 », A : sans ; switch-logs-archive/20261028-ab-particules-{A,B}/ ; fréquences de base) :
  ville 242 dessins A 18,3-18,4 ms / B 18,4-18,6 (contrôleurs 1,22-1,26 / 1,19-1,23) ; mine A 138-139 dessins 14,1-14,2
  / B 136-137 13,6-13,8 (contrôleurs 1,83-1,89 / 1,86-1,92 : sans effet au repos) ; combat A 318-381 dessins 31-37 ms,
  contrôleurs 6,8-8,7 / B 272-347 dessins 25-31 ms, contrôleurs 3,7-5,0 : ~2,5-3,5 ms gagnés par image (~8-10 %).
  Effets visuellement normaux (utilisateur). torchlight_particules = 2 par défaut.
  Profil de combat avec le parallèle (dist/torchlight-essai-natives-2.nro 9b5e6db0 ; pilas-archive/rex_pilas-20261028-
  1520.txt, switch-logs-archive/20261028-profil-natives-2/) : contrôleurs 8,6 % du thread principal (13,6 % avant),
  mais attente du verrou commun ~1 % (rmutexLock sous _update) : toute la notification de caméra était en série. Le
  verrou ne couvre plus que le tri des particules (sub_821A1CA8) ; validation par le suivi : aucun accès croisé hors
  réécritures de la même valeur (cache de vue de la caméra, déjà à jour). Critère de préparation : techniques
  désactivées (+92 nul) et sautées (+136) ignorées (console : jusqu'à ~45 systèmes refusés par image à cause d'elles).
  Natives (src/torchlight_ogre_natif.cpp, validées au bit près sur PC, mode 2, nouveau script de manette qui fait
  parcourir la mine) : sub_821D7D38 (clé et valeur d'une std::map, 4,2 M appels), sub_82312D98 (effets vivants d'un
  objet, 0,87 M), RenderPriorityGroup::clear sub_821CC998 (parcours des passes du cimetière et à re-hacher, recherches
  natives ; 1,8 M recherches, 3 688 passes présentes), Material::getBestTechnique sub_821CF100 (schéma absent laissé
  au jeu ; 4 M appels, 321 au jeu). Appels figés remis en sub_X : 821D7D38 (135 sites, 69 fichiers), 82312D98 (3),
  821CC998 (193), 821CF100 (24 sites), 821A1CA8 (145) ; sub_821C3A88 refigé (161). Reste du profil : plat (rendu
  d'Ogre : renderSingleObject 2,2 %, setPass 1,4 %, paramètres 0,9 % ; logique : grosses fonctions < 1 %).
  Build consolidée (dist/torchlight-essai-consolidee.nro 972a1319, ancien profil PGO ; switch-logs-archive/
  20261028-consolidee/) : ville 242 dessins 18,15-18,35 ms (contrôleurs 1,04-1,09) ; mine 143-144 dessins 14,2-14,55
  (contrôleurs 1,81-1,87) ; combat ~380 dessins 33,0-34,4 ms (contrôleurs 4,6-5,4) contre 37,2 (8,7) en A. Aucune
  erreur. Blocage : le thread principal (99 % d'un cœur en combat, attente du rendu 0,2-0,8 ms) ; le GPU (307 MHz)
  n'est pas le facteur limitant.
- 2026-10-28 (soir) : **session PGO 5, tri des tâches, mode boost des chargements, étude des billboards.**
  Session PGO (dist/torchlight-pgo-instrumente-5.nro 3ee068f3 ; 6 vidages, ~18 min sur 21,5 ; profil précédent
  gardé dans pgo-20261028/ ; switch-logs-archive/20261028-session-pgo-5/ ; CPU à 1224 MHz ~3 min, sans effet sur les
  comptes) : refus de préparation des particules tombés à 0 dans les scènes stables.
  Test des fréquences par l'utilisateur (même build ; switch-logs-archive/20261028-frequences/) : GPU à 460 MHz sans
  effet (ville 242 dessins 18,3-18,4 ms comme à 307 MHz) ; CPU à 1224 MHz : ville 16,25-16,3 ms (47 images/s), mine
  12,4 ms (58), combat 26,6-29,4 ms : le jeu est limité par le thread principal, pas par le GPU.
  Tri des tâches de particules (mode 2) : les plus coûteuses d'abord (particules actives des techniques +304, plus un
  coût fixe) pour moins attendre la dernière (Executer : 4,8 % du thread principal en combat, sa part comprise).
  Mode boost pendant les chargements (src/torchlight_boost.{h,cpp}, --torchlight_boost_chargement, vrai par défaut) :
  thread de surveillance ; appletSetCpuBoostMode(FastLoad) quand aucune image n'est finie depuis 250 ms (signal :
  EndGameFrame du rendu natif), retiré après 0,5 s d'images régulières ou hors du premier plan ; durée de chaque
  chargement dans le journal (« [boost] »). À vérifier sur la console.
  Étude (non faite, décision de l'utilisateur : publier d'abord) : sommets des billboards en parallèle. Mise à jour des
  billboards (BillboardRenderer::_updateRenderQueue sub_821DA090, appelée pendant le parcours de l'octree) : 3,96 % du
  thread principal en combat (sommets ~1 %, boucle ~1 %, préparation ~0,6 %, verrouillage ~0,5 % sous section
  critique du Direct3D du jeu, mise en file ~0,5 %). Piste : appels mis de côté pendant le parcours (natif), faits en
  parallèle à la fin de ce même parcours, addRenderable notés puis rejoués dans l'ordre (ordre de la file changé :
  seulement entre transparents de même profondeur), requestUpdate différés, création de BillboardSet en série ;
  validation par le suivi mémoire ; gain attendu ~1,5-2 % en combat.
- 2026-10-28 (fin) : **1.0.3 publiée.** Build des joueurs (profil PGO de la session 5, tri des tâches, mode boost) :
  .nro 3e94a750 (dist/torchlight-joueurs-1.0.3-20261028-2037.nro, ELF switch/elf/torchlight-joueurs-1.0.3-20261028-
  2037.elf). Console XJE1 sans torchlight.toml (switch-logs-archive/20261028-joueurs-final/) : ville 242 dessins
  18,25-18,4 ms ; mine 153 dessins 14,15 ms ; combat 368 dessins 32,2 ms ; 24 passages en mode boost (0,5-6,7 s, en
  groupes par chargement : lancement de la partie ~26 s, ville ~15 s, mine ~20 s), aucun en combat ; le profileur
  l'a vu (CPU 2397 MHz, GPU 76,8 MHz). Chargements plus courts d'après l'utilisateur. Aucune erreur.
  Zip refait à la main avec ce .nro et les README/THIRD_PARTY_NOTICES à jour (crédit torchlight-rekindled, conseil :
  monter le CPU plutôt que le GPU) : dist/torchlight-nx-1.0.3.zip 259af13c (build_joueurs.sh refait l'édition des
  liens, donc un autre .nro : à ne pas utiliser pour un simple changement de documents). main 25ccc79,
  torchlight-nx 6624ad2 poussé ; texte dist/RELEASE-1.0.3.md ; release à créer par l'utilisateur.
- 2026-11-01 : **documentation de renut-nx** (https://github.com/ChanseyIsTheBest/renut-nx, docs/, GPL-3.0 ; portage
  de Banjo-Kazooie N&B, outils repris de sa2-nx, masseffect-nx, futari-nx, marathon-nx). Comparée à notre chaîne :
  pistes retenues, par ordre : (1) toute la mémoire du jeu sans volatile + barrières en tête de boucle
  (pch_no_volatile.py + loop_barriers.py ; masseffect-nx : +15-20 % d'images/s sur la console) ; (2) plus
  d'écriture ctx.lr = <retour> avant chaque appel (elide_lr_stores ; 406 020 sites chez nous ; mflr lit toujours
  ctx.lr, contrairement à skip_lr) ; (3) non_argument_as_local refaite : leur première version oubliait de passer
  r11/r12... aux branchements conditionnels de fin de fonction (if (crN) { sub_X(ctx, base); return; }), et il faut
  une liste de fonctions gardées (tools/non_argument_audit.py) : explication probable de notre blocage du
  2026-10-02 (sections critiques jamais relâchées). Plus petits : mode 4 cœurs (docs/four-core-mode.md, NPDM du
  forwarder Sphaira écrit par le .nro), changements de mode flottant (24 661 sites disableFlushMode chez nous),
  rlwinm en décalages simples (-0,5 %), sections critiques en ligne. Leur rendu (deko3d) ne nous concerne pas.
- 2026-11-01 : **mémoire du jeu sans volatile** (switch/tools/memoire_non_volatile.py, lancé en dernier par
  build_switch.sh ; option CMake TORCHLIGHT_MEMOIRE_NON_VOLATILE, ON ; OFF = macros volatile et barrières vides,
  donc le code de la 1.0.3). Les huit REX_LOAD_U*/REX_STORE_U* passent par des types rex_mem_uN (may_alias,
  aligned(1)) ; REX_MM_* restent volatile. REX_LOOP_BARRIER() (asm "+m" sur la fenêtre de 4 Go seulement : ctx, pile
  de l'hôte et variables des registres locaux gardent leurs registres) après 28 606 des 33 773 têtes de boucle (les
  autres ne lisent pas la mémoire du jeu et n'appellent rien, ou chaque retour passe par un appel indirect) ;
  REX_SYNC_BARRIER() après les 75 sync / lwsync / eieio (le générateur n'en écrit rien ; le compilateur ne
  réordonne plus à travers, comme avec volatile). Sauvegarde du code généré d'avant :
  switch/out-pc/generated-avant-memoire-20261101.tar.zst. Contrôle de l'ELF : switch/tools/boucles_infinies.py
  (d'après find_self_loops.py) : 0 saut sur lui-même dans une fonction recompilée (1.0.3 : 2, sub_82418168 et
  sub_826A5BB8, des b . voulus). Build Switch LTO + PGO (ancien profil, appliqué en grande partie) sans outils,
  switch/out-memoire : .text 58,54 -> 57,18 Mo (-2,3 %). PC (clang) : session scriptée de 200 s (sauvegarde de la
  console, ville, déplacements, attaques, dialogue), natives en validation (mode 2) : 0 différence partout, images
  justes. Essai : dist/torchlight-essai-memoire.nro (d044c6e9, ELF switch/elf/torchlight-essai-memoire-20261101-
  0808.elf). Reste : A/B sur la console aux fréquences de base contre la 1.0.3 (ville, mine, combat), puis
  nouvelle session PGO si gardé.
- 2026-11-01 : **essai « mémoire sans volatile » sur la console XJE1** (fréquences de base vérifiées ; journaux
  switch-logs-archive/20261101-essai-memoire/, première session ignorée : réglage des fréquences par l'utilisateur ;
  ceux de la 1.0.3 d'avant : 20261101-avant-essai-memoire/). Aucun gel ni erreur. Ville, même endroit, 242 dessins :
  18,62-18,72 ms contre 18,25-18,38 avec la 1.0.3 (20261028-joueurs-final) : pas de gain, ~2 % plus lent. Mine
  (181 dessins contre 153) et combat (~285 contre 368) à d'autres endroits : non comparables. Profil PGO bien
  appliqué au code généré (torchlight_recomp.50 recompilé avec -Wmissing-profile -Wcoverage-mismatch : aucun
  avertissement). ctx est déjà __restrict dans notre SDK, pas de -mstrict-align. Le gain de masseffect-nx ne se
  retrouve pas ici.
- 2026-11-01 : mémoire sans volatile **désactivée** (TORCHLIGHT_MEMOIRE_NON_VOLATILE OFF par défaut, étape retirée
  de build_switch.sh, outil gardé ; code généré revenu à l'identique de la sauvegarde, vérifié par diff).
- 2026-11-01 : **adresse de retour non écrite dans ctx.lr** (switch/tools/retour_sans_lr.py, d'après elide_lr_stores
  de renut-nx ; lancé par build_switch.sh après pile_non_volatile.py ; option CMake TORCHLIGHT_RETOUR_LR, OFF = rien
  écrit, ON = code d'origine). 406 020 « ctx.lr = 0x...; » deviennent REX_LR(0x...). Audit : 34 757 mflr, presque
  tous de prologue (rangés sur la pile puis remis par mtlr) ; hors prologue, fonctions de la bibliothèque C du jeu
  0x8287C430/C570/C61C/C640 (mflr puis mtlr ; le blrl de funclet garde sa cible, écrite par un mtlr) et setjmp /
  longjmp ; RtlUnwind / RtlVirtualUnwind sont des bouchons du SDK (pas de déroulement de la pile du jeu) ; le runtime
  ne lit ctx.lr que pour des journaux et la sauvegarde d'état des threads. appels_indirects.py accepte désormais
  « REX_LR( » comme ligne avant un bctrl (381 sites promus retrouvés, 0 changement). Build Switch LTO + PGO (profil
  de la 1.0.3, structure des fonctions inchangée) sans outils, switch/out-memoire : .text 58,54 -> 54,92 Mo
  (-6,2 %) ; mêmes 2 sauts sur eux-mêmes que la 1.0.3. PC : même session scriptée, 0 différence partout, images
  justes. Essai : dist/torchlight-essai-lr.nro (fbceeee7, ELF switch/elf/torchlight-essai-lr-20261101-0850.elf).
- 2026-11-01 : **essai « ctx.lr non écrit » sur la console XJE1** (fréquences de base vérifiées ; journaux
  switch-logs-archive/20261101-essai-lr/). Aucune erreur. Ville, même endroit, 242 dessins : 17,85-18,03 ms contre
  18,25-18,38 (1.0.3) et 18,62-18,72 (essai mémoire sans volatile) : ~-2 % contre la 1.0.3. Mine, même endroit que
  l'essai mémoire (181-183 dessins) : 14,81-14,90 contre 15,29-15,42 (-3 % ; pas de mesure 1.0.3 à cet endroit).
  Combat (280-320 dessins) : 24-26,5 ms, comme la 1.0.3 et l'essai mémoire à nombre de dessins égal (bruit trop
  fort pour conclure). Gardé.
- 2026-11-01 : **non_argument_as_local refaite** (r0, r2, r11, r12, f0, v32-v63 en variables locales ; d'après
  renut-nx / sa2-nx). Cause probable du blocage du 2026-10-02 : notre SDK n'avait que l'option de base, sans liste
  de fonctions gardées ni passage des registres : un appelant qui met r12 en variable ne le passe pas à une fonction
  qui le lit à l'entrée (funclets : r31 = r12 - N). Générateur du SDK modifié (switch/patches/nfsmw-sdk.patch :
  config.h/.cpp non_argument_as_local_keep ; builders/context.cpp localizeNonArguments, keepsNonArgumentsInCtx,
  emit_call_handing_non_arguments aux appels directs, aux branchements conditionnels de fin et aux tables de saut
  (control_flow.cpp) ; emit_call_sharing_registers élargi à r0-r13/f0-f13/v0-v63 ; rexglue recompilé, en-tête
  précompilé de nfsmw-nx/out/host périmé supprimé). Audit : switch/tools/audit_non_arguments.py (copie de
  non_argument_audit.py de renut-nx) sur un code généré SANS l'option dans le bloc-notes : ENTRY 308 (helpers
  __save/__rest et 90 fonctions : funclets r12, sub_82623220/34 r11), RET 409 dont presque tout derrière des
  fonctions sans retour (lancement d'exception sub_8285ADE0/AE18 -> sub_82865D90, DbgBreakPoint sub_8287D560,
  rapporteur d'erreur sub_828B5820, longjmp sub_828636E0 : --noreturn) ; restent 3 subfe sans effet et un vrai
  retour dans r11 (sub_828756C8, lu par sub_82875600). Appels indirects qui passent r12 : _CallSettingFrame
  sub_8287C430 (r12 = r4) et sub_828609A4 (r12 = r5). Liste switch/non_arguments_gardes.txt (93), écrite par
  gen_manifest.sh dans non_argument_as_local_keep (option ajoutée aux options par défaut). Code généré avec
  l'option, audit --with-option : HANDOVER 0, RET 2 (subfe), ENTRY = les fonctions gardées. Régénération du vrai
  code puis toute la chaîne (constantes 6974 repliées, 381 appels promus retrouvés, natives appelées par leurs
  interceptions, aucune retouche à la main). Build Switch LTO + PGO (profil de la 1.0.3 : il s'applique encore,
  torchlight_recomp.50 sans avertissement) : .text 54,92 -> 52,38 Mo (-4,6 % ; -10,5 % depuis la 1.0.3) ; mêmes 2
  sauts sur eux-mêmes. PC : deux sessions scriptées, 0 différence partout, images justes ; le chemin du personnage
  diffère d'une session à l'autre de la même build (dialogues de PNJ selon leur position) : le script de manette
  n'est pas reproductible au-delà de ~50 s. Essai : dist/torchlight-essai-nonarg.nro (859ac9fd, ELF
  switch/elf/torchlight-essai-nonarg-20261101-1010.elf). Sauvegarde du code généré d'avant :
  switch/out-pc/generated-avant-memoire-20261101.tar.zst (sans les REX_LR, remis par retour_sans_lr.py).
- 2026-11-01 : **essai non_argument_as_local (+ ctx.lr non écrit) sur la console XJE1** (fréquences de base
  vérifiées ; journaux switch-logs-archive/20261101-essai-nonarg/). Aucune erreur, pas de blocage. Ville, même
  endroit, 242 dessins : 17,13-17,36 ms (44 images/s) contre 17,85-18,03 (essai ctx.lr) et 18,25-18,38 (1.0.3) :
  -4 % et -6 %. Mine, même endroit (180 dessins) : 14,13-14,32 contre 14,81-14,90 (essai ctx.lr, 183) et
  15,29-15,42 (essai mémoire, 181) : -4,5 %. Combat (comparaison approchée à nombre de dessins égal) : 276 dessins
  22,8 ms, 286 23,8, 309 25,3 contre 280 24,0, 291 26,5, 322 26,2 (essai ctx.lr) : ~-5 %. Gardé.
- 2026-11-01 : **mémoire sans volatile re-essayée par-dessus non_argument_as_local et ctx.lr, abandonnée.** Build
  Switch .text 52,38 -> 51,01 Mo, 0 saut sur lui-même. Mais PC (session scriptée, natives en mode 2) : 1 parcours
  de l'octree différent sur 54 000 (les trois sessions précédentes : 0 sur 52-56 000), venu du test de visibilité
  sub_821C0E78 (Camera::isVisible) : native et recompilé, mêmes données au même moment, résultats différents. Cause
  probable : le changement de mode des denormals (ctx.fpscr.disableFlushMode -> msr fpcr / _mm_setcsr) n'a pas de
  clobber mémoire ; avec volatile, une lecture du jeu ne passait pas de l'autre côté, sans volatile la lecture et le
  calcul flottant qui en dépend peuvent remonter avant le changement de mode (calcul dans le mauvais mode, visible
  seulement sur des denormals). Sur la Switch, REX_LOAD_F32 reste en asm volatile, mais lfd (REX_LOAD_U64) et les
  copies entières restent exposés. À corriger (clobber mémoire dans setcsr du SDK) avant tout nouvel essai ; gain
  non démontré (-2 % au premier essai). Barrières retirées, option OFF.
- 2026-11-01 : **relevé des piles de l'essai nonarg** (console XJE1, pilas-archive/rex_pilas-20261101-1139.txt,
  ELF switch/elf/torchlight-essai-nonarg-20261101-1010.elf ; thread principal, 60 % de fin) : profil plat (aucune
  fonction du jeu > 1,7 %) ; code du jeu ~72 %, natives ~17 % (WalkOctree 4,1 %, contrôleurs 3,4 %), relevé du
  rendu natif 2,8 %, memcpy 2 %, malloc/free de newlib ~1,7 % dont attente de son verrou (svcArbitrateLock sous
  _free_r appelé depuis sub_821EE1C0 : concurrence avec d'autres threads ; piste hors code généré), sommeil du jeu
  1,25 %, sections critiques 0,28 % (piste « sections critiques en ligne » de renut-nx écartée).
- 2026-11-01 : **rlwinm / rlwimi en décalages simples** (switch/tools/decalages_rlwinm.py, d'après
  rotate_word_shifts de renut-nx ; lancé en dernier par build_switch.sh). rotl64(dup(x), SH) & M avec M sans tour
  -> (uint32_t)(x << SH) & M, (x >> (32 - SH)) & M ou rotl32(x, SH) & M ; rlwimi : même chose sous masque
  (GCC : bfi). 48 001 rlwinm et 4 123 rlwimi réécrits, 2 288 gardés (masque qui fait le tour). Test
  switch/tools/test_decalages_rlwinm.py : paires du code (1 191, 9,7 M comparaisons) et toutes les paires (16 896,
  138 M), 0 différence (le premier jet avait une condition fausse pour le décalage à droite, m >> (32 - SH) au lieu
  de m >> SH : attrapée par le test). GCC de devkitA64 : 1 instruction au lieu de 2 (décalage), 1 au lieu de 2-3
  (extraction), 1 au lieu de 4 (rlwimi). PC : session de contrôle, 0 différence, images justes. Pas d'essai console
  seul (gain attendu ~0,5 %, sous le bruit). Code généré figé pour la session PGO.
- 2026-11-01 (fin) : build instrumentée switch/out-pgo (code figé, main 589625f) envoyée sur la console XJE1
  (profils de la console vidés, journaux archivés dans switch-logs-archive/20261101-avant-pgo6/) ; session PGO n°6
  jouée par l'utilisateur, à récupérer à la prochaine session (étapes dans CLAUDE.md). Profil de la 1.0.3 gardé dans
  pgo-103/ ; zip publié 1.0.3 protégé dans dist/torchlight-nx-1.0.3-publie.zip.
- 2026-11-01 (soir) : **session PGO n°6 récupérée** (240 .gcda, 10 vidages, ~35 min de 20:55 à 21:30, ville, mine
  et combats jusqu'à 350+ dessins ; journaux switch-logs-archive/20261101-pgo6/, profil dans pgo/). **1.0.4**
  (TORCHLIGHT_VERSION passé à 1.0.4 avant build_joueurs.sh) : .text 52,00 Mo (1.0.3 : 58,54), 2 sauts sur
  eux-mêmes (les voulus), .nro dist/torchlight-joueurs-1.0.4-20261101-2145.nro (MD5 6e78a8dd), ELF
  switch/elf/torchlight-joueurs-1.0.4-20261101-2145.elf. Console XJE1, fréquences de base
  (switch-logs-archive/20261101-joueurs-104/) : ville 242 dessins 17,42-17,60 ms (1.0.3 refaite le même soir :
  18,25-18,38, -4,5 % ; essai nonarg avec l'ancien profil : 17,13-17,36, donc ~+1,5 %, juste au-dessus du bruit
  d'une session à l'autre ; relevé natif 1,30 -> 1,23 ms : l'écart est dans le code généré) ; mine 184-185 dessins
  14,56-14,67 (pas le même endroit que les essais) ; combat 352 dessins 28,7 ms. Cause possible de l'écart avec
  nonarg, non vérifiée : les décalages rlwinm (jamais mesurés seuls sur la console) ou le profil n°6. A/B proposé
  (nonarg puis 1.0.4 dans la même session), pas fait : l'utilisateur publie la 1.0.4 telle quelle.
  Étude du même soir : setVertexBufferBinding (sub_821C3E78, 1,7 % du thread principal) ne vaut pas une native :
  SetStreamSource déjà sauté, mise à jour des tampons périmés (sub_821A71E8, 0,4 %) nécessaire au rendu natif, et
  le temps propre (1,2 %) est fait de défauts de cache sur les objets tampons que natif::Render relit juste après.
  Mêmes conclusions pour setPass / _setTextureUnitSettings (le reste est lu par le natif) ; renderSingleObject et
  les paramètres des programmes : ~1 % chacun pour un gros chantier.
- 2026-11-03 : **essai d'un cœur réservé au thread principal (idée de l'utilisateur), préparé, pas encore mesuré.**
  Avant : aucun thread épinglé (préféré -1, masque 0x7) sauf la sortie audio (priorité 0x2B, cœur 0 seul, ~1 %,
  threadCreate avec -2) et les threads de Mesa (masque 0x6, inactifs). Journaux de la 1.0.4 : le temps hors CPU du
  thread principal égale son attente du rendu (ville ~0,20 contre ~0,26 ms ; mine ~0,30 contre ~0,24 ; combat ~0,39
  contre ~0,48) : les autres threads ne lui prennent presque rien, seuls les effets de cache (L1, TLB) et les saccades
  peuvent bouger. nfsmw-nx : préféré sans effet, masque exclusif sur leur thread de rendu = minimum de 21 à 16,3 images/s
  (cœur non réservé) ; migrations ~0,05 % du CPU.
  src/torchlight_coeurs.cpp + RexSwitchReserverCoeur (switch_perf.cpp, nfsmw-sdk.patch) ; repassé toutes les 5 s pour
  reprendre les nouveaux threads ; torchlight_coeur_principal (-1 par défaut), torchlight_coeur_alterner (réservation
  activée et retirée à chaque fenêtre de 5 s). Lignes « [coeurs] » du journal, analyse switch/tools/ab_coeurs.py
  (paires de fenêtres voisines, chargements écartés). Build : dist/torchlight-essai-coeur.nro (MD5 59af8ba1 ; profil
  PGO de la 1.0.4, avertissements de lignes décalées dans torchlight_natif.cpp seulement), réglages
  dist/torchlight-essai-coeur.toml à copier sous le nom torchlight.toml, ELF
  switch/elf/torchlight-essai-coeur-20261103-1050.elf. switch/out-joueurs contient désormais cette build, plus la 1.0.4.
  Protocole : 2 min immobile en ville, 2 min dans la mine, puis des combats, avec le cœur 2 puis le cœur 1.
  Session 1 (cœur 2, fenêtres de 5 s, switch-logs-archive/20261103-essai-coeur2, firmware 23.0.1, mémoire à 1331 MHz) :
  écart moyen ville 23,53 -> 23,04 ms (-2,1 %), mine 21,15 -> 20,80 (-1,7 %), médiane en baisse aussi ; combat ~-0,9 % brut,
  ~-6,7 % à 280 dessins (estimation sur 28 fenêtres) ; pics maximaux plus longs (182 contre 109 ms en combat). NON CONCLUANT :
  cycle de 10 s en phase avec les rapports du profileur (48 rapports sur 48 en masque 0x7, donc réservation non prouvée, et
  coût des rapports dans les fenêtres sans réservation). Build suivante : fenêtres de 7 s, masques relus (lignes « [coeurs]
  masque= autres= ») ; dist/torchlight-essai-coeur.nro (MD5 898ec3ab), ELF switch/elf/torchlight-essai-coeur-20261103-1114.elf.
  Session 2 (cœur 2, fenêtres de 7 s, kip Horizon-OC chargé, mémoire à 1600 MHz, switch-logs-archive/20261103-essai-coeur2-7s) :
  réservation PROUVÉE (masque 0x4 et 0 autre thread dans 29 fenêtres sur 30 ; 3 autres dans la dernière, threads créés
  entre-temps), rapports du profileur répartis (23 réservés, 20 non). Ville 22,20 -> 21,73 ms (-2,2 %, 18 paires sur 18),
  mine 20,27 -> 19,74 (-2,8 %, 21 sur 21) ; médiane et 99e centile en baisse (-2,3 à -3,3 %). Pics de 32 ms ou plus en
  immobile : 13 fenêtres sur 19 sans réservation, 6 sur 21 avec ; mais les deux seuls pics de plus de 100 ms (148 et
  107 ms) sont avec réservation, cause inconnue. Combat : 70 s seulement (10 fenêtres, ~300 dessins, 40,10 -> 39,83 ms, très variable) : non concluant,
  session de combat longue à faire. Horizon-OC-Monitor (obsolète) lit mal les mV et la charge de la RAM depuis
  hoc-clk 2.6.0, Status Monitor correct (confirmé par l'utilisateur).
- 2026-11-03 : **recyclage des tampons du relevé, sommets des billboards différés, étude des poses.** Build d'essai non
  encore sur la console : dist/torchlight-essai-coeur-bb-recyclage.nro (1a3b21d0) + .toml, ELF switch/elf/torchlight-essai-coeur-bb-20261103-1242.elf.
  **Recyclage** (src/torchlight_natif.cpp) : c.bytes.assign allouait à chaque recopie et les tampons retraités étaient
  libérés en rafale au début de l'image : malloc + free = 1,59 % du thread principal en combat (profil 20261028-1520),
  dont 0,44 % d'attente du verrou de malloc. Réservoir par puissance de 2 (16 Mo au plus), retraites par tranches d'image
  (FreeRetiredGeometry avant ForgetOldGeometry). PC : 42-64 copies par image, toutes recyclées, 0 tampon neuf.
  **Billboards** (--torchlight_billboards, défaut 0 ; switch/tools/billboards_differes.py, dernière étape du pipeline,
  recopie telle quelle la boucle de sub_821DA090 dans torchlight_billboards_boucle) : le thread principal garde la
  préparation, beginBillboards, setVisible, la mise en file (même ordre) et la statistique ; les boucles (~2,3 % sur
  4,1 % en combat) sont faites à la fin du parcours de l'octree (hook sub_821C7158), tampons déverrouillés ensuite dans
  l'ordre. Rien d'autre ne tourne pendant les boucles. PC : mode 3 (faites aux deux moments, comparées à l'octet)
  774 903 boucles, 0 différence ; mode 2 sans erreur ; suivi mémoire (suivi_billboards.cpp, mode 1) 646 354 boucles,
  440 M d'accès : aucun accès croisé, témoin TORCHLIGHT_SUIVI_TEMOIN vu en (A) et (B) ((C) non couvert par le témoin).
  Scènes PC : ville et mine, peu de particules (~30 par parcours), pas de vrai combat : à confirmer sur la console.
  **Alternance générale** : --torchlight_alterner="coeur,billboards,recyclage" remplace torchlight_coeur_alterner : un
  seul réglage change à chaque fenêtre de 7 s (code de Gray) ; ab_coeurs.py compare les fenêtres voisines par réglage.
  **Étude des poses** (Entity::cacheBoneMatrices sub_821B93B0, 7,4 % du thread principal en combat : 5,7 % dans la logique,
  1,75 % au rendu ; suivi_poses.cpp, preparer.py ... poses ; switch-logs-archive/20261103-suivi-poses/ ; session
  PC ville et mine, 13 461 images, ~27 poses exécutées par image, pas de vrai combat). L'allocateur (sub_8241F6F8...)
  est écarté : ses métadonnées passaient pour des lectures. Part du coût par accès mémoire (approximation) :
  **21 % jamais lues** ; **~78 % lues pendant la logique**, juste après la pose, par quelques fonctions (sub_821D9008 :
  objets attachés +588/+592 ; Node sub_821C8518, sub_821C9BB0, sub_821AC1E8 ; ~11 unités par image, ~10 lectures de
  mots chacune). Entrées modifiées après la pose : numéro d'image, un mot partagé, et sub_8251A8E8 sur ~8 % des images.
  Accès croisés entre poses : seulement le numéro d'image des squelettes partagés (sub_821B93B0, 38 adresses) : les poses
  sont parallélisables d'après cette session. Entités jamais lues : 2e instance du familier (SMRBInstance, DOG.MESH,
  8,5 %), coffres, ~14 petites entités (0,5 % chacune). Instance « General » du héros (11,7 %) lue seulement en logique.
  Conclusions : différer les poses de la logique ne rapporte presque rien (attente dès la lecture suivante) ; éviter les
  poses jamais lues : ~1,5 % au mieux en ville (lectures directes en mémoire invisibles : à borner, par exemple aux
  entités non visibles sans objets attachés) ; calculer les poses en parallèle en tête de sub_8219D288 (sub_821BB578
  entier avancé) : ~3-4 % au mieux, mais l'ordre change quand une unité modifie l'animation d'une autre dans la même
  image : suivi à refaire au niveau de sub_821BB578, en vrai combat. Rien de fait, décision de l'utilisateur.
- 2026-11-03 (fin) : **A/B dans la même session des trois réglages sur la console** (build 1a3b21d0, cœur 2,
  billboards mode 2 ; switch-logs-archive/20261103-essai-coeur-bb/, analyse_ab.py : régression sur les fenêtres de 7 s,
  fréquences de base, kip Horizon-OC chargé, mémoire 1600 MHz). Aucune erreur, aucun plantage ; réservation prouvée (masque
  0x4, 0 autre thread, 56 fenêtres). Écart moyen entre images, effet de chaque réglage (marge à 95 %) :
  ville (245 dessins, 38 fenêtres) : cœur -2,10 ± 0,77 %, recyclage -1,54 ± 0,77 %, billboards -0,21 ± 0,77 % ;
  mine (192 dessins, 33 fenêtres) : cœur -2,40 ± 0,37 %, recyclage -1,48 ± 0,37 %, billboards -0,35 ± 0,37 %.
  Tout activé contre tout coupé : ville 22,46 -> 21,57 ms (-3,95 %), mine 20,14 -> 19,30 ms (-4,17 %) ; temps de l'image
  côté jeu : ville 17,44 -> 16,49, mine 14,63 -> 14,03 (tout coupé = mesures de la 1.0.4). Combat (~3,5 min, 30 fenêtres,
  200-378 dessins) : non concluant, marge de ± 7 % : la scène change trop en 7 s ; les pics par réglage y suivent surtout
  les fenêtres les plus lourdes (fenêtres 001 et 101 à 360-378 dessins). Pics de plus de 100 ms en immobile : 103 ms
  avec réservation, aucun sans (avec la session précédente : 3 fenêtres sur 58 avec, 0 sur 53 sans hors sortie de
  chargement) : faible, à surveiller. Prochaine mesure proposée pour le combat : fenêtres courtes (~1,5 s).
- 2026-11-03 (soir) : **poses : seconde instance évitée et précalcul en parallèle (prototype, src/torchlight_poses.cpp).**
  Suivi étendu (switch-logs-archive/20261103-suivi-poses/donjon-regles.txt) sur une copie de la sauvegarde de la console
  (switch/out-pc/donnees-console-20261103, personnage de niveau 18, ruines d'Estherian, 318 entités, peu de vrai combat :
  menu 22:a,30:bas,34:a,40:bas,42:bas,44:bas,46:bas,50:a,56:a, pas plus tôt : un « bas » trop tôt mène à New Character).
  Le héros et le familier ont un modèle caché (unité +432, mêmes os que +428) : 37 % du coût des poses dans cette scène.
  Celui du familier (SMRBInstance, sans objet attaché : 15,6 %) n'est jamais lu ; celui du héros (21,4 %, objets attachés)
  est lu par sub_821C9BB0, gardé. Règle retenue : +432, entité +528 nul. Autres poses jamais lues : ~6 % ici (0,17 % si
  on exige « pas rendue à l'image précédente »).
  Précalcul de sub_821BB578 (85 % des accès de la boucle des unités) : lectures de mots modifiés plus tôt dans la boucle
  dans 5 images sur 13 760 (sub_822C4790, sub_822C6000, sub_8251A8E8) ; courses seulement à la première utilisation d'une
  animation (6 images : liste des temps sub_8246E800, sous verrou dans le crochet de sub_821E2D90). Conditions de
  sub_821D5410 et sub_8219D340 reproduites ; entités avec objets attachés laissées au jeu (Node::requestUpdate sur un
  parent partagé). Validation PC en mode 3 (parallèle, chaque pose recalculée à sa place et comparée) : donjon 70 355
  poses, 0 différence, aucune erreur ; menus 52 237, 1 différence. Appels figés remis en sub_X : sub_821BB578 (13),
  sub_821B9478 (1), sub_8219D288 (3), sub_821E2D90 (4). Gain estimé : 0,4-0,8 % (double), ~3 % en combat (précalcul) ; à
  mesurer. Build : dist/torchlight-essai-poses.nro (8e2740ac) + .toml (fenêtres de 2 s ; cœur et recyclage
  actifs en continu), ELF switch/elf/torchlight-essai-poses-20261103-1423.elf.
- 2026-11-03 (soir) : **A/B sur la console : poses en parallèle, billboards, seconde instance** (build 8e2740ac, fenêtres de 2 s,
  cœur 2 et recyclage actifs en continu ; switch-logs-archive/20261103-essai-poses/ ; analyse switch/tools/ab_regression.py
  ville=14:27:05-14:28:30 mine=14:29:05-14:33:20 combat~=14:33:30-14:37:05). Aucune erreur, aucun plantage. Écart moyen entre
  images, effet de chaque réglage (marge à 95 %) :
  poses : ville -0,94 ± 1,03 %, mine -0,31 ± 0,26 %, combat -4,59 ± 3,13 % (régression avec le nombre de dessins, 107
  fenêtres) ; 53 paires voisines corrigées des dessins : -4,14 ± 2,07 %, 37 plus rapides ; le temps de l'image côté jeu ne
  bouge pas (0 ± 3 %) : le gain est bien dans la logique ; il suit le nombre d'unités (19 modèles par boucle en gros combat) ;
  billboards : ville -1,05 ± 1,03 %, mine -0,03 ± 0,26 %, combat -1,04 ± 3,16 % (6 paires à 270 dessins ou plus : -2,84 %) ;
  seconde instance : ville -0,39 ± 1,04 %, mine -0,81 ± 0,26 %, combat +0,33 ± 3,16 % (trop petit pour le combat).
  Tout activé contre cœur + recyclage seuls : ville 21,37 -> 20,77 ms (46,8 -> 48,2 images/s), mine 19,03 -> 18,79. Contre les
  mesures tout coupé de la session précédente : ville 22,46 -> 20,77 (-7,5 %). Pics de 100 ms ou plus en combat : 4 fenêtres
  avec poses, 5 sans : pas de dégradation. Fenêtres de 2 s : le combat devient mesurable. Reste : validation à l'octet en vrai
  combat sur la console (torchlight_poses = 3, simple réglage), puis, pour une version, valeurs par défaut et nouvelle
  session PGO (code généré modifié : billboards_differes.py, appels remis en sub_X).
- 2026-11-03 (soir) : **validation des poses en vrai combat sur la console** (build 8e2740ac, torchlight_poses = 3 ;
  switch-logs-archive/20261103-validation-poses/, pilas-archive/rex_pilas-20261103-1454.txt, ELF
  switch/elf/torchlight-essai-poses-20261103-1423.elf) : 103 923 poses comparées (jusqu'à 30 modèles par boucle), 1 différente
  (entité 494DDD10, à 14:51:33), aucune erreur, aucun défaut visuel vu par l'utilisateur sur la session à 2 s. Malloc et free du
  thread principal en combat : 1,59 % -> 0,19 % (recyclage). Ryzen (ssh ryzen, ~/workspace identique, RX 9070 XT, Weston sans
  écran : ~/essais/lancer.sh) : compilation PC et jeu sans écran validés.
- 2026-11-03 (nuit) : **Ryzen, poses du rendu, pilote de natives.** Ryzen (ssh ryzen, RX 9070 XT, Weston sans écran,
  ~/essais/veille.sh, serie.sh) : `/dev/shm` plein après des arrêts forcés (« Erreur du bus ») : nettoyer par fuser. Poses du
  rendu : première version (toute sub_821E0EA8 en parallèle) : 1 plantage en ~30 sessions (lecture à 0x11 : _Isnil d'un std::set),
  non reproduit en 50 sessions, mais le suivi mémoire montre les accès croisés (std::set, verrou sub_821B4C80, cache de techniques
  sub_821CF100) : switch-logs-archive/20261103-suivi-poses/rendu-avant-correctif.txt. Correctif : seule la pose part en parallèle,
  une par squelette, le reste de sub_821E0EA8 sur le thread principal dans l'ordre : plus aucun accès croisé hors allocateur
  (rendu-apres-correctif.txt, 832 486 tâches) ; mode 3 : 71 654 poses comparées, 0 différence (peu d'entités par parcours : 1,9 au
  plus). Leçon : les sessions répétées ne trouvent pas ces courses, le suivi mémoire si.
  Natives : 571 fonctions du code généré = 75 % du thread principal en combat, dont 11,6 % déjà natives (une native apparaît sous
  son nom sub_X dans le profil) ; 50 premières restantes : 29,8 %. Pilote de 7 fonctions (3,9 %) : sub_821B7510, sub_821BFB00,
  sub_821A7428, sub_821D7CE0, sub_821C4728, sub_821CE4B8, sub_821CCE90 ; mode 2 sur le Ryzen : 3,4 M appels, 0 différence. À mesurer
  sur la console (torchlight_alterner = "natives,rendu") avant d'écrire les autres.
- 2026-11-03 soir : **top 50 du thread principal en natif, demandé par l'utilisateur** (puis les 50 suivantes une fois stabilisé).
  Fin de src/torchlight_ogre_natif.cpp, sous g_pilote_actif (alternance « natives » de src/torchlight_coeurs.cpp) ; fonctions pures
  comparées en mode 2 (FinR3, FinMots ; CompterDifference note la ligne du source des premières différences).
  Méthode, à garder pour la suite :
  - Lecture : asm des commentaires du code généré. Attention : `ble` = !(a > b), `bge` = !(a < b), `bne` = !(a == b) sur les
    flottants (NaN) : à traduire tels quels. Crochets par macro (TL_TRACE, TL_SLOT de torchlight_trace.cpp) : invisibles à un grep de
    REX_HOOK_RAW(sub_X) : corps natif dans torchlight::ogre, appelé par un crochet écrit à la main (sub_821C3E78, sub_821C8E40, sub_821C93A0).
  - **Bug trouvé par les essais : un pointeur partagé passé par valeur est détruit par l'appelé (convention MSVC)** : sub_821C26D0 ne
    peut pas sauter le compteur +1 avant la case 364 (bindGpuProgramParameters) ; plantage à la première passe. Pour un const& (sub_821C8E40),
    le +1/-1 peut sauter. Regarder si le jeu appelle le destructeur après l'appel.
  - Convention LTCG : switch/tools/audit_retours_natifs.py signale les appelants qui relisent un registre volatil après un bl. Seul
    sub_821AFE28 (r8, r9, r10 que l'original ne touche pas non plus) : sans risque.
  - Cas rares laissés à l'original à l'entrée, avant tout effet : sub_821C5730 (unité à ombres, +324 == 2), sub_821C5FA0 (états supprimés,
    par lumière, sans lumières), sub_821C47F0 (réflexion), sub_821CC700 (premier appel, garde du vector statique), sub_821CD7F8 (hors cache).
  - Essais Ryzen : ~/essais/veille.sh n1 200 (natif, captures), n2 200 --torchlight_ogre_natif=2 ; gdb2.sh (pile au premier accès invalide).
    Piège : ne pas additionner avec un sed sans filtre : les lignes « [poses] ... validation » donnaient 19 x n « différences » (l'heure).
  Mesures de validation jusqu'ici : 77,7 M appels, 0 différence ; session native 200 s sans violation, rendu vérifié sur capture.
  Pas encore mesuré sur la console.
  Suite du même soir : top 50 fini (49 de la liste : sub_8221FEB0 écrite après coup, oubliée au premier passage ; sub_821D5410 exceptée), 15 sessions de 260 s en
  donjon sans plantage (3 manches de 5). Les 50 suivantes : 37 du classement, plus des fonctions de rang inférieur et des auxiliaires, 109 en tout à la fin du fichier ;
  hors crochets le thread passe de 41,2 % à ~30 % des échantillons du profil (ne dit rien du gain : une native coûte aussi). Le code généré n'a pas de
  sauvegarde des registres en prologue (variables locales) : le gain attendu de ces réécritures vient des détours supprimés (tri par insertion
  natif, pointeurs partagés sans compteur, recherches dans les arbres, Lock et accesseurs en ligne), pas du prologue. À mesurer sur la console avant d'en tirer une conclusion.
  sub_821AC8F8 (effets) : passe par l'original pour une unité qui a un effet long (EffetLong) ; compteur natifs / original sur PC.
  Outils : switch/tools/audit_retours_natifs.py (appelants qui relisent un registre volatil), switch/tools/appels_figes.py.
  Dernière tranche (recherche de chemin sub_8221FEB0 et sub_82220110, Window::setArea_impl de CEGUI sub_821A4B78 et ses auxiliaires, cartes de wstring sub_8231F078 et
  sub_823C67E0, tirage d'animation sub_821C00C0 sans les deux copies de chaînes, sub_825EBAA0, free du tas sub_821CCF30) :
  - sub_821CCF30 est le mspace_free de dlmalloc, appelé par tous les threads. Verrou récursif compatible avec l'original (échange atomique sur le mot +444), sans la section
    critique globale du recompilé (mfmsr/mtmsrd). Blocs mmappés laissés à l'original, avant toute écriture. Réduction du tas (sub_8241DA58, sub_8241D610) en fin de chemin.
  - Validation d'une native à effets de bord (à réutiliser) : coeur écrit en gabarit sur la mémoire (TasDirect / TasJournal). En mode 2, verrou tenu, le calcul natif écrit dans un journal,
    puis l'original s'exécute : chaque mot du journal doit être en mémoire, et l'en-tête du tas (114 mots) ne doit pas changer hors du journal. 1,32 M de blocs comparés, 0 différence.
  - Couverture sur PC : les macros Pilote() et PiloteOuValidation() journalisent « [couverture] sub_X » au premier passage (hors Switch). Sessions donjon et ville, 240 s : 103 natives suivies, 6
    jamais atteintes (sub_821BE5A8, sub_821C08B8, sub_821C8A58, sub_821CF690, sub_821A4A50, sub_821A4020) : leurs appelants sont devenus natifs et passent par les mêmes auxiliaires.
  Résultats : mode 2, 122 M appels, 0 différence ; 15 sessions de 260 s en donjon, 0 violation d'accès. Laissées de côté : sub_8247F888 (chargement d'un maillage, rien par image),
  sub_821F3F28 et sub_821A6D90 (anneau et registres du GPU émulé), sub_821BA040 (Direct3D du jeu), sub_821DA090 (déjà en parallèle), sub_821D5410, bibliothèque C, inflate.
  Essai : dist/torchlight-essai-natives.nro (MD5 867842b7), ELF switch/elf/torchlight-essai-natives-20261103-2043.elf.
  **Mesure sur la console (XJE1, 1020/307,2/1600 MHz, alternance natives,rendu toutes les 2 s, cœur réservé, billboards et poses actifs ; session de 5 min 30 sans plantage) :**
  switch-logs-archive/20261103-essai-natives/ab_regression.txt. Effet des natives, en % avec marge à 95 % (négatif = mieux) :
  ville (23 fenêtres, 243 dessins, 21,10 ms) écart -0,15 ± 0,96, image +1,78 ± 1,21 ; mine (24 fenêtres, 178 dessins, 18,60 ms) écart +0,01 ± 0,58, image +1,67 ± 0,75 ;
  combat (58 fenêtres, 276 dessins, 32,18 ms) écart -1,28 ± 4,19, image -0,19 ± 2,64 : non concluant. **Aucun gain mesurable, le temps de l'image est plutôt un peu moins bon.**
  Poses du rendu, même session : ville +0,05 ± 0,96, mine -0,33 ± 0,58, combat -1,09 ± 4,22 (image -2,95 ± 2,66).
  Hypothèse non vérifiée : le recompilé est entraîné par le profil PGO et rangé par switch/ordre_fonctions.ld, pas ces natives, donc l'A/B les désavantage. Essai possible : instrumenter avec
  l'alternance active pour entraîner les deux côtés, puis refaire l'A/B. Autre explication possible : le code généré à registres locaux vaut déjà une traduction à la main.
- 2026-11-03 nuit : **session PGO avec l'alternance natives,rendu, puis même A/B.** Instrumentée : dist/torchlight-pgo-natives.nro (f6448c6c, contient des chemins locaux : à ne pas distribuer), 33 min, 10 vidages,
  248 fenêtres par combinaison, sans plantage. Profil de la 1.0.4 gardé dans pgo-104/. Ordre des fonctions : 19 natives absentes de la liste (sub_821C0E50 à 42 Mo), ajoutées par switch/tools/ordre_natives.py (édition des
  liens seulement). Avec le profil, le chemin de validation sort du code chaud : sub_821CCF30 passe de 68 Ko à 10 Ko ; natives à 3,6 Mo au plus. Aucune incompatibilité de profil.
  Mesure (build 80008c80, 1020/307,2/1600 MHz, 8 min 15 sans plantage ; switch-logs-archive/20261103-essai-natives-pgo/ab_regression.txt), natives dans la session : ville -6,24 ± 1,42 %, mine -4,48 ± 0,73 %, combat
  -7,32 ± 3,50 % (paires voisines : -9,5 ± 3,1 %) ; temps de l'image -7,0 ± 1,2, -3,2 ± 1,0, -5,8 ± 2,4 ; combat, images > 33 ms : 25,2 % -> 14,2 %.
  **Piège : cet écart surestime le gain.** Temps absolus, écart entre images (recompilé profil 1.0.4 / natives profil 1.0.4 / recompilé nouveau profil / natives nouveau profil) : ville 21,11 / 21,08 / 21,69 / 20,34 ms ; mine
  18,60 / 18,60 / 19,18 / 18,32 ms ; combat 30,83 / 29,89 / 30,32 / 28,24 ms (combats différents, non comparable). Avec ce profil le recompilé ralentit de ~3 % à l'arrêt (cause non cherchée) : une alternance entraînée des deux
  côtés n'est pas neutre. Gain plus crédible contre l'état sans natives : ville ~-3,7 %, mine ~-1,5 % (de session à session, à mêmes endroits, marge non chiffrée). Toujours regarder les temps absolus de chaque côté, pas seulement la différence.
  Poses du rendu : écart ville -0,45 ± 1,44, mine -0,09 ± 0,73, combat +1,78 ± 3,54 ; p99 du combat +15,5 ± 14,5 %, et +21,1 ± 19,4 % à la session précédente ; images > 33 ms 15,7 % -> 23,9 % (21,5 % -> 30,5 % avant) : à couper.
  Si les natives restent : une dernière session avec elles seules. Pour ne la faire qu'une fois, retirer d'abord sur la Switch les tests Pilote() et les chemins de validation (le profil vaut pour le code exact). Juger alors de session à
  session contre 21,11 / 18,60 ms, aux mêmes endroits : le recompilé n'étant plus entraîné, un A/B dans la session serait faussé.
  Plan accepté par l'utilisateur (aucun défaut vu dans les menus et les dialogues pendant ces sessions). Fait : option -DTORCHLIGHT_NATIVES_FIXES (Switch, ON par défaut) : Mode() rend 1, g_pilote_actif constant, le reste
  disparaît à la compilation (plus de « validation : {} appels » dans le binaire) ; instrumentée dist/torchlight-pgo-natives-seules.nro (87b242d8), sur la console avec dist/torchlight-natives-seules.toml (3e75b73e) ;
  sub_821C0E50 tombe à 340 octets même instrumentée. Profil de l'alternance gardé dans pgo-alternance/. Outil : switch/tools/ab_absolu.py (temps absolus par scène et par état, avec ou sans alternance ; redonne bien les
  chiffres ci-dessus). **Session PGO pas encore jouée ; suite : CLAUDE.md, « REPRENDRE ICI ».**
  Pièges : attendre une build avec une boucle `pgrep -f build_switch` ne finit jamais (la boucle se détecte elle-même) : lancer la build elle-même en arrière-plan. `console.sh pgo` efface pgo/ avant la copie : garder
  une copie d'abord. Sans `find` des objets plus anciens que le profil, ninja garde l'ancien profil.

### 2026-11-04 : natives seules avec leur propre profil
- Session PGO jouée (37 min, 11 vidages, build 87b242d8, sans plantage ; dernier vidage ~35 s avant la fin ; switch-logs-archive/20261104-pgo-natives-seules/). Build de mesure : dist/torchlight-essai-natives-seules.nro (178f89a2),
  16 min, 0 « coverage mismatch », 242 objets sur 243 avec leur .gcda (torchlight_init.cpp n'en a jamais eu), ELF switch/elf/torchlight-essai-natives-seules-20261104-0927.elf.
- Mesure (switch-logs-archive/20261104-essai-natives-seules/, ab_absolu.txt ; 1020/307,2/1600 MHz, boost seulement au démarrage et au chargement ville -> mine ; 6 min 30 sans plantage), écart entre images, de session à session :
  ville 21,11 -> 19,21 ms (-9,0 %, 243 dessins des deux côtés ; 47,4 -> 52,1 images/s) ; mine 18,60 -> 17,47 ms (-6,1 %, 185 dessins contre 178 ; 53,8 -> 57,2). Dispersion des fenêtres de 2 s : ± 0,06 et ± 0,03 ms ; pas de marge
  chiffrée entre sessions (pas d'A/B). Temps de l'image : ville 16,53 -> 14,99, mine 13,51 -> 13,40 ; « reste » (surtout logique) : 4,58 -> 4,22 et 5,09 -> 4,07. Contre les natives au profil de l'alternance : -5,6 % et -4,6 %.
  Après la mine (09:35:45-09:38:38, combats et déplacements mêlés, 248 dessins en moyenne) : 26,33 ms, 7,3 % d'images > 33 ms ; non comparable aux 28-31 ms d'avant (combats différents).
  Contre la 1.0.4, temps de l'image aux mêmes dessins : ville ~17,5 -> 14,99 (-14 %), mine ~14,6 -> 13,40 (-8 %), mesures d'il y a plusieurs sessions (cœur, recyclage, billboards et poses compris dans cet écart).
- Pièges : les journaux s'accumulent (torchlight_001.log resté de la session d'avant, mesure dans torchlight_002.log) : regarder le plus récent. Dans rex_perfil.log, le « 10.0 s » de l'en-tête est l'intervalle du bilan, pas le temps écoulé.
  -Wno-missing-profile fait taire un .gcda absent : comparer les noms des objets et de pgo/.
- Pistes sur le trafic mémoire, non testées (question de l'utilisateur : 7,2 lu sur un overlay en jeu ; Status Monitor et l'overlay Horizon-OC affichent des GB/s, soit 28 % des 25,6 Go/s théoriques à 1600 MHz). Le jeu est sensible à la mémoire :
  1331 -> 1600 MHz ville -5,7 %, mine -5,1 % (sessions du 2026-11-03, kip Horizon-OC chargé entre les deux) ; CPU +20 % ne donne que +12 % d'images/s : environ un tiers du temps ne suit pas le CPU (estimation) ; 2400 MHz -27 % dans la mine
  (une session ancienne, ne concorde pas). Envoi vers le GPU : ville 4637 Ko/image (241 Mo/s), mine 8918 Ko/image (508 Mo/s). (1) `NVK_SWITCH_ZCULL=0`, défaut du .nro et de torchlight.toml, reste d'un test du 2026-09-30 sous émulation Xenos,
  jamais refait en rendu natif : essai possible sans recompiler (mesa_env = "" dans le .toml), risque : blocage du GPU. (2) NVK sur la Switch ne compresse que les images à allocation dédiée (nvk_device_memory.c) et src/natif/rendu.cpp n'en demande
  aucune : cibles de rendu probablement non compressées, à vérifier à l'exécution. Gain sur le thread principal seulement si le trafic du GPU retarde ses accès mémoire, ce qui n'est pas prouvé (attente du rendu ~0).
  Relevé de l'utilisateur sur l'overlay (ouvert à chaque test, donc comparaisons valables) : CPU toujours dominant, GPU à 70 % ou moins : ces deux pistes passent après. Piste côté CPU, jamais mesurée : le coût des défauts de TLB du thread principal. Le SDK
  confirme la mémoire invitée par blocs de 2 Mo pour que le noyau puisse employer des descripteurs de bloc, mais rien ne vérifie qu'il le fait (vues par svcMapProcessMemory, pages isolées, ~1430 pages protégées). Test possible : micro-mesure sur la
  console, une ligne par page sur ~4000 pages (tient dans le cache L2, pas dans le TLB), en mémoire invitée et en mémoire de l'hôte. Le préchargement déjà essayé sur les contrôleurs ralentissait (2026-10-21) : ne pas le refaire tel quel.

### 2026-11-04 : défauts de TLB, fenêtre du jeu jamais alignée sur 2 Mo
- Module de mesure src/torchlight_tlb.cpp (-DTORCHLIGHT_MESURE_TLB=ON, jamais pour les joueurs ; build_joueurs.sh force OFF, le cache CMake de switch/out-joueurs le garde sinon). Première version fausse (switch-logs-archive/20261104-essai-tlb/) : motif
  régulier, 64 ensembles du cache L2 sur 2048 en mémoire continue, donc surtout la RAM ; le tas lui-même montrait +135 ns. Tirage aléatoire ensuite.
- Mesure corrigée (dist/torchlight-essai-tlb2.nro, switch-logs-archive/20261104-essai-tlb2/analyse.txt, 2048 lignes, écart dispersé - groupé) : tas de l'hôte +7,5 ns (blocs : 810 Mo puis 68 Mo continus, virtuelle et physique alignées pareil) ;
  mémoire du jeu +140 à +165 ns dans les grandes zones (0x40000000, 0xF0000000, 0x110000000), +58 à +90 ailleurs ; code du .nro +96 ns en lecture, +148 ns en exécution. Un défaut de TLB y coûte autant qu'un accès à la RAM.
- Cause pour la mémoire du jeu : **virtmemFindAslr(size, guard_size) : le second paramètre est une garde, pas un alignement.** RexGmInit lui passait 2 Mo : la fenêtre n'a jamais été alignée, le noyau n'y a jamais employé de blocs, malgré les
  morceaux de 2 Mo du SDK. Indices concordants : deux tranches trouées par zone (début et fin), adresses du témoin non alignées (même erreur de ma part). Corrigé dans guest_memory_switch.cpp (2 Mo de plus, base arrondie). Sans effet sur le code généré : le profil PGO reste valable.
- Le code du .nro (50 Mo) reste en pages de 4 Ko probables : le chargeur le place à une adresse quelconque (reste sur 2 Mo 0x175000 puis 0x55000) ; correction plus lourde, à voir après. svcQueryPhysicalAddress refuse le code et les vues du jeu (0xD401), pas le tas.
- La vue 0xE0000000 cible 0x100001000 : si le masque de xmemory.cpp ne retire pas ce décalage de 4 Ko, elle reste en pages de 4 Ko même après la correction : à regarder dans la mesure suivante (zone 0x0F0000000).
- Mesure avec la fenêtre alignée (dist/torchlight-essai-tlb3.nro, 85daf7f2, ELF switch/elf/torchlight-essai-tlb3-20261104-1055.elf ; switch-logs-archive/20261104-essai-blocs2m/, 8 min sans plantage, overlay ouvert comme dans la référence) :
  fenêtre 0x8eaa00000, reste sur 2 Mo 0. Témoin, mêmes lignes physiques : aligné 53,0 ns, décalé de 4 Ko 130,0 ns, **+77 ns dus au seul TLB**. Zones du jeu (2048 lignes) : 0x40000000 +165 -> +8 ns, 0x110000000 +155 -> +7, 0x80000000 +90 -> +7, 0xD0000000 +60 -> +2,
  0x60000000 +58 -> -2. **0xF0000000 (113 Mo) reste à +126 ns** (décalage de 4 Ko de la vue 0xE0000000). Code du .nro inchangé : +81 ns en lecture, +115 ns en exécution. Blocs du noyau : au plus 2271 (2350 avant), aucune erreur.
  Temps (1020/307,2/1600 MHz, boost hors des fenêtres), contre dist/torchlight-essai-natives-seules.nro, de session à session : ville 19,21 -> 17,70 ms (-7,9 %, 243 -> 249 dessins ; 52,1 -> 56,5 images/s), image 14,99 -> 13,92 (-7,1 %) ; moitié stable de la minute
  19,08 -> 17,39 (-8,9 %). Mine : **plafond de 60 images/s atteint** (écart 16,65-16,69 dans toutes les fenêtres : l'écart ne mesure plus rien), image 13,40 -> 10,72 ms (-20,0 %, 185 -> 186 dessins). Après la mine : 21,32 ms, 8,9 % > 33 ms en 1 min 50 : non comparable.
  Contre le recompilé du profil 1.0.4 : ville 21,11 -> 17,70 ms (-16 %), mine (image) 13,51 -> 10,72 ms (-21 %). Le code généré n'a pas changé : profil PGO valable, 0 « coverage mismatch ».
- Restent en pages de 4 Ko, chantiers plus lourds : (1) vue 0xE0000000 : un bloc y demanderait une mémoire physique décalée de 4 Ko, donc perdre les blocs des vues 0xA0000000, 0xC0000000 et 0x100000000, et un morceau de 2 Mo y chevauche deux tranches ; (2) code du .nro : le chargeur le place à une adresse
  quelconque ; il faudrait le reprojeter au démarrage, au moins au même endroit avec une source alignée pareil, et rétablir l'ancienne projection pour hbloader à la sortie. Le même bug existe dans le SDK de nfsmw-nx (RexGmInit).

### 2026-11-04 : code du .nro en blocs de 2 Mo (src/torchlight_code_blocs.cpp, -DTORCHLIGHT_CODE_BLOCS, ON par défaut)
- Demande de l'utilisateur : traiter les pages de 4 Ko restantes, à mettre dans la 1.0.5 si c'est faisable. Reprojection à la même adresse virtuelle dans un constructeur de priorité 101 (premier thread du SDK à la priorité 102), source retrouvée comme l'unique bloc du tas marqué emprunté de la taille
  du .nro, copie de même reste sur 2 Mo que la cible, relais en assembleur dans sa propre page exécutable (retrait, pose, droits, cache d'instructions), pile hors du .nro. Pas de retour à hbloader à la sortie (ISelfController::Exit, rex_app.cpp) : rien à rétablir. Les 83 Mo de l'ancienne source sont perdus.
  Échec avant le retrait : jeu inchangé. Ligne « [code] » du journal.
- Mesure (dist/torchlight-essai-code2m.nro, 1a337cc0, ELF switch/elf/torchlight-essai-code2m-20261104-1127.elf ; switch-logs-archive/20261104-essai-code2m/, 8 min sans plantage) : « fait », cible à reste 0x1e2000, 83648 Ko, 4 segments. Code : exécution +115 -> +7 ns, lecture +81 -> +5 ns (2048 lignes).
  Contre la fenêtre alignée seule : ville 17,70 -> 17,29 ms (-2,3 %, 249 -> 253 dessins), image 13,92 -> 13,73 (-1,4 %) ; mine au plafond, image 10,72 -> 10,33 (-3,6 %). Gain modeste, réel : la fenêtre du jeu comptait bien plus. Reste à vérifier : la sortie par le menu du jeu.

### 2026-11-04 : vue 0xE0000000 en blocs de 2 Mo (en cours)
- La vue vise 0x100001000 : un morceau de la mémoire physique commence maintenant à 0x...1000 (GridDown / GridUp de guest_memory_switch.cpp) et la vue physique directe est décalée de 0x1FF000 dans la fenêtre (kPhysicalViewOffset, xmemory.cpp, Switch seulement), car le relevé du rendu la lit sur le thread principal
  (TranslatePhysical, src/torchlight_invite.h). Perdent leurs blocs : vues 0xA0000000, 0xC0000000, 0x7F000000 (~27 Mo), dont les adresses sont imposées par le jeu. Le code généré ne change pas : profil PGO valable.
- Mesure (dist/torchlight-essai-e0.nro, 679ff17a, ELF switch/elf/torchlight-essai-e0-20261104-1150.elf ; switch-logs-archive/20261104-essai-e0/, 9 min sans plantage, aucun défaut de texture ou de modèle vu) : zone 0xF0000000 +132 -> +12 ns ; 0x110000000 reste à +8 ; 0xD0000000 -1 -> +61 (attendu).
  Contre le code en blocs : ville 17,29 -> 16,72 ms (59,8 images/s, presque au plafond ; 253 -> 246 dessins), image 13,73 -> 13,51 ; mine au plafond, image 10,33 -> 10,72 (187 -> 193 dessins). Ramené au dessin, le temps de l'image ne bouge pas à l'arrêt : gain non démontré ici, dessins trop différents et plafond.
- Combats, par régression sur les fenêtres de 2 s hors plafond (écart et image à 250 puis 300 dessins, marge du tirage ± 0,2-0,3 ms, combats différents donc marge réelle plus grande) : natives seules 26,64 / 32,10 et 18,35 / 21,24 ; fenêtre alignée 22,19 / 27,24 et 15,94 / 18,70 ; code 21,65 / 26,21 et 14,98 / 17,55 ;
  vue E0 21,13 / 26,08 et 14,50 / 17,60. Soit, sur l'écart : fenêtre -15 à -17 %, code -2 à -4 %, vue E0 0 à -2 % (dans la marge) ; total -19 à -21 %. Vue E0 gardée : sans régression, gain faible ou nul à démontrer. Blocs du noyau 2379 au plus, 514 Mo au plus (session plus longue, zones différentes).
- Fin du travail sur les pages de 4 Ko : restent ces ~27 Mo, le début et la fin des segments du .nro. Préparation de la 1.0.5 : valeurs par défaut (cœur 2, billboards 2, poses 2, poses_double) dans torchlight_app.h et switch/torchlight.toml, TORCHLIGHT_VERSION.

### 2026-11-04 : relevé des piles de la 1.0.5 (switch-logs-archive/20261104-pilas-105/, pilas-archive/rex_pilas-20261104-1342.txt, ELF switch/elf/torchlight-joueurs-1.0.5-20261104-1216.elf)
- Session de 7 min avec le profileur, sans torchlight.toml : valeurs par défaut actives (masque 0x4, « [code] fait »), sans plantage. Le thread principal n'a que ~20 échantillons/s : 919 par minute immobile, 3985 en combat. Bilans de ~15,2 s avec le profileur (le « 10.0 s » et les % de CPU sont alors faux d'autant).
- **Attente active au plafond** : sub_821F31C8 (présentation) -> sub_821A5C10 (fence du Direct3D du jeu, boucle) : 17,85 % dans la mine, 2,18 % en ville, 0 % en combat. D'où un thread à 99,5 % à 60 images/s : à l'arrêt, le 100 % de l'overlay ne veut plus dire saturé. Remplaçable par un vrai sommeil (crochet déjà en place, torchlight_trace.cpp) : aucune image gagnée, moins de charge.
- Combat, arbre (1 % = 0,21 ms) : rendu 70,4 % (caméra principale 47,2 % : files 23,6, interface 6,9, objets visibles 14,4 ; secondaires 22,4 : contrôleurs 9,8, visibles 4,8, nœuds 2,9, files 2,8) ; logique 29,4 % (unités sub_821D3FD8 11,2 ; poses 6,9, dont 3,5 de tâches faites par le thread principal ; sub_821A40E0 3,5). Profil plat : plus haute fonction 3,84 % (walkOctree, déjà native).
- **Par instruction (switch/tools/profil_instructions.py) : 66,5 % des échantillons sur une lecture entière, 4 % sur une lecture flottante, 10,6 % sur une écriture entière** (mêmes proportions dans les trois scènes). Le thread attend surtout des données ; c'est une attribution d'échantillonnage, pas une mesure de cycles.
- Écartés par ce relevé : changements de mode flottant (22 833 sites, 0,30 % : piste close), malloc de l'hôte 0,3 %, verrous 1,0 % + attente des aides 0,9 %, BaseHeap::AllocRange 0,5 %. dcbt déjà écartés (2026-10-20).
- **Piste principale : 98-99 % de la géométrie envoyée à chaque image n'a pas changé.** Relevé / envoi, Ko par image : ville 34 copiés, 4472 repris, envoi 4640 ; mine 28, 8653, 8835 ; combat 70, 9913, 10236. Le thread de rendu reconvertit et repousse tout (rendu.cpp, Draw -> Push, indices et sommets), soit 2 à 5 fois la taille du cache L2 partagé (2 Mo) par image, et ~1 Go/s de trafic mémoire en lectures et écritures. Un tampon GPU persistant par géométrie inchangée (clé CopyRange, compteur de Lock) le supprimerait, soulagerait le thread de rendu (9,5-13 ms) et le GPU.
  Non prouvé : le thread principal n'attend pas le rendu (0,02 ms), il ne gagnerait que par moins d'expulsions du L2 et moins de trafic. Test d'abord : alterner dans la session (torchlight_alterner) un état où le thread de rendu ne fait rien (écran figé), pour lire le gain maximal sur le temps de l'image, aux mêmes endroits.
- **Test du thread de rendu coupé (2026-11-04, dist/torchlight-essai-sans-vulkan.nro, ec115e91 + .toml, switch-logs-archive/20261104-essai-sans-vulkan/ab_regression.txt ; alternance « vulkan », 2 s actif, 2 s images jetées, écran figé) : le thread principal va bien plus vite quand le rendu ne touche plus à la mémoire.**
  Paires voisines, temps de l'image : ville -16,7 ± 0,7 % (13,14 -> 10,93 ms), mine -17,6 ± 0,4 % (10,50 -> 8,65 ms), combat -14,4 ± 1,6 % (15,83 -> 13,72 ms) ; écart en combat -15,4 ± 2,0 % (22,50 -> 19,13 ms, 44 -> 52 images/s), images > 33 ms 2,2 % -> 0,5 %. C'est un plafond : il compte tout le thread de rendu et tout le GPU, et n'en sépare pas les parts (cache L2, trafic du GPU).
- Géométrie persistante écrite : numéro de version dans GeometryCopy, donné au rendu à partir de la 2e utilisation sans Lock (VertexStream / IndexStream.version) ; le rendu garde la conversion dans 3 arènes de 32 Mo (RendererImpl : PushPersistent, FindGeometry, kGeomArena), la plus ancienne reprise quand elles sont pleines, tampon de l'image à défaut. --torchlight_geometrie (vrai), alternance « geometrie », --torchlight_geometrie_verif=N (1 plage sur N reconvertie et comparée : « différentes » dans le bilan [natif]).
- Première mesure de la géométrie persistante (dist/torchlight-essai-geometrie.nro, d00d375a + .toml, switch-logs-archive/20261104-essai-geometrie/ab_regression.txt, alternance 2 s / 2 s, aucun défaut vu) : 12 016 plages reconverties et comparées, 0 différente. Paires voisines : ville image -6,5 ± 0,7 % ; mine image -4,6 ± 1,4 % ;
  combat image -2,7 ± 2,1 %, écart -5,4 ± 2,1 % (23,67 -> 22,36 ms), images > 33 ms 10,2 -> 5,7 %. Sous-estimé hors de la ville : défaut de la 1re version : arènes pleines dès le chargement (3 x 32 Mo) et jamais reprises tant qu'une de leurs plages servait (plages figées à 7109 dans la mine, 3,2 Mo servis sur 8,9). Corrigé : l'arène reprise sort ses plages de la table, puis attend les images en vol (free_at).
- Attente active : crochet de sub_82774170 (un tour de l'attente du GPU, torchlight_trace.cpp), pause de 100 µs (svcSleepThread, --torchlight_attente_dort) à la place du db16cyc recompilé en rien ; six appelants déjà en sub_X dans le code généré, rien à y changer, profil du jeu intact. Boucle de sub_821A5C10, envoi au GPU et détection de blocage de 5 s inchangés.
- Mesure de la géométrie corrigée et de la pause (dist/torchlight-essai-geometrie2.nro, bf2fc19c + dist/torchlight-essai-geometrie.toml, switch-logs-archive/20261104-essai-geometrie2/ab_regression.txt, 6 min 20 sans plantage ; géométrie en alternance 2 s / 2 s, pause toujours active) : 14 670 plages comparées, 0 différente ; plages qui évoluent dans la mine, 8,8 Mo servis sur 8,9 quand c'est actif, une reprise d'arène à l'arrivée. Paires voisines, temps de l'image : ville -7,4 ± 2,4 %, mine -7,0 ± 0,8 %, combat -2,7 ± 2,2 % ; écart en combat -4,7 ± 2,1 % (régression -6,3 ± 2,4 %), 23,61 -> 22,29 ms, images > 33 ms 7,5 -> 3,1 %. Soit ~40-45 % du plafond mesuré en coupant tout le rendu : le reste est le reste du thread de rendu et le GPU, non séparés (si le GPU pèse, ZCULL et compression des cibles redeviennent intéressants).
  Pause : thread principal 99,5 -> 80 % dans la mine à 60 images/s (CPU total 186 -> 152 %), 99,5 -> 96,5 % en ville, ~99 % en combat ; plafond tenu (écart 16,65-16,83 ms), maximum médian par fenêtre 19,1 -> 19,8 ms dans la mine, négligeable.
- **Ralentissement au début du premier combat (signalé par l'utilisateur, ~15 images/s)**, chronologie (session bf2fc19c, torchlight_011.log) : 15:30:49,04 une image de ~270 ms (invisible dans « [coeurs] » : l'image à cheval sur deux fenêtres n'est pas comptée, mais la fenêtre dure 1254 ms au lieu de 1000) ; 49,29 le mode boost la prend pour un chargement, GPU à 76,8 MHz ; 49,38-49,83 une image toutes les ~55 ms (~18 images/s), avertissements Mesa « submission pressure » ; 49,83 fin du boost ; fenêtre suivante : thread principal bloqué ~180 ms sur le rendu en retard. Puis 26-34 images/s, 334-362 dessins : scène lourde, pas un blocage. Mêmes événements de 0,8 s : 13:39:11, 13:39:52, 13:40:34, 15:32:04.
  Corrigé : kSeuil du boost de 250 à 400 ms (images en jeu vues jusqu'à ~300 ms). Les 270 ms eux-mêmes ne sont pas expliqués : pendant ce temps le thread de rendu attend, le thread principal travaille sans qu'une fonction domine (échantillons variés du rendu, une allocation physique MmAllocatePhysicalMemoryEx), ni faute de mémoire ni NvMap ; 46-50 textures créées dans le bilan. Hypothèse : première apparition d'un type de monstre ou d'effet. Pour conclure : journaliser les images longues, avec temps de logique et de rendu, compilation de shaders, allocations et lectures de fichiers.
- Essai du ZCULL (dist/torchlight-joueurs-1.0.5-20261104-1606.nro, 323f2ee6 + dist/torchlight-essai-zcull.toml, switch-logs-archive/20261104-essai-zcull/, 5 min, aucun blocage ni défaut vu ; attention : mesa_env = "" ne l'active pas, la valeur par défaut du .nro reprend la main, il faut NVK_SWITCH_ZCULL=1) : plans vus dans rex_stderr.log (65 504 octets pour 1280x720). Temps de l'image contre la référence, de session à session : ville 12,47 -> 12,32 ms, mine 10,03 -> 9,99 (189 dessins contre 186) ; combat à dessins égaux, écart 20,62 -> 21,18 et 25,49 -> 25,60, image 14,81 -> 15,06 et 17,40 -> 17,68 : aucun gain pour le thread principal. GPU à 75-80 % sur l'overlay de l'utilisateur, sans référence à 60 images/s. Gardé coupé. Le temps Vulkan n'est pas comparable (la référence alternait la géométrie).
- **Correctif du boost vérifié : aucune pression de Mesa ni aucun boost en combat** (143 avertissements, tous aux chargements). **Le blocage du début de combat est dans la logique** (« [image longue] », nouveau journal) : 16:13:51 101 ms (reste 82), 16:13:52 283 ms (image 23, attente 0, reste 260), 16:13:57 73 ms (reste 59), 16:14:08 95 ms (reste 71). Un autre type à 16:14:07 : 149 ms, dont 121 d'attente du rendu. Correction de l'analyse précédente : l'attente du rendu à 15:30:50 venait surtout du GPU bridé par le boost.
- Relevé suivant : -DTORCHLIGHT_... inchangé, dist/torchlight-essai-reference.toml et console.sh pilas-on ; thread principal échantillonné ~250 fois/s, « T: » devant le thread dans rex_pilas.txt pour un échantillon pris pendant une image de 45 ms et plus (RexSwitchPerfTiron, activé par --torchlight_images_longues).
- Relevé des images longues (323f2ee6 + dist/torchlight-essai-reference.toml, switch-logs-archive/20261104-pilas-images-longues/, pilas-archive/rex_pilas-20261104-1628.txt, session courte, 1 min 30 en jeu) : hors chargement, images >= 60 ms : 3,6 s, moitié rendu, moitié logique. Sur-représentées pendant ces images : recherche de chemin (sub_8228F8C8 -> sub_8221F100 -> sub_8221F8B8 -> sub_8221FB00 -> sub_8221FEB0, 7,7 % de temps propre) ; sub_82307130 -> sub_82306A70 -> sub_823BA4D8 -> sub_823B9D18 (12,5 % contre 0,1 % ailleurs, sous sub_821D8238, rôle inconnu) ; sub_823AE748 -> ... -> sub_82272288 -> ... -> sub_823DE3F8 (5,8 % contre 0). **Mais l'échantillonneur ralentit d'environ 4 fois pendant ces images (~285 échantillons/s au lieu de 1000 ; 31 du thread principal pour 1212 ms au lieu de ~220) : indice d'un temps passé dans le noyau, où il ne lit pas la pile (non prouvé).** Le relevé seul ne conclut donc pas.
  Chronomètres ajoutés (SDK : RexChrono, guest_memory_switch.h ; RexGmCommit, FaultIn, Protect, Decommit ; MmAllocatePhysicalMemoryEx, MmFreePhysicalMemory, NtAllocateVirtualMemory, NtFreeVirtualMemory, NtReadFile, NtCreateFile ; D3DXCompileShader) : la ligne « [image longue] » donne appels et ms de chacun depuis l'image précédente, tous threads confondus, plus relevé et textures. Activés par --torchlight_images_longues.
- Chronomètres (dist/torchlight-joueurs-1.0.5-20261104-1642.nro, 134af282 + torchlight-essai-reference.toml, switch-logs-archive/20261104-chronos/) : 27 images >= 60 ms hors chargement, 3572 ms : shaders 128 ms, lectures 86, allocations physiques 39, ouvertures 11, commit / protect / fautes 0 ms ; 3308 ms non expliqués par ces postes. Hypothèse du noyau par la mémoire écartée. Dans le relevé de 16:28, thread principal 103 échantillons sur 104 hors appel système, autres threads au repos : travail en code utilisateur, ou un ralentissement mal compris de l'échantillonneur.
  **Blocages identifiés : (1) enregistrement du cache de pipelines sur le thread de rendu : 16:45:14,075 134 ms, 16:45:34,164 172 ms (attente du rendu) ; envoyé sur le thread de compilation. (2) Première rencontre : 16:45:01, 275 ms de logique avec 273 ouvertures et 86 lectures de fichiers (47 ms) : chargement de données du jeu sur le thread principal, le reste étant sans doute leur lecture.** Restent inexpliqués : 170-180 ms dans le rendu (16:45:22, :37, :41), 143 et 111 ms dans la logique.
- Demande de l'utilisateur : tout traiter avant la 1.0.5, y compris les blocages. Chien de garde ajouté (torchlight_coeurs.cpp, --torchlight_images_longues) : dès qu'une image dépasse 40 ms, pile du thread principal toutes les 2 ms, durée de chaque mise en pause et chemin de chaque fichier ouvert dans logs/rex/rex_longues.txt. Aucun coût entre les images longues.
- Chien de garde (dist/torchlight-joueurs-1.0.5-20261104-1710.nro, eae1214f, switch-logs-archive/20261104-chien/, 2916 échantillons, pause médiane 8 µs) : **causes nommées**. (a) 17:12:58, 286 ms, première rencontre : chargement du squelette (modèle, sons, arme : 273 ouvertures), sous sub_823AE748 ... sub_823DE1A8 -> sub_822883F0 -> sub_8228B4B8 : décompression 17 %, lectures 14 %, reste analyse et allocations ; pareil, moins fort, au premier objet lâché (or, potion, masse : 114-145 ms). (b) 17:13:33, 181 ms : **RibbonTrail::updateTrail (sub_821E30A0)** : un élément par longueur entre l'ancienne et la nouvelle position, alors que l'anneau n'en garde que total ; saut ajouté dans la native au-delà de 4 x total longueurs, 2 x total tours gardés (éléments identiques à l'arrondi près). (c) 17:13:08, 94 ms : le thread principal écrivait le journal (stdio plein -> fsdev_write) ; bilans périodiques coupés sauf --torchlight_journal_stats. Dans la session, écriture du journal : 1,1 s d'un bloc de chargement de 2,3 s. (d) À l'arrivée, ~1,1 s à 5 images/s, 2300 dessins : premières images, matériaux chargés à la volée.
- Décompression native : src/torchlight_inflate.cpp, inflate (sub_823E1590) par la zlib 1.3.1 de la console, flux associé par l'adresse du z_stream, init / reset / fin d'origine gardés ; --torchlight_inflate_verif compare les deux. Lecture en avance reportée (peu de gain attendu sur de petits fichiers épars). Boost court en jeu refusé par l'utilisateur.
- Autres pistes : aides des particules et poses (le thread principal fait 7,7 % lui-même, 1,6 ms, alors que les cœurs 0 et 1 ont plus d'un cœur libre : recouvrir au lieu d'attendre, risque d'état partagé, switch/tools/suivi_memoire/) ; interface 6,9 % + sub_821A40E0 3,5 % (réutiliser le tracé quand rien ne change, risque d'interface périmée) ; ordre des fonctions à refaire (switch/ordre_fonctions.ld).
- Fonctions d'analyse des données (demande de l'utilisateur, 2026-11-04 soir : « ça ne sortira jamais dans un profil de piles mais c'est un vrai point de contention »). Lecture du code sur les 560 échantillons du chien de garde pris pendant les chargements : le lecteur de données (sub_82257C10, sub_822574F0, sub_8224F068 : format binaire, nœuds récursifs, chaînes UTF-16) n'a presque pas de temps propre (< 0,5 % chacun) ; le coût est dans les feuilles. Trois découvertes, plus utiles que la réécriture des parcours :
  - **Verrou global** : le générateur traduit mfmsr / mtmsrd par un std::recursive_mutex (REX_CHECK_GLOBAL_LOCK : prise et rendu, REX_ENTER / LEAVE : prise et rendu, 2 barrières, 2 compteurs atomiques). Les 185 sections du code ne contiennent que lwarx/ldarx, stwcx./stdcx. et des opérations de registres, et stwcx. est déjà un vrai compare-and-swap : le verrou ne protège rien. 2,2 % du thread principal en moyenne, 6,4 % des chargements rien que sous le malloc du jeu (sub_821CD7F8, chemin lent), et les threads de travail s'y disputent avec le thread principal. switch/tools/atomiques_sans_verrou.py (build_switch.sh) vide ces lignes sur place, numéros de ligne gardés, en refusant une section qui contiendrait autre chose. Les 121 fonctions touchées perdent leur profil PGO : leur graphe change. Le verrou reste pour le noyau du SDK.
  - **Lectures du zip** : un squelette se lit par 4, 16 puis 12 octets par image clé, et chaque lecture appelait inflate avec une toute petite sortie, donc son chemin lent, octet par octet (inflate_fast demande 258 octets) : inflate pesait plus qu'inflate_fast, 29 des 62 échantillons de readKeyFrame (sub_8250E0A0). sub_8258A068 (zzip_file_read) réécrit dans src/torchlight_inflate.cpp : décompression par blocs de 64 Ko dans un tampon de l'hôte, lectures servies par copie, lectures de 32 Ko et plus directes. restlen (+12) compte les octets pas encore livrés (zzip_tell et fin de fichier justes) ; inflateReset vide le tampon (retour en arrière). Vérification : --torchlight_zip_verif et switch/tools/verif_zip.py (taille et CRC32 contre le dossier de pak.zip).
  - **Feuilles** : src/torchlight_donnees_natif.cpp, --torchlight_donnees_natif 0/1/2 (2 = comparé à chaque appel ; les chaînes sont refaites sur une copie dans le tas du jeu) : unpackColour / packColour d'Ogre (recoloration des textures par sub_822DC6B0, pixel par pixel), png_read_filter_row, décodeur IMA ADPCM de FMOD (tables du jeu vérifiées : formule standard), std::wstring (constructeur, deux assign, _Grow, _Copy, destructeur, compare, wcslen), AnimationTrack::createKeyFrame. Fichier sans profil PGO. Laissés : parcours du lecteur (peu de temps propre), sub_8250E0A0 et sub_8250DF08 (appels virtuels à garder, gain faible une fois les lectures en tampon), chaînes étroites sub_821BFF10 et sub_821BDDE8 (0,9 % à deux).
  - Estimation sur ces 560 échantillons, à vérifier : décompression -7, verrou -6 à -8, pixels -3, chaînes -4, ADPCM et PNG -2,5, plus le tampon du zip : **un quart à un tiers du blocage, pas sa disparition** (286 ms vers ~200 ms). Restent les lectures du fichier (10 %, svcSendSyncRequest : seule une lecture en avance les retirerait) et le chemin lent de malloc.
- **Le contrôle « 0 coverage mismatch » ne prouve rien pour le code généré** (trouvé le 2026-11-04) : DEFINE_REX_FUNC vient d'un en-tête système du SDK, et GCC masque les avertissements placés dans une macro d'en-tête système ; une fonction dont le graphe a changé perd son profil sans un mot (la vérification du 2026-11-01, torchlight_recomp.50 avec -Wcoverage-mismatch, ne valait donc pas non plus). Outil : switch/tools/audit_profil.py (-Wsystem-headers, sur chaque fichier). Sur la build 612a81f5 : 144 fonctions sur 43 815 sans profil, les 121 aux atomiques (attendu) et 23 autres ; aucune absente, aucun fichier sans .gcda. **L'hypothèse d'une perte de profil cachée derrière les vieux écarts inexpliqués est donc écartée** : les appels remis en sub_X (appels_figes.py) ne changent pas le graphe.
- Validation sur PC (sans le Ryzen, injoignable : fenêtre sur le bureau) : mode 2, 300 s en donjon scripté : unpackColour 1,4 M, packColour 700 000, _Grow 1,1 M, compare 700 000, assign(chaîne) 600 000, _Copy 500 000, wcslen 400 000, assign(ptr) 200 000, wstring() et createKeyFrame 100 000, filtre PNG et ADPCM au moins 1 000 (derniers paliers), 0 différence ; zip : 2241 fichiers lus, taille et CRC32 justes. La première session avait trouvé un vrai bug (adresse de la table des formats : 0x82065EB0 au lieu de 0x83065EB0, lis -31994 mal converti). Puis natif pur, 300 s : aucun plantage, 2241 fichiers justes ; captures d'écran non produites (cause non cherchée), rendu non vu sur PC. Build console : dist/torchlight-joueurs-1.0.5-20261104-1831.nro (612a81f5), ELF switch/elf/torchlight-joueurs-1.0.5-20261104-1831.elf, envoyée avec dist/torchlight-essai-validation.toml.
- Validation sur la console (dist/torchlight-joueurs-1.0.5-20261104-1831.nro, 612a81f5, switch-logs-archive/20261104-validation-donnees/, fréquences de base, ~4 min, console en français) : derniers paliers compare 4,1 M, _Grow 3,7 M, _Copy 2,2 M, unpackColour 2,1 M, wcslen 1,8 M, assign(chaîne) 1,5 M, assign(ptr) 1,4 M, packColour et wstring() 1,1 M, createKeyFrame 200 000, filtre PNG et ADPCM au moins 1 000 : 0 différence. Zip : 4647 fichiers lus, 4647 justes (une première lecture en voyait 20 faux : ils viennent de translations/fr/pak_fr.zip, polices et RANDOMNAMES.DAT.adm ; verif_zip.py prend maintenant toutes les archives). Aucun défaut graphique vu par l'utilisateur, aucun plantage. Mesure à suivre avec dist/torchlight-essai-reference.toml, à comparer à switch-logs-archive/20261104-chien/.
- Mesure sur la console (dist/torchlight-joueurs-1.0.5-20261104-1831.nro, switch-logs-archive/20261104-mesure-donnees/, fréquences de base, relojes vérifiés) : **ville immobile 60,0 images/s, écart 16,66 ms, image 12,35 ms (243 dessins), 0 image > 33 ms ; mine 60,0 images/s, image 9,71 ms (185 dessins)** ; contre le recompilé du profil 1.0.4 (image 16,53 et 13,51 ms) : -25 % et -28 %. Première rencontre : 286 -> 217 ms (-24 %, une seule rencontre par session, conforme à l'estimation). Combat à dessins égaux : non concluant (-15 % à 250 dessins sur 36 fenêtres, +10 % à 300, -9 % à 350 : la scène compte autant que le nombre de dessins, et trop peu de fenêtres dans la session d'avant). Fin du chargement d'une zone : ~13 images d'environ 200 ms à 2300-2450 dessins (18:47:56, 18:49:31), sans gain, puis un dernier blocage de ~1 s, celui-ci en mode boost ; le jeu est fluide ensuite (18:49:36 : 53 images/s, puis 60). Pas une gêne en jeu pour l'utilisateur, qui les situe à la fin du chargement, vers l'animation de sortie du portail : pas un chantier. Ces images ne viennent pas du GPU bridé : sur 24 images à plus de 2000 dessins, 19 sont déjà hors du mode boost (GPU à sa fréquence normale), image médiane 196 ms, attente du rendu 0 ; les 5 en boost font 140 ms (processeur à 1785 MHz). Plus longue période de boost de la session : 6,3 s. Idée de l'utilisateur (boost limité à 10 s, 10 s de pause ensuite) : ne changerait rien à ces images, et priverait la fin des chargements longs du boost ; abandonnée par l'utilisateur (2026-11-04), non appliquée.
- Baisse avec le jeu en pause (18:51:47-18:52:27, 510 dessins, menu ouvert ; aussi 18:51:16-18:51:33, 418 dessins) : la logique ne coûte plus rien (reste 2,1 ms), le rendu seul fait l'image. Le thread principal seul : 23,5 ms à 418 dessins, 28,3 ms à 510 (ses images sans attente). À 510, le GPU est de plus en retard : attente 8-9 ms, écart 39 ms (25 images/s), et le pilote refuse chaque envoi (« native submission pressure », une par image, 1069 en 44 s, avec une seule soumission en cours de ~2000 entrées et ~120 Ko, la capacité du GPFIFO paraît être de l'ordre de 2048 entrées : à vérifier). Toutes les ~5 s, une série de 2-3 images de 110-150 ms ; seulement pendant ces avertissements. Or un avertissement vide le journal sur la carte SD depuis le thread de rendu (flush_level = warn) : lien probable, pas prouvé. Correctif (vulkan_instance.cpp du SDK, patch à jour) : une ligne sur 1000 pour ce message ; les erreurs restent toutes. À vérifier : les séries de 5 s doivent disparaître. Le GPU à 460 MHz n'aiderait que dans ces scènes (mesure du 2026-10-28 : sans effet quand le thread principal limite) : au mieux 28,3 ms au lieu de 39, le thread principal reprend alors la main.
- GPU à 460,8 MHz en portable (demande de l'utilisateur, 2026-11-04, pour les scènes chargées) : --torchlight_gpu_mhz (0, 384, 460 ; 460 par défaut), profil officiel demandé par RexSwitchApmAplicar du SDK (venu de nfsmw-nx, jamais appelé ici jusque-là) : mémoire inchangée, sinon GPU laissé tel quel ; rien en dock. Demandé depuis le thread du boost (src/torchlight_boost.cpp), avant le premier boost (sinon le SDK lirait 76,8 MHz et refuserait le profil) et seulement en mode portable (cas d'un jeu lancé en dock puis sorti du dock). SDK : la surveillance de la mémoire ignore les périodes de boost (patch à jour). README et torchlight.toml à jour. **Référence changée : ne plus comparer aux sessions à 307,2 MHz** (ligne « relojes »). Build : dist/torchlight-joueurs-1.0.5-20261104-1927.nro (ed53992b), ELF switch/elf/torchlight-joueurs-1.0.5-20261104-1927.elf, torchlight_boost.cpp seul sans profil (attendu, code peu fréquent). À vérifier sur la console : « [apm] PUESTA » dans rex_stderr.log, retour à 460 après un boost et après la veille, séries de 5 s à 510 dessins, batterie et chauffe. Build d'avant, avec le correctif du journal mais sans le 460, non testée sur la console (choix de l'utilisateur) : dist/torchlight-joueurs-1.0.5-20261104-1910.nro (2659a438), ELF switch/elf/torchlight-joueurs-1.0.5-20261104-1910.elf. Piège : une boucle d'attente avec `pgrep -f <motif>` se détecte elle-même, sa ligne de commande contenant le motif, et ne se termine jamais.
- Premier essai de la build ed53992b sur la console XJE1, sans torchlight.toml (switch-logs-archive/20261104-gpu460/, 2 min 15, sans plantage) : « [apm] PUESTA la configuracion 0x92220008 » : GPU 460,8 MHz (était 307,2), mémoire restée à 1600 MHz alors que ce profil annonce 1331 : sur cette console, quelque chose d'autre tient la mémoire à 1600 (kip Horizon-OC ou sys-clk, non vérifié). Toutes les lignes « relojes » hors boost à 460,8 MHz, y compris après 13 périodes de boost ; surveillance de la mémoire jamais déclenchée. Veille non confirmée par le journal. Une seule ligne de pression de Mesa (message écrit une fois sur 1000 : moins de 1001 occurrences, pas une preuve), en plein chargement. Pas de chiffres par fenêtre sans le .toml de mesure : effet du 460 sur les scènes chargées et séries de 5 s à 510 dessins non mesurés. SoC 34,4 -> 37,8 °C en 2 min, trop court pour juger de la chauffe. Vu en passant : le mode boost affiche CPU 2397 MHz sur cette console (aussi dans la session de 18:52), pas les 1785 MHz du système : les temps de chargement mesurés ici ne valent pas pour une console d'origine.
- Relevé des piles à 460,8 MHz (build ed53992b + dist/torchlight-essai-reference.toml, switch-logs-archive/20261104-piles-gpu460/, pilas-archive/rex_pilas-20261104-2003.txt, 19:56-20:04, relojes vérifiés ; relevé actif, donc temps absolus plus hauts, ~10 % à vue, non mesuré). L'utilisateur voit un mieux, mais des baisses avec un cœur à 100 %. **Le thread principal limite : attente du rendu 0,01-0,03 ms presque partout, ~1 ms au pire (20:01:00-10, 390 dessins, écart 29 ms).** Écart : ~190 dessins 16,7 ms (plafond), 262 : 17,6 ms, ~300 en jeu : 24-25 ms, 375 en jeu : 28 ms ; menu ouvert (logique arrêtée, reste 2,3 ms) : 397 dessins, image 24,2 ms, écart 26,5 ms. Le prix suit le nombre de dessins (image ~50-60 µs par dessin). Charge : thread principal 100 %, rendu 37-48 % d'un cœur, deux aides ~4-5 % chacune, le reste quelques % : total ~165-175 % sur 300 %.
  Répartition du thread principal, tranches de temps estimées d'après le rang de l'échantillon (scripts du bloc-notes, non versionnés), combat / menu / calme : logique 27,7 / 12,9 / 21,7 ; recherche des visibles (sub_821A6070) 19,2 / 19,8 / 19,4 ; renderSingleObject 21,3 / 18,4 / 19,0 ; interface (sub_821FBC60) 7,9 / 24,0 / 6,3 ; contrôleurs 7,1 / 10,9 / 8,1 ; part des aides faite par le thread principal 12,4 / 10,4 / 8,4 (particules et billboards + poses) ; relevé des dessins natif 6,0 / 5,3 / 5,7 ; nœuds 3,2 / 2,3 / 2,5. Profil plat : aucune fonction > 3,9 % de temps propre.
  Pistes, gains estimés, non mesurés : (1) interface : le parcours des fenêtres hors dessin pèse 5,9 % en combat, 18,4 % au menu (DessinerFenetre -> sub_821AFF88, appelé à chaque image) : vérifier si les quads sont refaits sans changement (CEGUI d'origine ne les refait que sur demande de redessin ; à vérifier chez Runic, hypothèse), avec validation à l'octet. (2) Aides : le thread principal fait 12,4 % quand chaque aide fait ~4-5 % ; à parts égales ~7 %, plafond -5 %, sans doute la moitié. Verrou du tri des particules sub_821A1CA8 : 2,2 % du thread principal au menu, 0,3 % en combat, attente dans le noyau. (3) Recouvrir au lieu d'attendre : plus risqué, état partagé. (4) Natives restantes : -1 à -2 % au total. (5) Profil PGO et ordre des fonctions refaits en dernier. (6) Mesure sans code : 307,2 contre 460,8 MHz au même endroit, sans relevé, pour voir si le trafic mémoire du GPU freine le thread principal (contention -14 à -18 % jamais expliquée). Ensemble : -10 à -15 % en combat, pas 60 images/s en combat.
  Pics : archers squelettes, 7 fois en 8 min (+77, +129, +181, +213, +244, +300, +340 s), 53-61 ms, mêmes 24 ouvertures (MODELS\SKELETON, MODELS\WEAPONS\SPIKE_BOW) à chaque fois, 44-60 % des piles sous sub_8219C328 -> sub_823F5F60 -> sub_8219C380, ouvertures 1-2 ms seulement : modèle relu à chaque apparition, sans doute (hypothèse). Les trois ouvertures en échec par fichier (music, programs, RTShaderLib) ne coûtent que ~50 µs chacune. Autres : shader de première rencontre 32 ms, 142 allocations physiques 21 ms.
  **Correction (même soir), analyse refaite :** les « aides » étaient comptées deux fois (poses incluses dans Groupe::Lancer) : thread principal 7,8 % en combat, pas 12,4 % ; sur la session, 4,8 % de tâches contre 3,15 % pour chaque aide : plafond 1 à 2 % en combat, pas 5 %. **Archers : aucun fichier de modèle relu** : les 24 « ouvertures » sont 4 tests de dossier, une série par arme et par corps, pour 3 archers créés dans la même image (~15 ms chacun d'après les horodatages), ~50 µs l'ouverture ; le relevé ne commence qu'à 40 ms de l'image, donc rien encore sur cette phase. Le trafic mémoire du GPU à 460 MHz : hypothèse retirée (l'utilisateur a raison : même travail par image, 60 % sur son overlay).
- **Interface reconstruite à chaque image pour rien (2026-11-04 soir, demande de l'utilisateur).** sub_821FBC60 est System::renderGUI : reconstruction seulement si le drapeau +40 est levé. Traçage PC (gdb, point d'observation matériel, session lancée sans gdb puis attachée : sous gdb dès le départ, la manette de script se décale) : ville, 1502 images sur 1503, toujours sub_821A40E0 -> sub_821A41A0 -> sub_821A49C8 -> sub_821A4B78 -> sub_821A4938 (Window::onMoved). Ce sont 14 fenêtres, 7 paires <page>_lefttriggerbutton / <page>_righttriggerbutton (10, 12, 16, 18, 19, 21, 22) qui oscillent en permanence ; visibles par leur drapeau, mais leur page n'est pas sous la feuille active : aucun quadrilatère en cache, besoin de redessin jamais baissé, donc jamais atteintes. Correctif, fin de src/torchlight_ogre_natif.cpp, --torchlight_interface 0/1/2 : onMoved natif, drapeau levé seulement si la fenêtre est dessinée (visible jusqu'à la feuille active +48) ; enfants et événement gardés. CEGUI garde les quadrilatères par fenêtre et ne les refait que sur requestRedraw : un changement de contenu lève déjà le drapeau ; mais des widgets de Runic auraient pu s'en dispenser sans que personne le voie. Mode 2 : liste refaite à chaque image, comparée à l'octet (76 octets par quadrilatère, map +204 du rendu ; octets 37-39 masqués : remplissage après le booléen +36, jamais initialisé ni lu, ce sont eux qui ont donné les 255 « différences » de la première session).
  PC (ville, inventaire, onglet du familier, options, menu pause, aide, manette de script) : **7200 images, 1903 avec demande, 0 différence**, soit ~74 % d'images sans reconstruction dans cette session (part en combat réel à mesurer). Mode 1 : aucune violation, pages identiques à l'œil sur les captures ; écarts visibles limités à la 3D animée, à de petites marques et aux boutons LT/RT, à l'écran sur ces pages. Reste à valider sur la console, en vrai combat.
- Aides (src/torchlight_particules.cpp) : quelques µs d'essais avant le noyau sur les verrous du groupe et des sections en série, attente courte de la dernière aide, tri des tâches sur des indices sans allocation (même ordre). Natives (13, fin de src/torchlight_ogre_natif.cpp) : sub_821E4588 (propriété L"ATTACHES_TO_MASTER" : clé posée sur la pile, plus d'allocation à chaque appel), sub_82284BA0 (0,001 puissance x, 1 valeur au lieu de 4 ; hors plage à l'original), getters de AutoParamDataSource sub_821C1608, sub_821C1098, sub_821C16A0, sub_821C46D0, sub_821BE428, sub_82194138, sub_821C1E50, sub_821C30B0, sub_821A5150 (tri par insertion), sub_821A7B30. audit_retours_natifs : seul f13 relu par sub_821AC8F8 après sub_82284BA0, ni l'original ni la native n'y touchent. PC, mode 2 (240 s) : 6,5 M appels, 0 différence, sub_821E4588 et sub_82284BA0 couvertes ; toutes couvertes en mode 1. **Sans nouveau profil PGO, ces natives ne donneront rien de mesurable (leçon du 2026-11-03) : session PGO à refaire en dernier.** Chien de garde : --torchlight_images_longues_ms (seuil de lecture des piles ; seules les images >= 40 ms sont écrites), pour voir enfin le début des images des archers.
  Build console : dist/torchlight-joueurs-1.0.5-20261104-2126.nro (f98db5f6), ELF switch/elf/torchlight-joueurs-1.0.5-20261104-2126.elf ; essais : dist/torchlight-essai-interface.toml (validation), dist/torchlight-essai-mesure-archers.toml (mesure et piles). Profil : les « source locations ... have changed » (338 dans torchlight_ogre_natif.cpp : lignes décalées) gardent le profil ; perdus (« number of counters », hors initialisations) : torchlight_ogre_natif.cpp : ReportControllers, sub_821C1768, sub_821E30A0 ; torchlight_particules.cpp : Groupe::Lancer, Executer, Rapport, RapportBB ; torchlight_coeurs.cpp : Lire, FinDeFenetre ; à reprendre par la prochaine session PGO. Remarque de l'utilisateur : les archers sont invoqués à trois en même temps, avec un effet de particules et une animation de sortie du sol ; sur les 5 images à 24 ouvertures, les dessins ne font pas de bond (240-311) : ~+30 ms dans la logique et ~+10 ms au rendu à dessins égaux, en une image ; à confirmer par les piles.
- Validation de l'interface sur la console (mode 1 + comparaison, dist/torchlight-joueurs-1.0.5-20261104-2126.nro, f98db5f6, switch-logs-archive/20261104-validation-interface/, 21:33-21:37, fréquences 1020 / 460,8 / 1600 MHz, sans plantage) : 10 800 images, 0 différence, **mais 9194 avec une demande (85 % ; 68, puis 91, puis 96 % par tranche de 3600)** : les 74 % évités sur PC n'existaient qu'en ville et dans les menus. D'autres fenêtres demandent en vrai jeu ; 107 fonctions lèvent le drapeau (recherche statique : lwz rX,26572 puis stb rY,40(rX)), impossible à filtrer une par une. Précision de l'utilisateur : les archers sont une invocation aléatoire de son familier, trois d'un coup.
- **Règle générale, --torchlight_interface=3** (fin de src/torchlight_ogre_natif.cpp, défaut encore 1) : au moment de reconstruire, parcours des fenêtres dessinées (dans l'ordre de DessinerFenetre) : reconstruction seulement si l'une a son besoin de redessin levé (+424), si l'une a bougé, ou si leur liste a changé ; sinon drapeau baissé. Mode 2 : valide cette règle (liste refaite à chaque image, comparée à l'octet quand elle n'aurait rien refait). Avec --torchlight_journal_stats, le bilan nomme les fenêtres responsables. PC (ville, inventaire, familier, options, pause, aide) : mode 2, 7200 images, 2600 demandes, 876 utiles, 0 différence ; responsables : 10_lefttriggerbutton, 10_righttriggerbutton, 4_rightbumper, 5_leftbumper, à l'écran sur ces pages, 2_LargeMessageText ; mode 3, 7200 images, 1793 demandes, 711 reconstructions (90 % d'images sans), sans plantage, pages identiques sur les captures hors scène 3D animée et petits éléments d'interface animés. Reste : la même validation en vrai combat, puis la mesure, avant d'en faire le défaut. Passage en dock non testé.
- Bilan transmissible de la journée : switch/DECOUVERTES_2026-11-04.md (demande de l'utilisateur, pour une session sur un autre projet).
- **Session unique sur la console (dist/torchlight-joueurs-1.0.5-20261104-2159.nro, 9616191d + dist/torchlight-essai-session.toml, switch-logs-archive/20261104-session-combat-interface/, 22:29-22:35, 1020 / 460,8 / 1600 MHz, sans plantage).** Mode 2, vrai combat : **14 400 images, 12 076 demandes, 10 486 utiles, 0 différence** : la règle est juste, mode 3 passé par défaut. Par tranche de 3600 images, reconstructions utiles : 1108, 2892, 2886, 3600 : peu de gain en combat. Responsables : 2_PlayerManaBar (presque à chaque image, deux fois quand elle bouge aussi), 2_PlayerManaBarSub, 2_PlayerHealthBar, chiffres de dégâts (StaticText 192-206, « Coup critique ! ») ; un texte de dialogue et les boutons animés hors combat. Ils changent vraiment. Piste non commencée : rejouer les quadrilatères des fenêtres inchangées pendant la reconstruction (parcours : texte surtout, 2,3 % sur 4,6 % en combat, 10,4 % sur 14,2 % au menu ; estimé 2 à 3 % en combat). Temps à dessins par tranches, avec piles et validation : 150-250 dessins, écart 18,0 ms ; 250-320, 21,9 ; 320-400, 24,8 ; attente du rendu 0,01-0,04 ms.
  **Archers (invocation aléatoire du familier), seuil de piles à 5 ms, 5 images à 24 ouvertures, 119 piles :** de 0 à ~25 ms création des trois archers, ~9 ms chacun (sub_823DE1A8 -> sub_822883F0 -> sub_8228B4B8 et sub_8228B218, profil plat, sub_821A1510 en tête) ; ~25-35 ms logique et début normaux ; ~37-49 ms, **sub_8219C328 (écouteurs de la scène +17112, sub_8219C380) -> sub_823F5F60, ~12 ms, 25 % des piles, 0,1 % d'une image normale, rôle non identifié** ; puis rendu normal. Pas de correctif simple ; étaler les créations sur plusieurs images toucherait à la logique du jeu. Laissé tel quel en attendant le choix de l'utilisateur.
- Fin de journée (2026-11-04) : code figé à 15b0d05 à la demande de l'utilisateur. Build instrumentée dist/torchlight-pgo-1.0.5-20261104-2256.nro (8bde8799, ELF switch/elf/torchlight-pgo-1.0.5-20261104-2256.elf ; -DTORCHLIGHT_PGO=generer -DTORCHLIGHT_LTO=OFF -DTORCHLIGHT_OUTILS=OFF -DTORCHLIGHT_PILE_NON_VOLATILE=ON -DTORCHLIGHT_NATIVES_FIXES=ON -DTORCHLIGHT_CODE_BLOCS=ON -DTORCHLIGHT_MESURE_TLB=OFF, dans switch/out-pgo, ~20 min) envoyée sur la console, sans .toml, pgo/ de la console vidé. Profil précédent gardé dans pgo-natives-seules/. Session pas encore récupérée. Suite : CLAUDE.md, « REPRENDRE ICI ».
- Session PGO du 2026-11-05 (13:12-13:46, switch-logs-archive/20261105-session-pgo/, 10 vidages, sans plantage, ~30 s finales hors profil ; blocs du noyau 2821 au plus, contre 2350 le 2026-11-04, aucune erreur). Ordre des fonctions refait (2962 fonctions contre 1756). Build finale dist/torchlight-joueurs-1.0.5-20261105-1902.nro (d8867f5f), ELF switch/elf/torchlight-joueurs-1.0.5-20261105-1902.elf ; audit_profil.py : 218 fichiers, 0 fonction sans profil (144 sur la build 612a81f5).
- Mesure de cette build (dist/torchlight-essai-reference.toml, switch-logs-archive/20261105-mesure-finale/, 19:31-19:36, relojes 1020 / 460,8 / 1600 MHz vérifiés, sans plantage). Ville 242 dessins : image 12,78 ms (p10 12,73, p90 13,03), 60 images/s. Mine : 9,74 ms à 176-186 dessins (7 fenêtres, juste après l'arrivée, dessins encore changeants), 8,69 ms à 160-165 (pas de référence). Combat, 1 min 50 seulement, 161-320 dessins : 150-250 écart 19,02 ms (219 dessins), 250-320 écart 22,07 ms (275 dessins). **Aucun gain visible contre les références** : ville 12,28 ms à 307,2 MHz (+4,1 %, dispersions étroites, donc réel), mine 9,68 ms à 185 dessins, combat 18,81 et 21,62 ms (ces derniers avec validation et piles, donc pessimistes). Cinq choses ont changé depuis la référence de la ville : GPU à 460,8 MHz (aucune mesure de la ville à cette fréquence avant celle-ci), interface mode 3, 13 natives et verrous des aides, profil, ordre des fonctions : cause non établie. GPU et interface se départagent sans recompiler (torchlight_gpu_mhz = 0, torchlight_interface = 0, même .nro). Contre le recompilé du profil 1.0.4 (16,53 et 13,51 ms) : environ un quart de moins.
- Décision de l'utilisateur (2026-11-05) : essai GPU / interface écarté, le gain contre la 1.0.4 suffit ; 1.0.5 préparée sans recompiler. Avant le commit : zlib 1.3.1 (nouvelle dans le .nro) et poses de torchlight-rekindled (journal du 2026-10-27) ajoutées au README et aux notices ; zip refait à la main avec le .nro testé (dist/torchlight-nx-1.0.5.zip, 3aac0e63) ; nfsmw-sdk.patch identique au SDK compilé (mêmes lignes modifiées) ; Mesa inchangé depuis le 2026-10-11. Session finale sans .toml pas jouée.
- Étude « autres gains à bas niveau » (demande de l'utilisateur, 2026-11-05, rien de codé). Relevé pilas-archive/rex_pilas-20261104-2003.txt (ELF joueurs 20261104-1927, 460,8 MHz, relevé actif, avant interface mode 3, 13 natives et nouveau profil : proportions approximatives). **Piège : les lignes « T: » du relevé sont les échantillons pris pendant une image longue (switch_perf.cpp du SDK)** ; les compter avec les autres, ou seules, fausse tout. Comptes par plages de l'ELF (llvm-nm -S), pas par le symboliseur avec --inlines, qui s'est désaligné sur certaines adresses (accès TLS annoncé à 0,40 %, en réalité 0,05 %).
  - Images normales, thread principal (68 406 échantillons), temps propre : code généré 62,2 %, natives et relevé du port 27,5 %, libnx 4,6 % (sommeil du jeu 2,25 % au plafond, verrous 1,09 %, conditions 0,81 %), newlib 3,8 % (memcpy 3,35 % : constantes sub_821C26D0 0,90, tampon de l'interface par SetVertexBufferBinding 0,70, sub_821C5FA0 0,63, billboards 0,26), SDK 1,1 % (KeTlsGetValue 0,28, BaseHeap::AllocRange 0,22). Verrou le plus attendu : tri des particules (sub_821A1CA8, src/torchlight_particules.cpp) 0,50 %. **Aucun autre gros poste sous le code du jeu** : le temps est dans le code généré et les natives, qui attendent la mémoire.
  - Fermés : tas de l'hôte déjà en grandes pages (+7,5 ns), accès TLS (-mtp=soft) 0,05 %, défauts de page ~0/s, options du compilateur déjà -O3 -mtune=cortex-a57, mode 4 cœurs inutile (autres threads ~45 % d'un cœur en combat, CPU total 132 % sur 300). Thread principal à 97-100 % dans les vrais relevés de combat (mesure du soir, relevés 23-32) ; les moyennes plus basses viennent des chargements.
  - Images longues, thread principal (3882 échantillons) : svcSendSyncRequest 13,2 %. Dont **4,6 % pour la vérification du cache de shaders : ShaderSource (src/torchlight_natif.cpp:843) -> InCache -> pack::Valid (src/natif/shader_pack.h:68) ouvre le .tlsh, lit l'en-tête, va à la fin et le ferme sur la carte SD, sur le thread principal, même quand le shader est en cache** ; correctif simple : liste des noms valides en mémoire. Aussi : 5,4 % de lectures du jeu hors du cache de blocs (XFile::Read -> pread : lecture en avance), 1,1 % d'écritures et vidages de spdlog, 1,7 % dans BaseHeap::AllocRange du SDK (src/system/xmemory.cpp, recherche linéaire dans la table des pages à chaque MmAllocatePhysicalMemoryEx, appelé par sub_8287F090).
  - Inconnue la plus grosse : contention du cache L2 de 2 Mo et de la RAM avec le thread de rendu (24-30 % d'un cœur) et le GPU. Plafond de 14-18 % mesuré avant la géométrie persistante, jamais remesuré ; ville +4 % au passage à 460,8 MHz, cohérent avec plus de trafic du GPU, non prouvé. Mesurable sans recompiler sur le .nro de la 1.0.5 : torchlight_alterner = "vulkan" (images jetées par le thread de rendu, écran figé une fenêtre sur deux ; « rendu » est l'alternance des poses du rendu, pas celle-ci), avec le GPU à 460,8 puis à 307,2 MHz : dist/torchlight-essai-plafond-460.toml et dist/torchlight-essai-plafond-307.toml, mêmes réglages que dist/torchlight-essai-sans-vulkan.toml du 2026-11-04. **Abandonné par l'utilisateur (2026-11-05) : ne pas reproposer cette mesure.**
  - Vues 0xA0000000, 0xC0000000, 0x7F000000 (~27 Mo) restées en petites pages depuis la correction de la vue 0xE0000000 : gain borné (l'échange avait donné 0 à -2 %), chantier lourd ; pas prioritaire.
- Vérification du cache de shaders sortie du thread principal (2026-11-05, src/torchlight_natif.cpp, pas encore commité, pour la version suivante) : ShaderSource met chaque shader en file sans appeler InCache ; le thread de compilation vérifie le cache et ne compile (ni n'annonce) que ce qui manque. Sans thread de compilation, comportement d'avant. Validé sur le Ryzen (Weston sans écran, ~/essais/essai3.sh, build PC avec le SDK et l'arbre synchronisés, manette du donjon, 260 s, strace des openat, threads nommés par /proc/<pid>/task/*/comm) : avant (code de la 1.0.5), le thread principal ouvre 116 .tlsh, le thread de compilation 96, le rendu 201 ; après, thread principal 0, compilation 212, rendu 201 ; 48 shaders compilés dans les deux cas, 0 échec ; sans cache, 116 compilés, 0 échec, aucun plantage. Gain sur la console non mesuré (sur la carte SD, chaque vérification fait ouverture, lecture, déplacement et fermeture).
- **Trois correctifs des à-coups de l'arrivée intégrés à la 1.0.5 (demande de l'utilisateur, 2026-11-05, release pas encore faite)**, dans le SDK (switch/patches/nfsmw-sdk.patch régénéré, 30 fichiers) :
  - Lecture en avance asynchrone du cache par blocs (host_path_file.cpp, nfsmw_io_cache_anticipa, 1 dans torchlight_app.h et switch/torchlight.toml, 0 par défaut dans le SDK). Trace PC d'abord (nfsmw_io_cache_traza, diagnostic, coupé) : 523 blocs manqués, 44 % = bloc suivant du précédent, la plupart ~1 ms après ; simulé : 1 bloc d'avance évite 60 % des manques (60 % des blocs lus utiles), 2 blocs 65 % (52 %). Un thread (rex::thread, priorité 0x2C sur la console, sa propre poignée de fichier) lit le bloc suivant après chaque manque et à la première utilisation d'un bloc lu en avance ; un lecteur qui demande un bloc en cours de lecture attend cette lecture, un bloc seulement en file est repris et lu par le lecteur. Variables de condition et thread jamais détruits (sortie qui lance les destructeurs statiques).
  - Recherche de haut en bas de BaseHeap::AllocRange (xmemory.cpp/.h) : 213 des 214 échantillons d'AllocRange (MmAllocatePhysicalMemoryEx, tas physique de 131 072 entrées de 16 octets) dans la boucle qui descend une page prise à la fois. Compteur de pages prises par groupe de 64 (AllocFixed, AllocRange, Release, Restore) ; un groupe plein est sauté d'un coup, mêmes candidats écartés, même résultat. heap_busqueda_rapida (vrai), heap_busqueda_verif (diagnostic : les deux recherches et les compteurs comparés).
  - Journal (logging.cpp) : vidages par un thread à part (log_flush_hilo, vrai : un avertissement ou une erreur réveille le thread, flush_on coupé ; sinon toutes les log_flush_interval) et tampon du fichier de 64 Ko (log_file_buffer_kb, after_open de spdlog ; 1 Ko par défaut dans newlib). Sur le thread principal, 22 des 45 échantillons d'écriture du journal venaient de ResolvePath(SAVE:\) (erreur bénigne, à chaque sauvegarde, donc à l'arrivée dans une zone), 23 du tampon plein.
  - Validation sur le Ryzen (300 s, manette du donjon, réglages de la console passés en ligne de commande) : AllocRange 8448 recherches, 0 différente, 0 groupe mal compté ; cache 222 lectures de la SD au lieu de 526 (traceIO, même session sans lecture en avance), 413 blocs lus en avance dont 300 utilisés, 25 attentes, 10 repris, 39,5 Mo lus contre 32,7 ; --torchlight_zip_verif : 5058 fichiers lus, 5058 justes (switch/tools/verif_zip.py) ; aucun plantage. Gains sur la console non mesurés. Piège : une commande « build | grep | head ; cp » a copié un binaire d'avant (compilation pas allée au bout, sortie tronquée par head) et la première session de validation a tourné sans le nouveau code : vérifier le code de sortie et une chaîne du nouveau code (strings) avant d'essayer.
- Build de la 1.0.5 refaite avec ces correctifs : dist/torchlight-joueurs-1.0.5-20261105-2149.nro (894c80be), ELF switch/elf/torchlight-joueurs-1.0.5-20261105-2149.elf, zip dist/torchlight-nx-1.0.5.zip ; audit_profil.py : 0 fonction du code généré sans profil ; profils perdus (« number of counters ») : OnPostInitLogging, CompileThreadMain, ShaderSource seulement ; ancien candidat gardé (dist/torchlight-nx-1.0.5-candidat1.zip).
- Mesure sur la console (dist/torchlight-essai-reference.toml, switch-logs-archive/20261105-mesure-candidat2/, 22:01-22:06, relojes 1020 / 460,8 / 1600 MHz, sans plantage), contre le candidat précédent (19:31, même .toml, même enchaînement menu -> ville -> mine). Lectures sur la SD au même nombre d'accès (58 000) : 221 au lieu de 728 (-70 %) ; 554 blocs lus en avance, 445 utilisés, 130 attentes d'une lecture en cours. Images longues (lignes [image longue], détail par poste) : démarrage -> ville 35,8 -> 29,8 s (-17 %), dont lectures 3598 -> 2121 ms, allocations physiques 724 -> 97 ms, ouvertures 852 -> 779 ms ; ville -> mine 11,4 -> 10,4 s (-9 %), dont allocations physiques 319 -> 36 ms, lectures 71 -> 16 ms, ouvertures 340 -> 277 ms ; combat inchangé (5-6 images longues). Une session de chaque côté : ordres de grandeur, pas de marge. Régime stable sans recul : mine 160-165 dessins image 8,59 ms (8,69 avant) ; combat 150-250 dessins écart 18,78 (19,02), 250-320 20,61 (22,07 ; combats différents). Ville non comparable (256 dessins contre 245 : pas au même endroit). Journal complet jusqu'à la dernière fenêtre avant la sortie (vidage par le thread). Reste le plus gros poste des arrivées parmi les entrées-sorties : les ouvertures de fichiers (~280-780 ms par phase).

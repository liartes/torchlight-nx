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

# torchlight - version Nintendo Switch (.nro)
#
# Code généré par le rexglue de nfsmw-nx dans switch/generated (voir gen_manifest.sh),
# sources de l'app partagées avec le PC (../src).
#
# Configuration (dans le conteneur devkitPro, voir build_switch.sh) :
#   cmake -S switch -B switch/out -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_TOOLCHAIN_FILE=<nfsmw-nx>/tools/switch/cmake/switch-devkitA64.cmake \
#     -DREXSDK_DIR=<nfsmw-nx>/sdk \
#     -DREXGLUE_SWITCH_NVK_SDK=<mesa-switch>/nvk-sdk/opt/devkitpro/portlibs/switch

cmake_minimum_required(VERSION 3.25)
project(torchlight LANGUAGES CXX)

# Nom public du projet : torchlight-nx (portage non officiel ; menu homebrew, zip des joueurs). Version affichée par
# le menu homebrew et nom du zip (switch/build_joueurs.sh).
set(TORCHLIGHT_VERSION "1.0.3")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Rendu natif : les shaders HLSL du jeu sont compilés en SPIR-V sur la machine elle-même (console comprise), à
# partir des sources que le jeu génère ; aucun shader compilé n'est distribué. glslang du SDK avec HLSL et
# SPIRV-Tools (légalisation).
set(REXGLUE_GLSLANG_HLSL ON CACHE BOOL "" FORCE)

# Chemins de la machine de build retirés des __FILE__ (journaux et assertions du SDK et de l'app, ~140 chaînes dans
# le .nro distribué) : relatifs au dossier qui contient les dépôts. -fmacro-prefix-map seulement : -ffile-prefix-map
# changerait aussi les chemins des profils PGO (-fprofile-prefix-map). Avant le SDK (include ci-dessous).
get_filename_component(TORCHLIGHT_ESPACE "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
add_compile_options("-fmacro-prefix-map=${TORCHLIGHT_ESPACE}/=")

include(generated/rexglue.cmake)

add_executable(torchlight ../src/main.cpp ../src/torchlight_trace.cpp ../src/torchlight_manette.cpp
    ../src/torchlight_capture.cpp ../src/torchlight_natif.cpp ../src/torchlight_crt.cpp ../src/torchlight_ogre_natif.cpp ../src/torchlight_blocs.cpp ../src/torchlight_pgo.cpp ../src/natif/rendu.cpp
    ../src/natif/hlsl_prep.cpp ../src/natif/hlsl_compiler.cpp ../src/natif/d3dx_cache.cpp ../src/torchlight_installation.cpp ../src/torchlight_menu_fond.cpp ../src/torchlight_appels_indirects.cpp ../src/torchlight_particules.cpp ../src/torchlight_boost.cpp
    ${REXSDK_DIR}/thirdparty/glslang/StandAlone/ResourceLimits.cpp)
target_include_directories(torchlight PRIVATE ../src)
# Rendu natif : empreintes des textures avec XXH3 (rex/hash.h), en-tête du SDK non exporté par ses cibles.
if(REXSDK_DIR)
    target_include_directories(torchlight PRIVATE ${REXSDK_DIR}/thirdparty/xxHash)
endif()
# Compilation des shaders HLSL dans le jeu : glslang (entrée HLSL) et ses passes SPIRV-Tools.
target_include_directories(torchlight PRIVATE ${REXSDK_DIR}/thirdparty/glslang
    ${REXSDK_DIR}/thirdparty/glslang/StandAlone)
target_link_libraries(torchlight PRIVATE glslang SPIRV HLSL SPIRV-Tools-opt)
# Mêmes définitions que la compilation de glslang (ENABLE_HLSL change l'interface publique de TShader).
set_source_files_properties(../src/natif/hlsl_compiler.cpp
    ${REXSDK_DIR}/thirdparty/glslang/StandAlone/ResourceLimits.cpp
    PROPERTIES COMPILE_DEFINITIONS "ENABLE_HLSL;ENABLE_OPT=1")
rexglue_setup_target(torchlight GPU_PLUGINS xenos)
# Rendu natif (et son traceur) : absent du build PC d'origine (CMakeLists.txt racine).
target_compile_definitions(torchlight PRIVATE TORCHLIGHT_NATIF=1)
# Outils de développement (manette de script, capture d'écran de l'émulation) : OFF pour la build des joueurs
# (switch/build_joueurs.sh). Ne change que le code de l'application : le profil PGO du code généré reste valable.
option(TORCHLIGHT_OUTILS "Outils de développement (manette de script, capture d'écran)" ON)
if(TORCHLIGHT_OUTILS)
    target_compile_definitions(torchlight PRIVATE TORCHLIGHT_OUTILS=1)
endif()

# Build PC (switch/out-pc, pour développer le rendu natif) avec le SDK en sous-dossier :
# rex_app.cpp inclut imgui.h, que rexruntime lie en privé ; sur Switch,
# rexglue_switch.cmake s'en charge. Même correctif que nfsmw-nx/app/CMakeLists.txt.
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET imgui)
    target_include_directories(torchlight PRIVATE
        $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET renderdoc)
    target_include_directories(torchlight PRIVATE
        $<TARGET_PROPERTY:renderdoc,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

# Accès à la pile du thread sans volatile (switch/tools/pile_non_volatile.py, appliqué au code généré) : le
# compilateur peut garder en registres les valeurs sauvegardées sur la pile. OFF : macros volatile d'origine.
option(TORCHLIGHT_PILE_NON_VOLATILE "Accès à la pile du code généré sans volatile" ON)
if(TORCHLIGHT_PILE_NON_VOLATILE AND TARGET torchlight_recomp)
    target_compile_definitions(torchlight_recomp PRIVATE REX_STACK_NON_VOLATILE=1)
endif()

if(REXGLUE_PLATFORM_SWITCH)
    # -fno-math-errno sur le code du jeu (comme sa2-nx) : sans lui, chaque racine carrée (fsqrt) est suivie d'un test
    # et d'un appel de secours à sqrt() pour renseigner errno, que le code recompilé ne lit jamais (résultats
    # identiques). Mesuré (2026-10-14) : -0,3 % d'instructions dans un fichier à 12 racines carrées, 2 instructions
    # de moins par racine carrée exécutée. L'en-tête précompilé prend les options de la cible : l'option doit être
    # celle de la cible entière (sinon il garde -fmath-errno et l'impose). Aussi sur notre code (interceptions natives
    # appelées par le jeu) : avec le LTO, GCC n'intègre pas l'une dans l'autre deux fonctions qui diffèrent sur
    # -fmath-errno (piège relevé par sa2-nx). Notre code ne lit pas errno après une fonction mathématique.
    # Fonctions natives d'Ogre (validées au bit près sur PC, sans fma) : GCC contracte par défaut a * b + c en fma
    # (-ffp-contract=fast en mode GNU), ce qui changerait les arrondis sur la console ; les fma du jeu (fmadds)
    # y sont écrites explicitement (std::fma).
    set_source_files_properties(../src/torchlight_ogre_natif.cpp PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
    option(TORCHLIGHT_SANS_MATH_ERRNO "-fno-math-errno sur le code du jeu et le nôtre" ON)
    if(TORCHLIGHT_SANS_MATH_ERRNO)
        foreach(cible torchlight_recomp torchlight)
            if(TARGET ${cible})
                target_compile_options(${cible} PRIVATE -fno-math-errno)
            endif()
        endforeach()
    endif()
    # LTO sur le code du jeu et le nôtre (pas sur les bibliothèques du SDK), comme nfsmw-nx : avec les appels directs
    # (switch/tools/appels_directs.py), le compilateur peut intégrer des fonctions du jeu d'un fichier à l'autre.
    # Premier essai (un seul processus) : pic à 7,2 Go sur 15, phase LTRANS de ~300 Mo par partition ; 4 processus.
    option(TORCHLIGHT_LTO "LTO sur le code du jeu et de l'application" OFF)
    if(TORCHLIGHT_LTO)
        foreach(cible torchlight_recomp torchlight)
            if(TARGET ${cible})
                target_compile_options(${cible} PRIVATE -flto -fno-fat-lto-objects)
            endif()
        endforeach()
        target_link_options(torchlight PRIVATE -flto=4 -flto-partition=balanced)
    endif()
    # PGO (optimisation guidée par un profil) en deux temps, comme nfsmw-nx (docs/toolchain.md) :
    #   generer : build instrumentée (sans LTO) ; le jeu vide le profil dans sdmc:/switch/torchlight/pgo toutes les
    #             3 minutes (src/torchlight_pgo.cpp, fopen redirigé par --wrap) ;
    #   utiliser : recompilation avec les .gcda recopiés dans pgo/ (racine du dépôt, ignoré par git).
    # Même dossier de compilation, même code généré et mêmes sources entre les deux. -fno-profile-values : le profil
    # des appels indirects lit tpidr_el0, toujours 0 sur Horizon (-mtp=soft), la build plantait au démarrage.
    get_filename_component(TORCHLIGHT_PGO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pgo" ABSOLUTE)
    set(TORCHLIGHT_PGO "" CACHE STRING "PGO : vide, generer ou utiliser")
    if(TORCHLIGHT_PGO)
        file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}" TORCHLIGHT_PGO_PREFIXE)
        if(TORCHLIGHT_PGO STREQUAL "generer")
            set(TORCHLIGHT_PGO_OPTIONS -fprofile-generate=${TORCHLIGHT_PGO_DIR}
                "-fprofile-prefix-path=${TORCHLIGHT_PGO_PREFIXE}" -fprofile-update=single -fno-profile-values)
            target_link_options(torchlight PRIVATE -fprofile-generate -Wl,--wrap=fopen)
            target_compile_definitions(torchlight PRIVATE TORCHLIGHT_PGO_GENERER=1
                "TORCHLIGHT_PGO_DIR=\"${TORCHLIGHT_PGO_DIR}\"")
        elseif(TORCHLIGHT_PGO STREQUAL "utiliser")
            # -fprofile-correction : compteurs sans synchronisation, négatifs dans les fichiers partagés entre
            # threads ; sans cette option, GCC refuse le profil.
            set(TORCHLIGHT_PGO_OPTIONS -fprofile-use=${TORCHLIGHT_PGO_DIR} "-fprofile-prefix-path=${TORCHLIGHT_PGO_PREFIXE}"
                -fprofile-partial-training -fno-profile-values -fprofile-correction -Wno-missing-profile
                -Wno-error=coverage-mismatch)
            target_link_options(torchlight PRIVATE -fprofile-use -fprofile-partial-training -fprofile-correction)
        else()
            message(FATAL_ERROR "TORCHLIGHT_PGO doit être vide, generer ou utiliser (et non '${TORCHLIGHT_PGO}')")
        endif()
        foreach(cible torchlight_recomp torchlight)
            if(TARGET ${cible})
                target_compile_options(${cible} PRIVATE ${TORCHLIGHT_PGO_OPTIONS})
            endif()
        endforeach()
    endif()
    # Limites de l'intégration (inlining) pour le code du jeu, avec le LTO et un profil (comme sa2-nx 1.0.1,
    # docs/cpu-rounds.md « Link-time optimisation of a recompiled program ») : aucune fonction recompilée n'est
    # déclarée inline, donc la limite de GCC pour les autres (max-inline-insns-auto = 30) refuse la plupart des
    # appels chauds. Avec le profil, l'intégrateur ne fait grossir que les appels exécutés ; inline-unit-growth
    # plafonne la croissance du code (40 % chez GCC). Le profil reste valable : ses compteurs sont posés avant
    # l'intégration interprocédurale. Options de compilation (GCC garde ses paramètres par fonction et l'intégrateur
    # prend ceux de l'appelant). Vide : valeur du compilateur.
    # Essai du 2026-10-13 (64 / 12, switch/RENDU_NATIF.md) : code +5 %, aucun gain mesuré sur la console (ville plus
    # lente de 8-10 % à nombre de dessins égal) : abandonné, vide par défaut.
    set(TORCHLIGHT_INLINE_AUTO "" CACHE STRING
        "max-inline-insns-auto du code du jeu avec LTO et profil (vide : valeur du compilateur)")
    set(TORCHLIGHT_INLINE_UNIT_GROWTH "" CACHE STRING
        "inline-unit-growth du code du jeu avec LTO et profil (vide : valeur du compilateur)")
    if(TORCHLIGHT_LTO AND TORCHLIGHT_PGO STREQUAL "utiliser" AND TARGET torchlight_recomp)
        if(TORCHLIGHT_INLINE_AUTO)
            target_compile_options(torchlight_recomp PRIVATE --param=max-inline-insns-auto=${TORCHLIGHT_INLINE_AUTO})
        endif()
        if(TORCHLIGHT_INLINE_UNIT_GROWTH)
            target_compile_options(torchlight_recomp PRIVATE
                --param=inline-unit-growth=${TORCHLIGHT_INLINE_UNIT_GROWTH})
        endif()
    endif()
    # Fonctions chaudes regroupées au début de .text (switch/tools/ordre_fonctions.py, d'après les profils des
    # piles de la console ; méthode de nfsmw-nx). Le fichier garde KEEP (*(.crt0)) en tête.
    option(TORCHLIGHT_ORDRE_FONCTIONS "Fonctions chaudes au début de .text" ON)
    if(TORCHLIGHT_ORDRE_FONCTIONS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ordre_fonctions.ld")
        target_link_options(torchlight PRIVATE
            "-Wl,--section-ordering-file,${CMAKE_CURRENT_SOURCE_DIR}/ordre_fonctions.ld")
        set_property(TARGET torchlight APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ordre_fonctions.ld")
    endif()
    # Sur la carte SD : sdmc:/switch/torchlight/torchlight.nro, avec le contenu
    # de assets/ (default.xex...) directement dans le même dossier (src/torchlight_app.h).
    # Icône : switch/out/icone.jpg, fabriquée par build_switch.sh depuis nro-icon.png (racine du dépôt) ; sinon
    # l'icône par défaut de libnx.
    set(TL_ICONE)
    if(EXISTS "${CMAKE_BINARY_DIR}/icone.jpg")
        set(TL_ICONE ICON "${CMAKE_BINARY_DIR}/icone.jpg")
    endif()
    rexglue_switch_add_nro(torchlight
        NAME "torchlight-nx"
        AUTHOR "Liartes"
        VERSION "${TORCHLIGHT_VERSION}"
        ${TL_ICONE})
endif()

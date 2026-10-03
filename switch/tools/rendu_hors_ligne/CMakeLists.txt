# Rendu hors ligne d'une capture (voir switch/RENDU_NATIF.md) : même code de rendu que le jeu (src/natif).
#   cmake -S switch/tools/rendu_hors_ligne -B switch/out-pc/rendu_hors_ligne -G Ninja \
#         -DCMAKE_CXX_COMPILER=clang++-20 -DCMAKE_BUILD_TYPE=Release
#   cmake --build switch/out-pc/rendu_hors_ligne
cmake_minimum_required(VERSION 3.25)
project(rendu_hors_ligne LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Vulkan REQUIRED)
find_package(ZLIB REQUIRED)

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../src)
add_executable(rendu_hors_ligne rendu_hors_ligne.cpp ${SRC}/natif/rendu.cpp)
target_include_directories(rendu_hors_ligne PRIVATE ${SRC})
target_link_libraries(rendu_hors_ligne PRIVATE Vulkan::Vulkan ZLIB::ZLIB)

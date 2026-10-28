cmake_minimum_required(VERSION 3.16)
project(dx7fm CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

# --- demo/testes (motor header-only, sem dependencias externas) -----------
add_executable(demo src/demo.cpp)
target_compile_options(demo PRIVATE -Wall -Wextra)
target_include_directories(demo PRIVATE src) # demo.cpp inclui app/event_queue.h (sem SDL/ALSA)
target_link_libraries(demo PRIVATE Threads::Threads)

# --- patches de demonstracao: gerador + testes de comportamento ------------
foreach(tool gen_presets patch_tests golden)
    if(tool STREQUAL golden)
        add_executable(${tool} tools/golden/${tool}.cpp)
    else()
        add_executable(${tool} tools/presets/${tool}.cpp)
    endif()
    target_compile_options(${tool} PRIVATE -Wall -Wextra)
    target_include_directories(${tool} PRIVATE src app)
    target_link_libraries(${tool} PRIVATE Threads::Threads)
endforeach()

# --- ctest: testes do motor (nos dois caminhos SIMD) + testes dos patches ---
enable_testing()
add_test(NAME motor COMMAND demo WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME motor_sse2 COMMAND demo WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(motor_sse2 PROPERTIES ENVIRONMENT DX7FM_NO_AVX2=1)
add_test(NAME patches_de_fabrica COMMAND patch_tests ${CMAKE_BINARY_DIR}/patch_tests_wav)

# --- app standalone (SDL2 + ALSA + Dear ImGui) -----------------------------
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(SDL2 sdl2)
    pkg_check_modules(ALSA alsa)
endif()

if(SDL2_FOUND AND ALSA_FOUND)
    set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)

    add_executable(dx7fm_standalone
        app/main.cpp
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
        ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp
    )
    target_include_directories(dx7fm_standalone PRIVATE
        src
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
        ${SDL2_INCLUDE_DIRS}
        ${ALSA_INCLUDE_DIRS}
    )
    target_compile_options(dx7fm_standalone PRIVATE -Wall -Wextra ${SDL2_CFLAGS_OTHER})
    # fonte da UI (Roboto Medium, distribuida com o Dear ImGui)
    target_compile_definitions(dx7fm_standalone PRIVATE DX7FM_FONT_DIR="${IMGUI_DIR}/misc/fonts")
    target_link_directories(dx7fm_standalone PRIVATE ${SDL2_LIBRARY_DIRS} ${ALSA_LIBRARY_DIRS})
    target_link_libraries(dx7fm_standalone PRIVATE ${SDL2_LIBRARIES} ${ALSA_LIBRARIES} Threads::Threads)
else()
    message(WARNING "SDL2 e/ou ALSA nao encontrados via pkg-config -- pulando o target dx7fm_standalone (o target 'demo' ainda compila normalmente).")
endif()

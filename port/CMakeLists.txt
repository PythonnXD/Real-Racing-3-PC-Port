cmake_minimum_required(VERSION 3.20)
project(rr3_pc LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/EHsc /bigobj /MP /utf-8 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS)
endif()

# ---- Boost (header-only for Dynarmic) ----
set(BOOST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost" CACHE PATH "" FORCE)
set(Boost_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost" CACHE PATH "" FORCE)
set(Boost_NO_SYSTEM_PATHS ON CACHE BOOL "" FORCE)

# ---- Dynarmic (A64 only, bundled externals) ----
set(DYNARMIC_FRONTENDS "A64" CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS ON CACHE BOOL "" FORCE)
set(DYNARMIC_FATAL_ERRORS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/dynarmic EXCLUDE_FROM_ALL)

# ---- zlib ----
set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/zlib EXCLUDE_FROM_ALL)

# ---- SDL2 (prebuilt VC x64) ----
set(SDL2_DIR_LOCAL "${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL2")
add_library(SDL2_local SHARED IMPORTED)
set_target_properties(SDL2_local PROPERTIES
    IMPORTED_IMPLIB "${SDL2_DIR_LOCAL}/lib/x64/SDL2.lib"
    IMPORTED_LOCATION "${SDL2_DIR_LOCAL}/lib/x64/SDL2.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${SDL2_DIR_LOCAL}/include"
)

# ---- ANGLE (prebuilt x64) ----
set(ANGLE_DIR_LOCAL "${CMAKE_CURRENT_SOURCE_DIR}/third_party/angle/angle-x64")
add_library(ANGLE_EGL SHARED IMPORTED)
set_target_properties(ANGLE_EGL PROPERTIES
    IMPORTED_IMPLIB "${ANGLE_DIR_LOCAL}/lib/libEGL.dll.lib"
    IMPORTED_LOCATION "${ANGLE_DIR_LOCAL}/bin/libEGL.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${ANGLE_DIR_LOCAL}/include"
)
add_library(ANGLE_GLESv2 SHARED IMPORTED)
set_target_properties(ANGLE_GLESv2 PROPERTIES
    IMPORTED_IMPLIB "${ANGLE_DIR_LOCAL}/lib/libGLESv2.dll.lib"
    IMPORTED_LOCATION "${ANGLE_DIR_LOCAL}/bin/libGLESv2.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${ANGLE_DIR_LOCAL}/include"
)

# ---- Helper to copy runtime DLLs into build dir ----
set(RR3_RUNTIME_DLLS
    "${SDL2_DIR_LOCAL}/lib/x64/SDL2.dll"
    "${ANGLE_DIR_LOCAL}/bin/libEGL.dll"
    "${ANGLE_DIR_LOCAL}/bin/libGLESv2.dll"
    "${ANGLE_DIR_LOCAL}/bin/d3dcompiler_47.dll"
)
foreach(dll ${RR3_RUNTIME_DLLS})
    get_filename_component(dll_name "${dll}" NAME)
    configure_file("${dll}" "${CMAKE_BINARY_DIR}/${dll_name}" COPYONLY)
endforeach()

# ---- Core config + distribution tools library ----
add_library(rr3_config_lib STATIC
    src/config.cpp
    src/dist_tools.cpp
)
target_include_directories(rr3_config_lib PUBLIC
    src
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zlib"
    "${CMAKE_BINARY_DIR}/third_party/zlib"
)
target_link_libraries(rr3_config_lib PUBLIC
    zlibstatic
    bcrypt
)

# ---- Core CPU + ELF Loader library ----
add_library(rr3_core STATIC
    src/cpu_dynarmic.cpp
    src/elf_loader.cpp
)
target_include_directories(rr3_core PUBLIC src)
target_link_libraries(rr3_core PUBLIC
    dynarmic
    bcrypt
)

# ---- Bionic libc + Layered VFS library ----
add_library(rr3_bionic STATIC
    src/vfs.cpp
    src/bionic_libc.cpp
)
target_include_directories(rr3_bionic PUBLIC src)
target_link_libraries(rr3_bionic PUBLIC
    rr3_core
    rr3_config_lib
)

# ---- ANGLE GLES2 Bridge + Fake JNI library ----
add_library(rr3_gles_jni STATIC
    src/gles_bridge.cpp
    src/jni_env.cpp
    third_party/single_header/rg_etc1.cpp
)
target_include_directories(rr3_gles_jni PUBLIC
    src
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/single_header"
)
target_link_libraries(rr3_gles_jni PUBLIC
    rr3_core
    rr3_config_lib
    ANGLE_EGL
    ANGLE_GLESv2
)

# ---- Task 1 Smoke Test ----
add_executable(test_smoke tests/test_smoke.cpp)
target_link_libraries(test_smoke PRIVATE
    rr3_config_lib
    dynarmic
    SDL2_local
    zlibstatic
)
target_include_directories(test_smoke PRIVATE
    src
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zlib"
    "${CMAKE_BINARY_DIR}/third_party/zlib"
)
target_compile_definitions(test_smoke PRIVATE SDL_MAIN_HANDLED)

# ---- Task 2 ELF + CPU Test ----
add_executable(test_elf_cpu tests/test_elf_cpu.cpp)
target_link_libraries(test_elf_cpu PRIVATE
    rr3_core
)

# ---- Task 3 Bionic + VFS + RR3 .init_array Test ----
add_executable(test_bionic_vfs tests/test_bionic_vfs.cpp)
target_link_libraries(test_bionic_vfs PRIVATE
    rr3_bionic
    rr3_gles_jni
)

# ---- Task 4 ANGLE GLES2 + Fake JNI Test ----
add_executable(test_gles_jni tests/test_gles_jni.cpp)
target_link_libraries(test_gles_jni PRIVATE
    rr3_gles_jni
    SDL2_local
)
target_compile_definitions(test_gles_jni PRIVATE SDL_MAIN_HANDLED)

# ---- Main Native PC Port Executable (rr3_pc.exe, includes built-in Launcher & Configurator UI) ----
add_executable(rr3_pc WIN32
    src/main.cpp
    src/configurator.cpp
    src/java_firemint.cpp
    src/java_offline_stubs.cpp
    src/audio_opensl.cpp
    src/input_pc.cpp
    assets/rr3_icon.rc
    assets/rr3_app.manifest
)
target_include_directories(rr3_pc PRIVATE
    src
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/single_header"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zlib"
    "${CMAKE_BINARY_DIR}/third_party/zlib"
)
target_link_libraries(rr3_pc PRIVATE
    rr3_bionic
    rr3_gles_jni
    SDL2_local
    zlibstatic
    dwrite
    comctl32
    comdlg32
    dwmapi
    gdiplus
    ole32
    shell32
    uxtheme
)
target_compile_definitions(rr3_pc PRIVATE SDL_MAIN_HANDLED)
if(MSVC)
    target_link_options(rr3_pc PRIVATE /ENTRY:mainCRTStartup)
endif()

# ---- Task 7 Input (Virtual Gamepads + Keyboard Ramp), Resize & Dist Tools Test ----
add_executable(test_input_resize
    tests/test_input_resize.cpp
    src/input_pc.cpp
)
target_include_directories(test_input_resize PRIVATE src)
target_link_libraries(test_input_resize PRIVATE
    rr3_bionic
    rr3_gles_jni
    SDL2_local
    dwrite
)
target_compile_definitions(test_input_resize PRIVATE SDL_MAIN_HANDLED)

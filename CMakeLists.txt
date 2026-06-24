cmake_minimum_required(VERSION 3.16)
project(AppDemoOpenFHE LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default build type for single-config generators
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ── OpenFHE ────────────────────────────────────────────────────────────
# Prefer user-supplied prefix; default to submodule install location
if(NOT OPENFHE_PREFIX)
    set(OPENFHE_PREFIX "${CMAKE_SOURCE_DIR}/openfhe")
endif()

# Try find_package first (uses installed OpenFHEConfig.cmake)
find_package(OpenFHE QUIET HINTS "${OPENFHE_PREFIX}/lib/OpenFHE")

# ── Sources ────────────────────────────────────────────────────────────
add_executable(AppDemo
    src/AppDemo.cpp
    src/test1.cpp
    src/test2.cpp
    src/mlwe.cpp
    src/mlwe_demo.cpp
)

target_include_directories(AppDemo PRIVATE ${CMAKE_SOURCE_DIR}/include)

# ── OpenFHE linking ────────────────────────────────────────────────────
if(OpenFHE_FOUND)
    # find_package succeeded
    target_link_libraries(AppDemo PRIVATE OPENFHEpke OPENFHEcore OPENFHEbinfhe)

    if(EXISTS "${OPENFHE_PREFIX}/include/openfhe/pke/openfhe.h")
        set(OPENFHE_INC "${OPENFHE_PREFIX}/include/openfhe")
        target_include_directories(AppDemo SYSTEM PRIVATE
            ${OPENFHE_INC}
            ${OPENFHE_INC}/core
            ${OPENFHE_INC}/pke
            ${OPENFHE_INC}/binfhe)
    elseif(DEFINED OpenFHE_INCLUDE)
        target_include_directories(AppDemo SYSTEM PRIVATE ${OpenFHE_INCLUDE})
    endif()

    message(STATUS "OpenFHE found via find_package (v${BASE_OPENFHE_VERSION})")
else()
    # Fallback: manual search when find_package fails (e.g. Windows where .so targets don't exist)
    message(STATUS "OpenFHE find_package failed, falling back to manual search")

    # Include dirs — prefer installed layout, fall back to source layout
    if(EXISTS "${OPENFHE_PREFIX}/include/openfhe/pke/openfhe.h")
        set(OPENFHE_INC "${OPENFHE_PREFIX}/include/openfhe")
        target_include_directories(AppDemo SYSTEM PRIVATE
            ${OPENFHE_INC}
            ${OPENFHE_INC}/core
            ${OPENFHE_INC}/pke
            ${OPENFHE_INC}/binfhe)
    else()
        set(OPENFHE_SRC "${OPENFHE_PREFIX}/src")
        target_include_directories(AppDemo SYSTEM PRIVATE
            ${OPENFHE_SRC}/pke/include
            ${OPENFHE_SRC}/pke/include/scheme
            ${OPENFHE_SRC}/pke/include/scheme/bfvrns
            ${OPENFHE_SRC}/pke/include/scheme/bgvrns
            ${OPENFHE_SRC}/pke/include/scheme/ckksrns
            ${OPENFHE_SRC}/pke/include/schemebase
            ${OPENFHE_SRC}/pke/include/schemelet
            ${OPENFHE_SRC}/pke/include/schemerns
            ${OPENFHE_SRC}/pke/include/key
            ${OPENFHE_SRC}/pke/include/keyswitch
            ${OPENFHE_SRC}/pke/include/encoding
            ${OPENFHE_SRC}/core/include
            ${OPENFHE_SRC}/core/include/lattice
            ${OPENFHE_SRC}/core/include/lattice/hal
            ${OPENFHE_SRC}/core/include/lattice/hal/default
            ${OPENFHE_SRC}/core/include/math
            ${OPENFHE_SRC}/core/include/math/hal
            ${OPENFHE_SRC}/core/include/math/hal/bigintdyn
            ${OPENFHE_SRC}/core/include/math/hal/bigintfxd
            ${OPENFHE_SRC}/core/include/math/hal/bigintntl
            ${OPENFHE_SRC}/core/include/math/hal/intnat
            ${OPENFHE_SRC}/core/include/utils
            ${OPENFHE_SRC}/core/include/utils/blockAllocator
            ${OPENFHE_SRC}/core/include/utils/prng
            ${OPENFHE_SRC}/binfhe/include)
    endif()

    find_library(OPENFHE_CORE  NAMES OPENFHEcore  PATHS "${OPENFHE_PREFIX}/lib")
    find_library(OPENFHE_PKE   NAMES OPENFHEpke   PATHS "${OPENFHE_PREFIX}/lib")
    find_library(OPENFHE_BINFHE NAMES OPENFHEbinfhe PATHS "${OPENFHE_PREFIX}/lib")

    if(OPENFHE_CORE AND OPENFHE_PKE AND OPENFHE_BINFHE)
        target_link_libraries(AppDemo PRIVATE ${OPENFHE_PKE} ${OPENFHE_CORE} ${OPENFHE_BINFHE})
    else()
        message(WARNING "Could not find OpenFHE libraries in ${OPENFHE_PREFIX}/lib")
    endif()
endif()

# ── OpenMP ─────────────────────────────────────────────────────────────
find_package(OpenMP QUIET)
if(OpenMP_CXX_FOUND)
    target_link_libraries(AppDemo PRIVATE OpenMP::OpenMP_CXX)
endif()

# ── Compiler warnings ─────────────────────────────────────────────────
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(AppDemo PRIVATE -Wall -Wextra -Wpedantic)
elseif(MSVC)
    target_compile_options(AppDemo PRIVATE /W4 /permissive-)
    target_compile_definitions(AppDemo PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ── RPATH (Linux only) ────────────────────────────────────────────────
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CMAKE_BUILD_RPATH "${OPENFHE_PREFIX}/lib")
    set_target_properties(AppDemo PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
    target_link_options(AppDemo PRIVATE
        "-Wl,--disable-new-dtags"
        "-Wl,-rpath,$ORIGIN"
        "-Wl,-rpath,${OPENFHE_PREFIX}/lib")
endif()

install(TARGETS AppDemo RUNTIME DESTINATION bin)
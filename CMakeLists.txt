# CMakeList.txt: AppDemo 的 CMake 项目，在此处包括源代码并定义
#
cmake_minimum_required (VERSION 3.16)

project (AppDemoOpenFHE LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default prefix
if(NOT OPENFHE_PREFIX)
  if(DEFINED ENV{HOME})
    set(OPENFHE_PREFIX "$ENV{HOME}/openfhe-install")
  else()
    set(OPENFHE_PREFIX "${CMAKE_SOURCE_DIR}/openfhe-install")
  endif()
endif()
message(STATUS "OPENFHE_PREFIX=${OPENFHE_PREFIX}")

# Unified single entry executable
add_executable(AppDemo AppDemo.cpp AppDemoOpenFHE.cpp BFVrnsExperiment.cpp AppDemoOpenFHE.h)

# Include OpenFHE headers (mark SYSTEM to reduce warnings)
target_include_directories(AppDemo SYSTEM PRIVATE 
  ${OPENFHE_PREFIX}/include/openfhe
  ${OPENFHE_PREFIX}/include/openfhe/core
  ${OPENFHE_PREFIX}/include/openfhe/pke
  ${OPENFHE_PREFIX}/include/openfhe/binfhe)

# Attempt to locate libs (do not hard fail)
find_library(OPENFHE_CORE NAMES OPENFHEcore PATHS ${OPENFHE_PREFIX}/lib)
find_library(OPENFHE_PKE NAMES OPENFHEpke PATHS ${OPENFHE_PREFIX}/lib)

if(OPENFHE_CORE AND OPENFHE_PKE)
  target_link_libraries(AppDemo PRIVATE ${OPENFHE_PKE} ${OPENFHE_CORE})
else()
  message(WARNING "Could not find OpenFHE libraries in ${OPENFHE_PREFIX}/lib. Ensure you installed OpenFHE.")
endif()

find_package(OpenMP QUIET)
if(OpenMP_CXX_FOUND)
  target_link_libraries(AppDemo PRIVATE OpenMP::OpenMP_CXX)
endif()

# Basic warnings
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(AppDemo PRIVATE -Wall -Wextra -Wpedantic)
elseif (MSVC)
  target_compile_options(AppDemo PRIVATE /W4 /permissive-)
  target_compile_definitions(AppDemo PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# rpath so binary can run without setting LD_LIBRARY_PATH after install
set(CMAKE_BUILD_RPATH ${OPENFHE_PREFIX}/lib)
set_target_properties(AppDemo PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")

install(TARGETS AppDemo RUNTIME DESTINATION bin)

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
find_library(OPENFHE_BINFHE NAMES OPENFHEbinfhe PATHS ${OPENFHE_PREFIX}/lib)
file(GLOB OPENFHE_ALL_SHARED "${OPENFHE_PREFIX}/lib/libOPENFHE*.so*")

set(_openfhe_found TRUE)
foreach(_lib IN ITEMS OPENFHE_CORE OPENFHE_PKE OPENFHE_BINFHE)
  if(NOT ${_lib})
    set(_openfhe_found FALSE)
  endif()
endforeach()

if(_openfhe_found)
  target_link_libraries(AppDemo PRIVATE ${OPENFHE_PKE} ${OPENFHE_CORE} ${OPENFHE_BINFHE})

  # Copy all OpenFHE shared libs (including versioned .so.1) next to the executable for easy run/debug
  add_custom_command(TARGET AppDemo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:AppDemo>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OPENFHE_ALL_SHARED} "$<TARGET_FILE_DIR:AppDemo>"
    COMMENT "Copying OpenFHE shared libraries (with symlinks) next to AppDemo")
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
# use escaped $ORIGIN to avoid shell/env expansion during link
set(CMAKE_BUILD_RPATH "\$ORIGIN;${OPENFHE_PREFIX}/lib")
set_target_properties(AppDemo PROPERTIES INSTALL_RPATH "\$ORIGIN/../lib;${OPENFHE_PREFIX}/lib")
# Also force rpath on the target to ensure loader picks libs from executable dir (and prefix) when F5/run
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # 【待验证】设置了--disable-new-dtags就使用RPATH不是RUNPATH了，然后之后不需要设置patchelf
  target_link_options(AppDemo PRIVATE "-Wl,--disable-new-dtags" "-Wl,-rpath,\$ORIGIN" "-Wl,-rpath,${OPENFHE_PREFIX}/lib")
  # target_link_options(AppDemo PRIVATE "-Wl,-rpath,\$ORIGIN" "-Wl,-rpath,${OPENFHE_PREFIX}/lib")
endif()

install(TARGETS AppDemo RUNTIME DESTINATION bin)

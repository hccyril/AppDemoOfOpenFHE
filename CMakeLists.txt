# CMakeList.txt: AppDemoOpenFHE 的 CMake 项目，在此处包括源代码并定义
# 项目特定的逻辑。
#
cmake_minimum_required (VERSION 3.16)

project (AppDemoOpenFHE LANGUAGES CXX)

# C++ 版本
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 在 WSL 下，默认使用固定安装路径（如未显式传入 OpenFHE_DIR）
if(NOT OpenFHE_DIR)
  set(OpenFHE_DIR "$ENV{HOME}/openfhe-install/lib/OpenFHE")
endif()

# 查找 OpenFHE 包（用于链接库）
find_package(OpenFHE CONFIG REQUIRED)

# OpenMP（可选，加速并去除忽略 omp pragma 的告警）
find_package(OpenMP)

# 将源代码添加到此项目的可执行文件。
add_executable (AppDemoOpenFHE "AppDemoOpenFHE.cpp" "AppDemoOpenFHE.h")

# 显式添加已安装 OpenFHE 的头文件搜索路径，并标记为 SYSTEM 抑制三方警告
# 需要同时包含模块根目录，使其内部相对包含（如 "utils/..."、"key/..."）可解析
target_include_directories(AppDemoOpenFHE SYSTEM PRIVATE 
  "$ENV{HOME}/openfhe-install/include/openfhe"
  "$ENV{HOME}/openfhe-install/include/openfhe/core"
  "$ENV{HOME}/openfhe-install/include/openfhe/pke"
  "$ENV{HOME}/openfhe-install/include/openfhe/binfhe"
)

# 链接 OpenFHE 库目标
target_link_libraries(AppDemoOpenFHE PRIVATE OPENFHEpke OPENFHEcore)

# 如找到 OpenMP，则链接启用（GCC/Clang 会自动加 -fopenmp）
if(OpenMP_CXX_FOUND)
  target_link_libraries(AppDemoOpenFHE PRIVATE OpenMP::OpenMP_CXX)
endif()

# 编译器警告与定义（仅对本项目源文件生效，三方头因 SYSTEM 不会刷屏）
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(AppDemoOpenFHE PRIVATE -Wall -Wextra -Wpedantic)
elseif (MSVC)
  target_compile_options(AppDemoOpenFHE PRIVATE /W4 /permissive-)
  target_compile_definitions(AppDemoOpenFHE PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# 运行期库搜索路径：构建目录可直接运行
set(CMAKE_BUILD_RPATH "$ENV{HOME}/openfhe-install/lib")

# 可选：设置 rpath 以在二进制文件旁边找到 lib（如果稍后安装两者）
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)
set_target_properties(AppDemoOpenFHE PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")

# 安装规则（可选）
install(TARGETS AppDemoOpenFHE RUNTIME DESTINATION bin)

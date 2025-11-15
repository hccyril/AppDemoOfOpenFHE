# CMakeList.txt: AppDemoOpenFHE 的 CMake 项目，在此处包括源代码并定义
# 项目特定的逻辑。
#
cmake_minimum_required (VERSION 3.8)

# 如果支持，请为 MSVC 编译器启用热重载。
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project (AppDemoOpenFHE LANGUAGES CXX)

# 将源代码添加到此项目的可执行文件。
add_executable (AppDemoOpenFHE "AppDemoOpenFHE.cpp" "AppDemoOpenFHE.h")

# C++20 标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 首选：查找已安装的 OpenFHE 包（Windows 无 WSL 可行）
# 配置时提供 OpenFHE_DIR 或 CMAKE_PREFIX_PATH 指向 OpenFHE 安装目录。
# 例如：-DOpenFHE_DIR="C:/deps/openfhe-install/lib/cmake/OpenFHE"
find_package(OpenFHE CONFIG REQUIRED)

# 链接 OpenFHE 的 PKE 模块（示例所需）
target_link_libraries(AppDemoOpenFHE PRIVATE OpenFHE::pke)

# 编译器警告与定义
if (MSVC)
  target_compile_options(AppDemoOpenFHE PRIVATE /W4 /permissive-)
  target_compile_definitions(AppDemoOpenFHE PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(AppDemoOpenFHE PRIVATE -Wall -Wextra -Wpedantic)
endif()

# TODO: 如有需要，请添加测试并安装目标。

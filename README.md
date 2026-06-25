# AppDemoOpenFHE

使用 OpenFHE 库的示例应用（BFVrns 乘法演示）。本手册涵盖：环境准备、获取源码、构建与运行、打包分发以及常见问题排查。

---
## 1. 环境与前置条件
### 1.1 WSL 安装（建议 Ubuntu 20.04+/22.04）
Windows PowerShell：
```powershell
wsl --install -d Ubuntu
```
更新与基本构建工具：
```bash
sudo apt update && sudo apt upgrade -y
sudo apt install -y build-essential cmake ninja-build git pkg-config
```
可选（性能并行支持）：
```bash
sudo apt install -y libomp-dev
```
### 1.2 目录规划
假设工作目录：`$HOME` 下或挂载盘 `/mnt/d/.../AppDemoOpenFHE`。
OpenFHE 安装前缀统一为：`$HOME/openfhe-install`。

---
## 2. 克隆OpenFHE代码仓库

以下命令克隆最新完整的OpenFHE代码仓库(包括当中的子模块)
```bash
mkdir third-party
cd third-party
git clone https://github.com/openfheorg/openfhe-development
cd openfhe-development
git submodule update --init --recursive
```

---
## 3. 构建并安装 OpenFHE（在 WSL 内）
进入子模块目录：
 - 也就是上一步骤进入到的`openfhe-development`目录下

配置（关闭示例、单元测试与基准，Release 模式）：
```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_EXAMPLES=OFF \
  -DBUILD_UNITTESTS=OFF \
  -DBUILD_BENCHMARKS=OFF \
  -DCMAKE_INSTALL_PREFIX=$HOME/openfhe-install
```
编译与安装：
```bash
cmake --build build -j
cmake --install build
```
验证关键文件：
```bash
ls $HOME/openfhe-install/include/openfhe/pke/openfhe.h
ls $HOME/openfhe-install/include/openfhe/core/utils/debug.h
ls $HOME/openfhe-install/lib/libOPENFHEcore.so
ls $HOME/openfhe-install/lib/libOPENFHEpke.so
```
返回到项目根目录：
```bash
cd ../
```

---
## 4. 构建示例应用
配置：
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DOPENFHE_PREFIX=$HOME/openfhe-install
```
构建：
```bash
cmake --build build -j
```
运行前设置动态库路径（若未使用打包 rpath）：
```bash
export LD_LIBRARY_PATH=$HOME/openfhe-install/lib:$LD_LIBRARY_PATH
./build/AppDemoOpenFHE
```
交互：程序末尾会要求输入一个数字结束。

---
## 5. 打包与安装（可选分发方式）
安装当前应用到自定义前缀：
```bash
cmake --install build --prefix "$PWD/out"
```
拷贝 OpenFHE 共享库：
```bash
mkdir -p out/lib
cp -a $HOME/openfhe-install/lib/libOPENFHE*.so out/lib/
```
运行（由安装时设置的 rpath 支持，无需再设 LD_LIBRARY_PATH）：
```bash
./out/bin/AppDemoOpenFHE
```

## 【调试用】3-5DEBUG. 调试构建并安装 OpenFHE（Debug）
```sh
cd openfhe
rm -rf build-debug
cmake -S . -B build-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_SHARED=ON -DBUILD_STATIC=OFF \
  -DBUILD_EXAMPLES=OFF -DBUILD_UNITTESTS=OFF -DBUILD_BENCHMARKS=OFF \
  -DCMAKE_INSTALL_PREFIX="$HOME/openfhe-install-debug"
cmake --build build-debug -j
cmake --install build-debug
```
构建完之后回到visual studio，按F5即可调试运行，并且能够断点进入OpenFHE内部代码，有关DEBUG遇到的更多问题细节参见[HowToDebug.md](./HowToDebug.md)

---
## 6. 常见问题与排查
### 6.1 头文件找不到
症状：`fatal error: core/utils/debug.h: No such file or directory`。
排查：确认已安装并使用安装版路径；查看：
```bash
ls $HOME/openfhe-install/include/openfhe/core/utils/debug.h
```
确保 `CMakeLists.txt` 中使用 `target_include_directories(... $HOME/openfhe-install/include/openfhe ...)`。

### 6.2 链接失败或库未找到
症状：运行时报 `libOPENFHEcore.so: cannot open shared object file`。
解决：
```bash
export LD_LIBRARY_PATH=$HOME/openfhe-install/lib:$LD_LIBRARY_PATH
```
或使用安装打包方式 + rpath。

### 6.3 大量三方库警告刷屏
原因：OpenFHE 头文件含大量未使用参数、OpenMP pragma。在本项目中将其标记为 `SYSTEM` 目录即可减少警告；若需要启用并行安装 `libomp-dev` 并重新配置（已在 `CMakeLists.txt` 自动检测）。

### 6.4 OpenMP pragma 被忽略
表现：`warning: ignoring '#pragma omp ...'`。
解决：安装 OpenMP 支持库后重新 CMake：
```bash
sudo apt install -y libomp-dev
rm -rf build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DOPENFHE_PREFIX=$HOME/openfhe-install/lib/OpenFHE
cmake --build build -j
```

### 6.5 更新子模块到最新上游
```bash
cd openfhe
git fetch --depth 1 origin main
git checkout main
git pull --rebase
cd ../..
git add openfhe
git commit -m "Update OpenFHE submodule"
```
重新执行第 3、4 步构建。

### 6.6 重新完全清理构建
```bash
rm -rf build out
find openfhe -maxdepth 1 -name build -type d -exec rm -rf {} +
```
然后重复配置与编译。

---
## 7. 已安装 vs 子模块源码的区别与用法
- 已安装版本：通过 `cmake --install` 生成的标准前缀结构（`include/openfhe/...` 与 `lib/libOPENFHE*.so`），演示中全部引用安装路径减小耦合。
- 子模块源码：保留供阅读/调试/定制。若需要直接开发并调试 OpenFHE，可在其 `build` 目录使用 `cmake --build` 并在主项目中改为指向该构建目录的 `include` 与 `lib`。
- 推荐：生产或演示场景使用安装版；研究开发可直接链接源码构建产物。

---
## 8. 目录结构简要（关键）
```
AppDemoOpenFHE/
  CMakeLists.txt          # 项目构建脚本
  AppDemoOpenFHE.cpp      # 示例主程序
  AppDemoOpenFHE.h
  Manual.txt              # 简要笔记（原始操作记录）
  openfhe/    # OpenFHE 子模块源码
  build/                  # （生成）项目构建目录
  out/                    # （可选生成）安装/打包输出
```

---
## 9. 快速命令汇总（TL;DR）
```bash
# 安装依赖（首次）
sudo apt install -y build-essential cmake ninja-build git libomp-dev

# 构建 & 安装 OpenFHE
cd openfhe
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_EXAMPLES=OFF -DBUILD_UNITTESTS=OFF -DBUILD_BENCHMARKS=OFF \
  -DCMAKE_INSTALL_PREFIX=$HOME/openfhe-install
cmake --build build -j
cmake --install build
cd ../../

# 构建应用
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DOPENFHE_PREFIX=$HOME/openfhe-install/lib/OpenFHE
cmake --build build -j

# 运行
export LD_LIBRARY_PATH=$HOME/openfhe-install/lib:$LD_LIBRARY_PATH
./build/AppDemoOpenFHE
```

---
## 10. 许可证
本演示代码保持与 OpenFHE 兼容的开源使用方式（请参阅上游 OpenFHE 仓库的 LICENSE）。

若需进一步功能（密钥切换、Galois 旋转、CKKS 支持等），请参考 OpenFHE 官方文档与示例

---
# Archives (备份only, 可删除)

_旧版本中关于如何创建子模块的说明_

---
## 2. 获取本仓库与添加子模块（只有第一次初始化需要，已经配好子模块的可以忽略）
```bash
# 克隆本项目（若已存在可跳过）
git clone <YOUR_REPO_URL> AppDemoOpenFHE
cd AppDemoOpenFHE

# 添加 OpenFHE 作为浅克隆子模块（只需主分支最近一次提交）
git submodule add --depth 1 https://github.com/openfheorg/openfhe-development openfhe

# 初始化并更新递归子模块
git submodule update --init --recursive

# 记录到版本库
git add .gitmodules openfhe
git commit -m "Add OpenFHE as submodule"
```
可选：禁止误向上游推送（保护上游仓库）：
```bash
cd openfhe
git remote set-url --push origin DISABLED
cd ../..
```

说明：
- 子模块源码仅用于参考或二次开发，不直接被当前 CMake 用于链接（演示采用“已安装”的 OpenFHE）。
- 若希望直接从子模块构建而不安装，可修改本项目 `CMakeLists.txt`，将 include/link 指向子模块构建产物，这里不展开。
# How to Debug AppDemoOpenFHE (WSL + Visual Studio)

下列步骤确保在 WSL 环境下可 F5 调试，并可单步进入 OpenFHE 源码。

_待验证内容：或许不需要patchelf，详见CMakeLists.txt 74-75行_
```sh
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(AppDemo PRIVATE
    "-Wl,--disable-new-dtags"   # 新增这一行
    "-Wl,-rpath,$ORIGIN"
    "-Wl,-rpath,${OPENFHE_PREFIX}/lib")
endif()
```

## 1. 构建并安装 OpenFHE（Debug）
```sh
cd third_party/openfhe
rm -rf build-debug
cmake -S . -B build-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_SHARED=ON -DBUILD_STATIC=OFF \
  -DBUILD_EXAMPLES=OFF -DBUILD_UNITTESTS=OFF -DBUILD_BENCHMARKS=OFF \
  -DCMAKE_INSTALL_PREFIX="$HOME/openfhe-install-debug"
cmake --build build-debug -j
cmake --install build-debug
```

## 2. 补丁 RPATH（避免间接依赖找不到）
使用 `patchelf` 给库与可执行设置 RPATH，使加载器在可执行目录和安装前缀都能找到依赖。

```sh
# 写入 rpath：当前目录、父目录、安装前缀
patchelf --set-rpath '$ORIGIN:$ORIGIN/..:$ORIGIN/../lib:$HOME/openfhe-install-debug/lib' \
  $HOME/openfhe-install-debug/lib/libOPENFHEpke.so.1
patchelf --set-rpath '$ORIGIN:$ORIGIN/..:$ORIGIN/../lib:$HOME/openfhe-install-debug/lib' \
  $HOME/openfhe-install-debug/lib/libOPENFHEbinfhe.so.1
```

## 3. 配置并构建 AppDemo（WSL 预设）
```sh
cmake -S . -B ~/.vs/AppDemoOfOpenFHE/out/build/wsl-ubuntu-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DOPENFHE_PREFIX=$HOME/openfhe-install-debug
cmake --build ~/.vs/AppDemoOfOpenFHE/out/build/wsl-ubuntu-debug --clean-first -j
```

构建后再给可执行写入 RPATH：
```sh
patchelf --set-rpath '$ORIGIN:$ORIGIN/..:$ORIGIN/../lib:$HOME/openfhe-install-debug/lib' \
  ~/.vs/AppDemoOfOpenFHE/out/build/wsl-ubuntu-debug/AppDemo
```

## 4. 运行/调试
- VS 调试：在配置列表选 "AppDemo (WSL Ubuntu Debug)"（`launch.vs.json` 已注入 `LD_LIBRARY_PATH=$HOME/openfhe-install-debug/lib:$LD_LIBRARY_PATH`），直接 F5。
- 命令行运行：
```sh
export LD_LIBRARY_PATH=$HOME/openfhe-install-debug/lib:$LD_LIBRARY_PATH
cd ~/.vs/AppDemoOfOpenFHE/out/build/wsl-ubuntu-debug
./AppDemo
```

## 5. 快速自检
- `ls $HOME/openfhe-install-debug/lib/libOPENFHEbinfhe.so.1` 存在。
- `ldd ~/.vs/AppDemoOfOpenFHE/out/build/wsl-ubuntu-debug/AppDemo` 能解析 `libOPENFHEbinfhe.so.1`（无 "not found"）。

> 若更换安装路径，统一调整 `OPENFHE_PREFIX`、`patchelf` 和 `LD_LIBRARY_PATH` 中的前缀即可。

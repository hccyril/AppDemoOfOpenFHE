_按以下格式规则添加_
```
# 日期 标题 
## 问题
...
## 原因分析
...
## 修改过程
...
```

---

# 2026-06-24 修复 mlwe.h 头文件包含导致的编译失败 (lattice/lat-defaults.h)

## 问题
在服务器上用 OpenFHE v1.4.2（GCC 16.1.1，Ninja，Release）编译时，cmake 配置成功，
但 `cmake --build build -j` 失败：
```
include/mlwe.h:46:10: fatal error: lattice/lat-defaults.h: No such file or directory
   46 | #include "lattice/lat-defaults.h"
      |          ^~~~~~~~~~~~~~~~~~~~~~~~
compilation terminated.
```
受影响的编译单元为 `src/mlwe.cpp` 与 `src/mlwe_demo.cpp`（均 `#include "mlwe.h"`），
二者均失败，导致整体构建中断。`src/test1.cpp` / `src/test2.cpp` 不受影响。

## 原因分析
`include/mlwe.h` 原本使用了「源码树布局」风格的细分头文件包含：
```cpp
#include "openfhecore.h"
#include "lattice/lat-defaults.h"   // ❌ 路径不存在
#include "math/nbtheory.h"
```
- 本项目通过 `find_package(OpenFHE)` 找到的是「已安装布局」的头文件目录，
  CMakeLists.txt 将其加入 SYSTEM include：`.../include/openfhe`、
  `.../include/openfhe/core`、`.../include/openfhe/pke`、`.../include/openfhe/binfhe`。
- 在该布局下，`#include "lattice/lat-defaults.h"` 相对 `core/` 解析时，
  目标文件实际位于 `core/lattice/hal/default/lat-defaults.h`，
  因此 `core/lattice/lat-defaults.h` 不存在 → fatal error。
- 而 `openfhecore.h`、`math/nbtheory.h` 恰好能在安装布局下解析，
  故唯一被预处理器报出的就是这一条。

项目其余代码（`include/common.h`、`src/test1.cpp`，且 test1.cpp 已在同等环境
编译通过）统一使用伞形头 `pke/openfhe.h`，没有这类路径问题。

`pke/openfhe.h` 是 `openfhecore.h` 的完整超集，本模块用到的全部 API
（`NativePoly`、`ILParams2N`、`RingElement::Integer`、`RootOfUnity`、`usint`、
`COEFFICIENT`、`SetValueAtIndex`、`SetFormat`、`GetLength`、`operator[]`、
`GetModulus`、`ConvertToInt` 等）均为稳定公开接口，故改为伞形头后实现文件无需改动。

## 修改过程
1. `include/mlwe.h`：将三条底层 OpenFHE 包含（`openfhecore.h` /
   `lattice/lat-defaults.h` / `math/nbtheory.h`）替换为单一伞形头：
   ```cpp
   #include "pke/openfhe.h"
   ```
   并补充注释，说明为何不直接包含底层子模块路径（安装版头文件布局下路径不匹配），
   以及伞形头屏蔽布局差异、跨版本/跨安装方式可移植的好处。
2. `src/mlwe.cpp` / `src/mlwe_demo.cpp`：无需改动（通过 `mlwe.h` 间接获得所需声明）。
3. 数学逻辑、运行时行为完全不变；仅修正头文件包含路径。

## 验证
在服务器上重新构建：
```bash
cmake --build build -j
```
构建通过后，运行 AppDemo 并选择「3: MLWE Demo」，验证密钥生成 / 加密 / 解密 /
加法同态性的正确性与解密误差（应 < q/2）。

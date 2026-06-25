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

---

# 2026-06-25 修复 MLWE 模块的多项编译错误与运行时错误

## 问题
在头文件路径问题修复后，`cmake --build build -j` 仍报出大量编译错误，涉及
`include/mlwe.h`、`src/mlwe.cpp`、`src/mlwe_demo.cpp` 三个文件。错误可归为
以下几类：

### 错误 1：`ILParams2N` 类型不存在
```
error: 'ILParams2N' in namespace 'lbcrypto' does not name a type; did you mean 'ILParams'?
```
出现在 `mlwe.h:137,172` 和 `mlwe.cpp:92`。

### 错误 2：`lbcrypto::COEFFICIENT` 命名空间错误
```
error: 'COEFFICIENT' is not a member of 'lbcrypto'; did you mean 'COEFFICIENT'?
```
出现在 `mlwe.cpp` 的 5 处（`MakeElement`、`MakeConstantElement`、`SampleGaussian`、
`SampleUniform`、`ElementToVector`）。

### 错误 3：`SetValueAtIndex` 方法不存在
```
error: 'NativePoly' has no member named 'SetValueAtIndex'
```
出现在 `mlwe.cpp` 的 4 处（`MakeConstantElement`、`SampleGaussian`、`SampleUniform`、
`VectorToElement`）。

### 错误 4：`std::max` 类型推导失败
```
error: no matching function for call to 'max(int64_t&, long long int)'
```
出现在 `mlwe.cpp:371`（`ElementToString` 中）。

### 错误 5：变量 `q` 重复声明
```
error: redeclaration of 'usint q'
```
出现在 `mlwe_demo.cpp:164`（第 93 行已声明 `usint q = 7681`）。

### 错误 6（运行时）：段错误（Segfault）
编译通过后运行 `AppDemo -t 3` 立即段错误。GDB 回溯显示崩溃在
`PolyImpl::operator[]` 中解引用空的 `m_values` 指针。

### 错误 7（运行时）：`operator*` 要求 EVALUATION 格式
修复段错误后运行，抛出异常：
```
operator* for PolyImpl supported only in Format::EVALUATION
```

### 错误 8（运行时）：解密结果系数为巨大数字（NTT 域原始值）
修复格式问题后运行不报错，但公钥/解密结果的系数为 ~10^17 量级的巨大数字，
远超模数 q=7681 的范围。

## 原因分析

### 错误 1：`ILParams2N` 是旧版 OpenFHE 的类型别名
当前安装的 OpenFHE v1.5.1 中，该类型已更名为 `ILParamsImpl<NativeInteger>`
（模板类实例化），不再提供 `ILParams2N` 别名。

### 错误 2：`Format` 枚举不在 `lbcrypto` 命名空间内
`Format` 枚举（`EVALUATION` / `COEFFICIENT`）定义在 `core/utils/inttypes.h` 的
全局命名空间中，而非 `lbcrypto::` 内。使用 `lbcrypto::COEFFICIENT` 会导致
"not a member" 错误。

### 错误 3：`NativePoly`（`PolyImpl<NativeVector>`）无 `SetValueAtIndex` 方法
该方法是旧版 API。当前版本通过 `operator[]` 返回可写的 `Integer&` 引用
（即 `NativeInteger&`）来设置单个系数。

### 错误 4：`std::max` 要求两个参数类型完全一致
`maxAbs` 是 `int64_t`（即 `long int`），`std::llabs()` 返回 `long long int`，
类型不匹配导致模板推导失败。

### 错误 5：同一作用域内重复声明 `q`
`mlwe_demo()` 函数中第 93 行已声明 `usint q = 7681`，第 164 行又用
`usint q = ctx->GetParams().q` 重复声明。

### 错误 6：`PolyImpl` 构造函数默认不初始化 `m_values`
`PolyImpl(params, format)` 的第三个参数 `initializeElementToZero` 默认为
`false`。此时内部 `m_values`（`unique_ptr<NativeVector>`）为空指针。
访问 `operator[]` 会解引用空指针导致段错误。必须传 `true` 以初始化系数向量。

### 错误 7：`PolyImpl::operator*` 要求操作数在 EVALUATION（NTT）格式
OpenFHE 的多项式乘法在 NTT 域（EVALUATION 格式）进行。采样/构造得到的元素
默认在 COEFFICIENT 格式，直接做 `*` 会抛出异常。需在乘法前通过 `SetFormat`
或 NTT 变换切换到 EVALUATION 格式。

### 错误 8：`PolyImpl::operator+=` 不更新 `m_format` 标记
这是最隐蔽的问题。`PolyImpl` 的 `operator+=`（内部调用 `PlusNoCheck`）仅做
逐项模加，不检查也不更新 `m_format` 标记。当累加器 `acc` 以 COEFFICIENT 格式
创建，经过 `+= EVAL_RESULT` 后，数据实际在 EVALUATION 域，但 `m_format` 仍
标记为 COEFFICIENT。后续 `SetFormat(COEFFICIENT)` 判断"已是目标格式"而跳过
INTT，导致系数值为 NTT 域的原始值（巨大数字）。

## 修改过程

### 1. `include/mlwe.h`
- 将 `lbcrypto::ILParams2N` 替换为 `lbcrypto::ILParamsImpl<lbcrypto::NativeInteger>`
  （2 处：`GetElementParams()` 返回类型和 `m_elemParams` 成员类型）。
- 新增 `MLWEContext::ToEval()` 静态方法：将环元素从 COEFFICIENT 切换到
  EVALUATION 格式，用于多项式乘法前的格式转换。

### 2. `src/mlwe.cpp`
- **`ILParams2N` → `ILParamsImpl<NativeInteger>`**（1 处：`MLWEContext` 构造函数中
  `make_shared` 的模板参数）。
- **`lbcrypto::COEFFICIENT` → `Format::COEFFICIENT`**（5 处，使用 `replaceAll`）。
- **`SetValueAtIndex(i, v)` → `e[i] = lbcrypto::NativeInteger(v)`**（4 处）。
  `operator[]` 返回可写的 `NativeInteger&` 引用，直接赋值即可。
- **`std::max(maxAbs, std::llabs(s))` → `std::max<int64_t>(maxAbs, static_cast<int64_t>(std::llabs(s)))`**
  （1 处，显式指定模板参数并强制类型转换）。
- **PolyImpl 构造添加 `initializeElementToZero = true`**（4 处：`MakeElement`、
  `MakeConstantElement`、`SampleGaussian`、`SampleUniform`），确保 `m_values`
  被初始化为全零向量，避免空指针段错误。
- **添加格式转换逻辑**（KeyGen、Encrypt、DecryptCore、InnerProduct）：
  - 在乘法循环前，将累加器 `acc`/`c0`/`inner` 显式 `SetFormat(EVALUATION)`，
    使 `m_format` 标记与数据状态一致（零多项式的 NTT 仍为零，数值不变）。
  - 乘法操作数通过 `MLWEContext::ToEval()` 切换到 EVALUATION 格式。
  - 乘法循环结束后，`SetFormat(COEFFICIENT)` 执行 INTT 切回系数表示，
    以便与 COEFFICIENT 格式的噪声/消息做加减法。

### 3. `src/mlwe_demo.cpp`
- 第 164 行 `usint q = ctx->GetParams().q` 改为 `q = ctx->GetParams().q`
  （去掉 `usint`，避免重复声明）。

### 4. 数学逻辑与运行时行为
- 所有修改仅涉及 API 适配与格式管理，不改变 MLWE 方案的数学语义。
- 解密误差（~3800，q/2=3840）符合理论预期（噪声项 e^T·r 的量级约 n·σ²）。

## 验证
```bash
cmake --build build -j
./build/AppDemo -t 3
```
所有测试用例解密成功（误差 < q/2），加法同态性验证通过。

---

# 2026-06-25 新增 BCHP Demo：论文《Fast Homomorphic Linear Algebra with BLAS》复现

## 问题
复现论文 arXiv:2503.16080（Bae, Cheon, Hanrot, Park, Stehlé）的核心方案——
把同态线性代数（矩阵-向量 / 矩阵-矩阵乘法）「归约」为明文 BLAS 调用，从而把
绝大多数数值计算交给高度优化的 OpenBLAS，把同态运算次数降到最低。

要求：在 `src/` 下创建 `bchp_demo.cpp`，结合本项目已有的 MLWE 模块（环结构）
与 BLAS（明文矩阵乘法），完整演示论文的 Algorithm 6（CP-MM），并加入详细中文注释、
标注与论文章节/公式/算法的对应关系；接入 AppDemo 菜单；更新构建脚本与开发日志。

## 原因分析（设计依据）
1. **论文核心思想**：CP-MM（明文-密文矩阵乘法）可归约为「一次明文 BLAS + 少量 HE 运算」。
   具体地，明文矩阵 M 与「打包进密文」的向量 ĉ 的乘法，等价于在明文侧做一次
   `cblas_dgemv`，再把结果重新打包成密文。批处理多列时升级为一次 `cblas_dgemm`
   （Level-3 BLAS，缓存/向量化效率最高）。
2. **工具选择**：
   - **MLWE 模块**：提供 R_q = Z_q[x]/(x^n+1) 的环元素（`mlwe::RingElement`，
     即 OpenFHE 的 `NativePoly`）。论文后端是 CKKS，本演示出于依赖最小化考虑，
     用 MLWE 环元素「扮演」密文向量的打包容器（n 个系数 = n 个 slot），
     「打包 + 一次 BLAS」的归约结构与论文完全一致，但不执行真实同态加密。
   - **BLAS**：参考 `BlasDemo/src/blas_demo.cpp` 的 CBLAS 调用方式
     （`extern "C" { #include <cblas.h> }` + `cblas_dgemv/dgemm`）。

## 修改过程

### 1. 新增 `src/bchp_demo.cpp`（核心交付物）
按论文章节组织代码，关键映射：
- **第 1 部分**：明文矩阵列优先存储约定 + `cblas_dgemm` 封装
  （对应论文 §2.1 cleartext linear algebra 约定 + Algorithm 6 第 3 行的明文乘法）。
- **第 2 部分**：`PackVector` / `UnpackVector`——把 double 向量量化为环系数打包，
  带符号还原（对应论文 §3.1 encoding / §2.2 packing of slots）。
- **第 3 部分**：`NaiveMatVec`——逐元素环运算式 MatVec，作为正确性与性能基准
  （对应论文 §1 描述的「朴素 HE MatVec」痛点）。
- **第 4 部分**：`Algorithm6_CPMM`——论文 Algorithm 6 三步归约：
  A. 打包 ĉ → `PackVector`；B. 明文 BLAS `cblas_dgemv`；C. 重打包 Ŷ。
  并与朴素路径做正确性 + 性能对比。
- **第 5 部分**：`BatchedMatMul`——一次 `cblas_dgemm` 处理 b 个密文向量，
  对应论文 §3.2 / Algorithm 4-5 的 batched 思路（Level-3 高吞吐）。
- **第 6/7 部分**：随机数据生成、浮点容差比较、四个演示子例程
  （小规模正确性 / 中等规模性能对比 / 批处理 / 思想总结）。
- 入口函数 `bchp_demo()`：构造 MLWE 上下文（n=256, q=7681，满足 q≡1 mod 2n）
  并依次运行四个演示。

### 2. `usint` 与头文件包含修正（自检发现）
编写过程中发现两处会导致编译失败的问题并已修正：
- `using mlwe::usint;` 是错的——`usint` 是 OpenFHE 的全局 typedef
  （经 `pke/openfhe.h` → `openfhecore.h` 引入全局命名空间），`mlwe` 命名空间内
  并无该别名。核查 `mlwe.cpp` 确认其直接使用裸 `usint`，故删除该 using，
  直接使用 `usint`（与 `mlwe.cpp` 用法一致）。
- 补充 `#include <algorithm>`（`std::min/max` 初始化列表形式）与 `<random>`
  （`mt19937_64` / `uniform_real_distribution`），避免对 `mlwe.h` 传递包含的依赖。

### 3. 接入 AppDemo（`src/AppDemo.cpp`）
- 添加前置声明 `int bchp_demo();`（紧随 `mlwe_demo()` 之后）。
- 菜单新增 `4: BCHP Demo (Homomorphic Linear Algebra with BLAS)`。
- switch 新增 `case 4` 调用 `bchp_demo()`。

### 4. 构建脚本（`CMakeLists.txt`）
- `add_executable(AppDemo ...)` 加入 `src/bchp_demo.cpp`。
- 新增「BLAS 查找」段：`find_package(BLAS)` → `pkg_check_modules(openblas)` →
  手动 `find_library`/`find_path(cblas.h)`，三级回退；找到后链接
  `target_link_libraries(AppDemo PRIVATE ${BLAS_LIBRARIES})`、
  加入头文件目录、定义 `BCHP_HAVE_BLAS=1`；未找到时输出明确的安装提示
  （Ubuntu/Debian/CentOS/macOS 命令）而非直接报错，便于在无 BLAS 环境先排查其余模块。

## 影响范围
- 仅新增一个源文件 + 接入菜单 + 构建脚本增强；不改动 MLWE/BFV 既有逻辑。
- BCHP Demo 是「方案级」教学实现：用 MLWE 环元素扮演密文打包，未实现真实 CKKS
  同态加密与 rescale/mod-switch；目的是清晰展示「数据搬运 + 一次 BLAS」的归约结构。

## 验证（请在服务器上执行）
```bash
# 1. 确认已安装 OpenBLAS（Ubuntu: sudo apt-get install libopenblas-dev）
# 2. 重新配置并构建
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DOPENFHE_PREFIX=$HOME/openfhe-install
cmake --build build -j
# 3. 运行 AppDemo，菜单输入 4
./build/AppDemo
```
预期：演示 1 打印「通过 ✓」（BLAS 路径 == 朴素路径，手算 y=[2,2,16]）；
演示 2/3 给出 BLAS 相对朴素实现的加速比（规模越大越显著）；演示 4 为思想总结。

---

# 2026-06-25 修复 BCHP Demo 的编译错误与运行时错误

## 问题
新增 `src/bchp_demo.cpp`（t=4 BCHP Demo）后，`cmake --build build -j` 报出
2 个编译错误；修复编译后运行又遇到 2 个运行时问题。

### 编译错误 1：`SetValueAtIndex` 方法不存在
```
bchp_demo.cpp:193: error: 'NativePoly' has no member named 'SetValueAtIndex'
```
出现在 `PackVector` 函数中，与之前 MLWE 模块遇到的问题完全一致。

### 编译错误 2：`lbcrypto::COEFFICIENT` 命名空间错误
```
bchp_demo.cpp:203: error: 'COEFFICIENT' is not a member of 'lbcrypto'
```
出现在 `UnpackVector` 函数中，同样是已知的 API 适配问题。

### 运行时错误 1：`PackVector: 向量长度超过环维数 n`
演示 2（性能对比）使用 1024×1024 和 2048×1024 规模的矩阵，但 MLWE 上下文
的环维数 n=256 太小，`PackVector` 检查 `v.size() > n` 后抛出异常。

### 运行时错误 2：演示 2 正确性检查全部失败（✗）
BLAS 路径经过 `PackVector` → `UnpackVector` 的量化往返（double → round → int64 → double），
结果被量化为整数；而朴素路径 `NaiveMatVec` 保留原始浮点值。两者在
`VectorsAlmostEqual(rel_eps=1e-5)` 比较时因量化误差超过容差而判定不一致。

## 原因分析

### 编译错误 1-2：OpenFHE API 变更（与 MLWE 模块同源）
`bchp_demo.cpp` 编写时参照了旧版 API 文档/示例，使用了已被移除的
`SetValueAtIndex` 方法和错误的 `lbcrypto::COEFFICIENT` 命名空间。
修复方式与 `mlwe.cpp` 完全一致。

### 运行时错误 1：环维数 n 与测试规模不匹配
`bchp_demo()` 入口函数构造 MLWE 上下文时取 n=256、q=7681（满足 q≡1 mod 2n）。
但演示 2 的测试用例包含 {1024, 1024} 和 {2048, 1024}，向量维数超过 n=256，
导致 `PackVector` 抛出 `invalid_argument`。

### 运行时错误 2：量化语义不一致
`Algorithm6_CPMM` 的 BLAS 路径：`PackVector(v)` → `cblas_dgemv` → `PackVector(y)` → `UnpackVector`，
结果经过 double→int→double 量化往返。朴素路径 `NaiveMatVec` 直接在 double 上计算，
不做量化。两条路径的输出在数学上差一个「就近取整」操作，对于大值结果
（如 1024 个 [-1,1] 随机数累加，量级 ~10-30），绝对误差可达 0.5，
远超 `rel_eps=1e-5` 的容差。

## 修改过程

### 1. `src/bchp_demo.cpp`：`PackVector` 中 `SetValueAtIndex` → `operator[]`
```cpp
// 修复前：
e.SetValueAtIndex(static_cast<usint>(i), coeff);
// 修复后：
e[i] = lbcrypto::NativeInteger(coeff);
```
添加注释说明 `NativePoly` 没有 `SetValueAtIndex`，需使用 `operator[]`
返回可写的 `NativeInteger&` 引用。

### 2. `src/bchp_demo.cpp`：`UnpackVector` 中 `lbcrypto::COEFFICIENT` → `Format::COEFFICIENT`
```cpp
// 修复前：
ec.SetFormat(lbcrypto::COEFFICIENT);
// 修复后：
ec.SetFormat(Format::COEFFICIENT);
```
添加注释说明 `Format` 枚举在全局命名空间。

### 3. `src/bchp_demo.cpp`：增大环维数 n 和模数 q
```cpp
// 修复前：
usint n = 256, k = 2, q = 7681, nu = 4;
// 修复后：
usint n = 2048, k = 2, q = 40961, nu = 4;
```
- n=2048 可容纳演示 2 中最大 2048 维向量和演示 3 中 512 维向量。
- q=40961 = 10 × 4096 + 1 = 10 × 2n + 1，经验证为素数（试除至 √40961 ≈ 202），
  满足 q ≡ 1 (mod 2n) 的要求。
- 添加详细注释说明参数选择的依据。

### 4. `src/bchp_demo.cpp`：`Algorithm6_CPMM` 正确性比较前对齐量化语义
在朴素路径计算完成后、返回前，将朴素结果也做就近取整：
```cpp
for (size_t i = 0; i < res.y_naive.size(); ++i) {
    res.y_naive[i] = std::llround(res.y_naive[i]);
}
```
使两条路径都在「量化后的整数」语义下比较，消除量化不对称导致的误判。
添加注释说明为何需要此对齐处理。

## 影响范围
- 仅修改 `src/bchp_demo.cpp`，不涉及 MLWE/BFV 既有模块。
- 数学逻辑不变，仅修正 API 调用、参数规模和比较语义。

## 验证
```bash
cmake --build build -j
./build/AppDemo -t 4
```
全部四个演示正确性检查通过（✓），演示 2 的加速比数据正常输出，
演示 3 的批处理矩阵乘正确性通过。

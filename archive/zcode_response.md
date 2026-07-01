\# 20260630 7:46



所有 6 个文件均已按计划完成修改。我已经完成了所有工作。在此提供我的工作总结。



\---



\## 完成总结



我对照论文 §6.3 检查并修复了 BCHP 演示"14ms 异常运行时间"的问题。经核查 4 个复合根因，并对代码做了对应修改。



\### 问题根因（4 项复合问题）

| # | 现状 | 论文要求 |

|---|------|----------|

| 1 | n=2048, q=40961(≈2¹⁶) | N=2¹⁴, q≈2⁶⁰ |

| 2 | m=3,k=2 → 仅 12 次环乘 | 256/512/1024 → N²≈2.7×10⁸ |

| 3 | 用 NTT 环乘做矩阵乘（违背核心思想） | 归约到 OpenBLAS dgemm |

| 4 | 无 CC-MM 演示 | §6.3 的 CC-MM 4 核 dgemm |



关键发现：`usint`=`uint32\_t` 装不下 2⁶⁰；`NativeInteger` 在 NATIVEINT=64 下支持 q≤2⁶⁰。



\### 修改内容

1\. \*\*`mlwe.h`/`mlwe.cpp`\*\*：`MLWEParams` 加 `q64`/`useAutoPrime`/`qBits` 字段与大模数构造函数；`MLWEContext` 用 `FirstPrime(60,2N)` 自动生成 \~2⁶⁰ NTT-friendly 素数；`SampleUniform`/`MaxCoeffAbsDiff`/`ElementToString` 中的 `q` 改 `uint64\_t` 避免截断。

2\. \*\*`bchp.h`/`bchp.cpp`\*\*：实现论文核心 \*\*`ModularMatMul\_dgemm`\*\*——多段数字分解(base=2¹⁵，L=⌈log\_base(q)⌉)、每段 `cblas\_dgemm`、`unsigned \_\_int128` 模 q 累加，保证 n·base²<2⁵³ 精确；新增 `Algorithm4\_CCMM\_Cores`（4 次 dgemm，对应 §6.3）。

3\. \*\*`bchp\_demo.cpp`\*\*：参数升级到 N=2¹⁴、q≈2⁶⁰；演示 B 改为 BLAS 归约核 vs NTT 朴素三重循环基线对比；演示 C 重写为 \*\*CC-MM 多规模对照(256/512/1024)\*\*，目标 5+ 秒；演示 A/D 保留升级。

4\. \*\*`docs/dev-logs.md`\*\*：追加完整问题分析、修改过程、风险说明与服务器验证命令。

5\. 全程补充了详细中文注释（标注论文章节/公式/算法编号、double 精度边界推导、`\_\_int128` 必要性）。



\### 预期效果

\- 演示 A：解密误差远小于 q/2；

\- 演示 B：BLAS 核 vs 朴素三重循环结果完全一致（最大系数差=0），体现归约精确性；

\- 演示 C：CC-MM 256×256 预计 5–20s（满足"5 秒以上"），512/1024 递增，与论文 §6.3 同量级；

\- 演示 D：C-MT 转置解密误差 < q/2。



\### 风险/约束

\- 单线程教学版，耗时量级与论文一致但绝对值受硬件(BLAS线程数)影响。

\- `unsigned \_\_int128` 依赖 GCC/Clang 扩展（服务器 GCC 16.1.1 支持；OpenFHE 自身也用）。

\- 内存：1024 规模每核展开约 256MB 级别，串行执行可控；若紧张可减小 `DemoC` 的 `dims`。



本机无执行环境，按你的要求我完成代码修改即结束任务，请你在服务器编译运行验证，若遇 `\_\_int128`/`FirstPrime`/`uniform\_int\_distribution<uint64\_t>` 等 API 报错，把输出发我据实修正。

# 20260701 10:30

四个问题全部修复完成。我先在源头核实了 OpenFHE 的实际语义，再据根因逐一处理：

## 修复总结

### (1) 编译告警 `__int128`
直接阅读 OpenFHE 源码确认：`openfhe/src/core/include/math/hal/basicint.h` 在 `NATIVEINT=64 + HAVE_INT128` 下已把 `unsigned __int128` 别名化为 `DoubleNativeInt`，并在其内部大量使用。把 `src/bchp.cpp` 模乘累加、`src/bchp_demo.cpp` 朴素基线累加器里的裸 `unsigned __int128` 全部改为 `DoubleNativeInt`，源码里不再出现裸 `__int128` token（只在说明性注释中提及），`-Wpedantic` 告警消除。

### (2) 演示 A 解密错误 ≈ q/2
读 `nbtheory-impl.h::FirstPrime` 与 `basicint.h` 确认根因：
- `FirstPrime(nBits, m)` 返回的是「**至少 nBits+1 位**」的素数；
- `MAX_MODULUS_SIZE = 60`（NATIVEINT=64）；
- 旧路径 `qBits=60` → 生成 **61-bit** 素数（用户实测 q=1152921504607338497>2^60）→ 超出安全上限 → NTT/Barrett 失败 → 误差塌缩到 q/2。

修复：主上下文与演示 C 的独立上下文都把 `qBits` 设为 **59**。因为 m=2n=32768=2^15，`2^59 mod 2^15 = 0`，`FirstPrime(59,32768)` 起始即 `q=2^59+1`，是 **60-bit**、恰好落在上限内且 `q≡1 mod 2n` ✓。

### (3) 演示 B 挂死 10 小时
根因不在 BLAS，而在 `ModularMatMul_dgemm` 的旧段数判断：
```cpp
Modulus cover = base; while (cover < q) { cover *= base; ++L; }
```
当 61-bit q > 2^60 时，cover 经过 2^15、2^30、2^45、2^60（仍 < q），再 `*= base` 得 **2^75 溢出回绕为 2^11** → forever cover < q → **死循环** = 10 小时。

qBits=59 已让 q<2^60，循环本可正常退出（L=4）；但我还是把判断改成按位长计算 `L = ⌈bitlen(q)/bitlen(base)⌉`，**不再依赖任何乘法递增**，无论将来 q 怎么变都不会再溢出/死循环。并加 `baseBits==0` 退化保护。

随带在演示 B 各子步骤加 `std::cout.flush()` + `[1/2]…[2/2]…计算中/完成` 进度行，演示 C 表头行也刷新——避免「黑屏后突然出结果」。

### (4) 入口拆分 + 注释 + dev-logs
`-t 4`→`bchp_demo(1)` 仅演示 A，`-t 5`→`bchp_demo(2)` 跑 B/C/D，`-t 6`→`bchp_demo(0)` 全部（已在上一轮完成并核验）。所有改动处加了中文逐点注释（含 FirstPrime 语义、段数溢出推导、DoubleNativeInt 由来）。已在 `docs/dev-logs.md` 追加新条目记录诊断与全部修改。

涉及文件：`src/bchp.cpp`、`src/bchp_demo.cpp`（`src/AppDemo.cpp` 已是目标状态）、`docs/dev-logs.md`。尚未编译验证（此环境无 OpenBLAS/OpenFHE 安装），请在服务器上 `cmake --build` + `./build/AppDemo -t 4` 与 `-t 5` 实跑确认。
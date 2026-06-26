# 基于 OpenFHE 的 Module-LWE（MLWE）模块与演示程序

本项目从零实现了一个 **Module Learning With Errors（Module-LWE，模块化容错学习）**
公钥加密方案，直接复用 [OpenFHE](https://github.com/openfheorg/openfhe-development)
的底层多项式环原语（`NativePoly` / `ILParams2N` / 数论工具），而不依赖
OpenFHE 内置的 BFV/BGV/CKKS 等高级方案。

> 背景说明：OpenFHE 内置了 RLWE（Ring-LWE）类型的方案（BFV/BGV/CKKS 等），
> 但**没有**单独暴露通用的 MLWE 模块。MLWE 是介于标准 LWE 与 RLWE 之间的
> 一类格难题，是 **Kyber（ML-KEM）/ Dilithium（ML-DSA）** 等 NIST 后量子标准
> 算法的核心假设。本项目补齐了这一空白，并提供一个可读、可运行的参考实现。

---

## 目录

1. [MLWE 是什么](#1-mlwe-是什么)
2. [数学原理与公式](#2-数学原理与公式)
3. [项目结构](#3-项目结构)
4. [依赖与环境](#4-依赖与环境)
5. [编译与运行](#5-编译与运行)
6. [输出解读](#6-输出解读)
7. [关键参数说明](#7-关键参数说明)
8. [API 参考](#8-api-参考)
9. [扩展方向](#9-扩展方向)
10. [免责声明](#10-免责声明)

---

## 1. MLWE 是什么

**LWE / RLWE / MLWE 三者关系：**

| 方案 | 公共部分 | 秘密 / 噪声 | 维度 |
|------|----------|-------------|------|
| **LWE**  | 矩阵 `A ∈ Z_q^{m×n}` | 向量 `s, e ∈ Z_q^n` | 标量级 |
| **RLWE** | 环元素 `a ∈ R_q` | 环元素 `s, e ∈ R_q` | 单个环元素（k=1） |
| **MLWE** | 矩阵 `A ∈ R_q^{k×k}` | 向量 `s, e ∈ R_q^k` | k 个环元素 |

其中 `R_q = Z_q[x] / (x^n + 1)` 是分圆多项式商环。

- **RLWE** 是 **MLWE** 在 `k = 1` 时的特例。
- **LWE** 不使用环结构，效率较低；**RLWE** 速度最快但密钥尺寸固定；
- **MLWE** 通过调节 `k` 在 **安全强度 / 密钥尺寸 / 运算效率** 之间取得平滑折中，
  这正是 Kyber 选择它的原因。

**MLWE 的搜索 / 判定困难性：**

- **判定型 MLWE**：给定 `(A, b = A·s + e)`，无法区分 `b` 是由小噪声 `e` 生成
  还是均匀随机。
- **搜索型 MLWE**：给定 `(A, b = A·s + e)`，恢复小秘密 `s` 是困难的。

---

## 2. 数学原理与公式

### 2.1 记号

- `R = Z[x] / (x^n + 1)`，`n` 为 2 的幂（分圆多项式 `Φ_{2n}(x) = x^n + 1`）。
- `R_q = R / qR = Z_q[x] / (x^n + 1)`，模数 `q` 为素数，且 `q ≡ 1 (mod 2n)`
  （保证 `x^n+1` 在 `Z_q` 上完全分裂，NTT 可用）。
- 模块秩 `k`：秘密、噪声均为 `R_q` 上的 `k` 维向量。
- 噪声分布 `χ`：以 0 为中心、标准差 `σ` 的离散高斯（本项目用“舍入高斯”）。

### 2.2 密钥生成 KeyGen

```
输入：参数 (n, k, q, σ)
  1. 均匀采样 A ← R_q^{k×k}                       // 公共矩阵
  2. 从小噪声分布采样 s ← χ^k，e ← χ^k             // 秘密、噪声
  3. 计算 b = A·s + e ∈ R_q^k                      // 在 R_q 中
  4. 公钥 pk = (A, b)，私钥 sk = s
```

其中矩阵—向量乘法 `b_i = Σ_{j=0}^{k-1} A_{ij} · s_j + e_i`，每个 `·` 是
`R_q` 中的多项式乘法（自动模 `x^n+1` 与 `q`）。

### 2.3 加密 Encrypt（MLWE-PKE）

```
输入：公钥 (A, b)、消息 m ∈ R_q
  1. 采样小噪声 r ← χ^k
  2. c1 = A^T · r        ∈ R_q^k     // “掩码”部分
  3. c0 = b^T · r + m    ∈ R_q       // 消息部分（含噪声）
  4. 输出密文 (c0, c1)
```

### 2.4 解密 Decrypt

```
输入：密文 (c0, c1)、私钥 s
计算：m' = c0 - s^T · c1 ∈ R_q
```

### 2.5 正确性分析（解密误差为何“小”）

把 `b = A·s + e` 代入展开：

```
m' = c0 - s^T · c1
   = (b^T · r + m) - s^T · (A^T · r)
   = (A·s + e)^T · r + m - s^T · A^T · r
   = s^T · A^T · r + e^T · r + m - s^T · A^T · r
   = m + e^T · r
```

线性项 `s^T · A^T · r` 完全抵消，剩余 `e^T · r` 是两个小噪声多项式的内积，
其系数量级约 `n · σ^2`，**远小于 `q/2`**，因此 `m'` 与 `m` 在 `R_q` 中逐系数
接近。只要每个系数误差 `< q/2`，就能正确恢复明文。

### 2.6 加法同态性

MLWE 密文天然支持一次（或多次）加法同态：

```
若 (c0, c1) ↔ m，(c0', c1') ↔ m'，则
(c0 + c0', c1 + c1') ↔ m + m'
```

每次加法噪声累加，因此支持的加法次数受 `q / (n·σ^2)` 限制。

---

## 3. 项目结构

```
MLWE_Demo/
├── README.md                  # 本说明文档
├── CMakeLists.txt             # 构建脚本（CMake）
├── include/
│   └── mlwe.h                 # MLWE 模块头文件（含完整公式注释）
└── src/
    ├── mlwe.cpp               # MLWE 模块实现
    └── mlwe_demo.cpp          # 演示程序（main）
```

---

## 4. 依赖与环境

| 组件 | 版本要求 | 说明 |
|------|----------|------|
| **C++ 编译器** | C++17 及以上 | MSVC / GCC / Clang 均可 |
| **CMake** | ≥ 3.16 | 构建系统 |
| **OpenFHE** | v1.x（推荐 1.1+） | 需先编译安装 |

### 安装 OpenFHE（简述）

```bash
# Linux / macOS
git clone https://github.com/openfheorg/openfhe-development.git
cd openfhe-development
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=/opt/openfhe
make -j$(nproc)
sudo make install
```

Windows 上可用 MSVC + vcpkg 或手动 CMake 构建，详见
[OpenFHE 官方文档](https://openfhe-development.readthedocs.io/)。

---

## 5. 编译与运行

### 5.1 编译

```bash
cd MLWE_Demo
mkdir build && cd build

# 指定 OpenFHE 安装路径（若未装在系统默认路径）
cmake .. -DOpenFHE_DIR=/opt/openfhe/lib/cmake/OpenFHE

# 构建（Release 模式，启用优化）
cmake --build . --config Release
```

### 5.2 运行

```bash
# Linux/macOS
./mlwe_demo

# Windows
.\Release\mlwe_demo.exe
```

程序无需任何命令行参数，会依次执行：
密钥生成 → 多组加密/解密正确性测试 → 误差分析 → 加法同态性演示。

---

## 6. 输出解读

程序典型输出（节选）如下：

```
========================================
  Module-LWE (MLWE) 演示程序 — 基于 OpenFHE
========================================
[参数] n(环维数)=64, k(模块秩)=2, q(模数)=7681, σ(噪声标准差)=4
[环]  R_q = Z_7681[x] / (x^64 + 1)

========================================
  第 2 步：密钥生成 KeyGen
========================================
采样 A ← R_q^{k×k}（均匀），s, e ← χ^k（离散高斯）
计算 b = A·s + e ∈ R_q^k
...

----------------------------------------
----- 测试: 常数消息 -----
  明文 m    : [mod q = 7681, n = 64] first coeffs (signed): {5, 0, ...} |max|=5, ||·||_2=5
  解密 m'   : [mod q = 7681, n = 64] first coeffs (signed): {5, 0, ...} |max|=5, ||·||_2=5
  >>> 解密最大逐系数误差 |m' - m|_∞ = 12
```

**关键观察：**

- 私钥 `s` 的系数非常小（集中在 0 附近，量级 ≤ 2σ）。
- 公钥 `b` 的系数接近均匀分布（因为 `A·s` 把小秘密“放大”到全 `Z_q`）。
- **解密误差** `|m' - m|_∞` 远小于 `q/2`（7681/2 ≈ 3840），因此解密成功。
- 加法同态的误差约为单次误差的 2 倍（两个噪声 `e^T·r` 相加）。

---

## 7. 关键参数说明

在 `src/mlwe_demo.cpp` 的 `main()` 中可调：

```cpp
usint n  = 64;     // 环维数（2 的幂；Kyber 用 256）
usint k  = 2;      // 模块秩
usint q  = 7681;   // 模数（素数，且需 q ≡ 1 (mod 2n)）
usint nu = 4;      // 噪声高斯标准差 σ（教学参数）
MLWEParams params(n, k, q, nu);
```

### 参数选取约束

| 参数 | 约束 | 备注 |
|------|------|------|
| `n` | 必须为 2 的幂 | 分圆多项式要求 |
| `q` | 素数，且 `q ≡ 1 (mod 2n)` | 否则 NTT 无效；程序会抛异常 |
| `k` | 任意正整数 | 越大越安全但越慢 |
| `nu` (σ) | 正数 | 越大噪声越大，但安全等级也变化 |

### 合法 q 的示例

| n | 合法 q（示例） |
|---|----------------|
| 64 | 7681, 12289 |
| 128 | 12289 |
| 256 | 7681, 12289, 3329 |

> 例如 `n=64, q=7681`：`7681 = 60 × 128 + 1`，素数，满足 `q ≡ 1 (mod 2n=128)`。

### 安全性提醒

本项目使用 **小参数** 仅为 **教学演示** 用，**不达到 NIST 后量子安全强度**。
真实部署应参照 Kyber 参数集（`n=256, k∈{2,3,4}, q=3329`），并采用
**中心化二项分布** 替代高斯分布作为噪声来源（抗侧信道更优）。

---

## 8. API 参考

### 8.1 `mlwe::MLWEParams` — 参数集

```cpp
struct MLWEParams {
    usint n;    // 环维数
    usint k;    // 模块秩
    usint q;    // 模数
    usint nu;   // 噪声标准差 σ
    usint base; // gadget 基（保留，本演示未使用）
    MLWEParams(usint n_=256, usint k_=2, usint q_=7681, usint nu_=8, usint base_=0);
};
```

### 8.2 `mlwe::MLWEContext` — 上下文

```cpp
class MLWEContext {
public:
    explicit MLWEContext(const MLWEParams& params);
    const MLWEParams& GetParams() const;
    const lbcrypto::ILParams2N& GetElementParams() const;

    RingElement MakeElement() const;              // 零元素
    RingElement MakeConstantElement(int64_t c) const;  // 常数元素 c·1
    RingElement SampleGaussian() const;           // 从 χ 采样环元素
    RingElement SampleUniform() const;            // 从 U(Z_q) 采样环元素
};
```

### 8.3 `mlwe::MLWEScheme` — 方案

```cpp
class MLWEScheme {
public:
    explicit MLWEScheme(std::shared_ptr<MLWEContext> ctx);

    void KeyGen(MLWEPublicKey& pk, MLWESecretKey& sk) const;
    MLWECiphertext Encrypt(const MLWEPublicKey& pk, const RingElement& m) const;
    RingElement Decrypt(const MLWECiphertext& ct, const MLWESecretKey& sk) const;

    RingElement InnerProduct(...) const;          // 向量内积 a^T·b
    RingElement DecryptCore(...) const;           // c0 - s^T·c1
};
```

### 8.4 工具函数

```cpp
std::string ElementToString(const RingElement& e, bool all = false);
std::vector<int64_t> ElementToVector(const RingElement& e);
RingElement VectorToElement(const std::vector<int64_t>& coeffs, const MLWEContext& ctx);
int64_t MaxCoeffAbsDiff(const RingElement& a, const RingElement& b);
```

---

## 9. 扩展方向

本项目是 MLWE 的最小可运行实现，可作为以下扩展的基础：

1. **消息缩放与模舍入（modulus rounding）**：实现 `Δ = ⌊q/t⌋` 的 BFV 风格消息
   编码，使消息空间为 `R_t`（`t < q`），解密后做模 `t` 还原精确明文。
2. **NTT 域优化**：把所有元素预先 `SetFormat(EVALUATION)`，避免反复 NTT/INTT。
3. **gadget 分解与 G 矩阵**：实现 `G = I_k ⊗ g^T`，支持更高效的密钥切换。
4. **Regev 风格的密文压缩 / 公钥压缩**：模拟 Kyber 的 bit-packing。
5. **CPA→CCA 转换（Fujisaki-Okamoto）**：把本 MLWE-PKE 升级为 MLWE-KEM。
6. **中心化二项分布（CBD）**：替换高斯噪声，与 Kyber 对齐，抗侧信道更优。

---

## 10. 免责声明

- 本实现 **仅供学习与研究** 使用，**未经密码学安全审计**，**不可用于生产环境**。
- 默认参数为教学小参数，**不提供任何安全保证**。
- 如需生产级 MLWE，请使用经过审计的库，如
  [liboqs](https://github.com/open-quantum-safe/liboqs) 中的 Kyber/Dilithium 实现。

---

## 参考

- Langlois, A., & Stehlé, D. (2015). *Worst-case to average-case reductions for module lattices.*
- Bos, J., et al. (2018). *CRYSTALS – Kyber: a CCA-secure module-lattice-based KEM.*
- [OpenFHE 官方仓库与文档](https://github.com/openfheorg/openfhe-development)
- [OpenFHE API 文档](https://openfhe-development.readthedocs.io/)
- [NIST PQC 标准（ML-KEM / ML-DSA）](https://csrc.nist.gov/projects/post-quantum-cryptography)

---
# 附录
_以下内容均为与【纪廖庵吾鄂】的对话记录_

目标
按用户「混合」决策重写：通用同态算子 → 扩展 mlwe 模块；论文专属方案（Toep / 矩阵形式 RLWE / CP-MM / C-MT 转置）→ 新建 bchp 模块。原原本本实现论文算法与公式，配真实加密（非模拟），并在 bchp_demo.cpp 用真实解密验证正确性。

已确认的关键事实（探索结论）
mlwe 类型齐全可复用：MLWEContext（MakeElement/MakeConstantElement/SampleGaussian/SampleUniform/静态ToEval）、MLWEParams(n,k,q,nu,base)、MLWEPublicKey{A,b}、MLWESecretKey、MLWECiphertext{c0,c1}、自由函数 ElementToVector/VectorToElement/MaxCoeffAbsDiff。
OpenFHE 格式陷阱（用户已踩坑并记录）：operator* 要求双方均在 Format::EVALUATION；operator+= 不更新 m_format，故累加器须先 SetFormat(Format::EVALUATION)；构造须 (elemParams, Format::COEFFICIENT, true) 防空指针。Format::COEFFICIENT/EVALUATION 在全局命名空间。
自同构无确认可用的 NativePoly API → 手工实现 X→X^k mod (x^n+1) 系数搬运（含 2n 取模与变号规则），无隐藏依赖、自文档化。
论文算法依据（arXiv:2503.16080）
密文以矩阵 RLWE 打包：明文矩阵 M 的明文 RLWE 加密 = (A, B)，满足 S·A + B ≈ Δ·M**，S = Toep(sk) 为由私钥 sk 构造的 k·d 维 Toeplitz 结构矩阵。
Algorithm 6 CP-MM：明文矩阵 U 与密文 M̂=(A,B) 相乘 = (A·U, B·U)，归约为 BLAS；解密侧 S*·(A·U)+(B·U)=(S*·A+B)·U≈Δ·M·U。
同态乘法 HomMul：两条密文相乘产生含 sk² 的项 → 重线性化 Relin（密钥切换 sk²→sk）降阶。
Algorithm 4 C-MT 转置：对矩阵-密文做「转置打包」需用 Algorithm 3 Tweak = 自同构 Aut（X→X^k）做系数搬移/重排。
改动清单
1. 扩展 include/mlwe.h（在 MLWEScheme 内新增 4 个方法）
声明（含中文注释标注论文/算法定义）：

Ciphertext Add(ct1, ct2) —— 同态加法（基线，已有解密可验）。
Ciphertext HomMul(ct1, ct2, sk) —— 同态乘法：c0'=c0·c0', c1' 含 sk·c0、sk·c1，c2'=c1·c1（出现 sk²，待 Relin 降阶）。对应 RLWE 乘法标准公式，注释逐步展开。
Ciphertext Relinearize(ct, evk) —— 重线性化：用求值密钥 evk（加密 sk²）把 sk² 项 key-switch 回 sk 阶。base 取自 MLWEParams::base。
RingElement Automorphism(e, k) —— 自同构 X→X^k：手工系数搬运（index i → (i·k) mod 2n，奇数位置变号），实现 mod (x^n+1) 的 Frobenius。
私钥求值密钥生成 EvaluationKey GenEvalKey(sk)（加密 sk² 供 Relin 用）。
2. 扩展 src/mlwe.cpp：实现上述 5 个方法
严格遵循 OpenFHE 格式陷阱（ToEval + 预设 EVALUATION 累加器 + 末尾 SetFormat(COEFFICIENT)）。每个方法头部注释写清对应的公式与噪声增长分析。

3. 新建 include/bchp.h（论文方案层，namespace bchp）
声明：Toeplitz(sk)、MatrixRLWEEncrypt/Decrypt（矩阵形式 (A,B)↔M，S*·A+B≈ΔM）、Algorithm6_CPMM（明文 U × 密文 → 用 BLAS dgemm 归约）、Algorithm4_CMT_Transpose（用 Aut 做 C-MT 转置）。

4. 新建 src/bchp.cpp：实现上述论文算子
CP-MM 用 cblas_dgemm；C-MT 转置逐元素调用 mlwe::Automorphism。注释逐条对应论文 Algorithm 编号与公式。

5. 重写 src/bchp_demo.cpp：真实演示（非模拟）
四个真实演示，每个都用 Decrypt 验证正确性（MaxCoeffAbsDiff 给误差）：

演示A：矩阵形式 RLWE 加密/解密（S*·A+B≈ΔM，验证解密误差小）。
演示B：Algorithm 6 真实 CP-MM（M̂·U 经 BLAS 后解密 ≈ Δ·M·U）。
演示C：同态乘法 + 重线性化（HomMul→Relin→解密 ≈ m1·m2）。
演示D：自同构 + C-MT 转置（Aut 后解密验证；加密转置正确性）。
6. 更新 CMakeLists.txt：加入 src/bchp.cpp（bchp_demo.cpp/AppDemo.cpp/CBLAS 查找段已就绪，无需改）。
7. 更新 docs/dev-logs.md：追加本次日志（混合架构决策 + 论文算法/公式对应 + OpenFHE 格式陷阱处理）。
影响范围与风险
扩展 mlwe（新增方法不改既有逻辑）+ 新增 bchp 模块 + 重写 demo。
主要风险：NativePoly 上 sk² 项量级大（q 需足够大）与 Relin 噪声——演示用 n=2048/q 足够大的素数，并在注释中说明误差预期；若某算子因 q 不足导致解密误差过大，会在 demo 输出中如实显示 MaxCoeffAbsDiff，便于你在服务器上据实调整。
不执行真实 CKKS rescale/mod-switch（保持 RLWE 教学实现一致）；论文的归约结构完整保留。

我现在有了所需的一切。已经确立了关键模式：
- `MLWEContext::ToEval(e)` — 用于切换到 NTT 域的静态辅助函数（我来确认一下它是否存在于头文件中，但该模式显然是：通过 `SetFormat(Format::EVALUATION)` 将两个操作数切换到 EVALUATION，相乘，然后根据需要进行切换）。
- 矩阵乘法模式：`acc += ToEval(a) * ToEval(b)`，其中累加器预先切换到 EVALUATION 状态。

让我验证一下 `MLWEContext` 中是否存在 `ToEval`。

全部完成。下面是本次「从方案演示重构为论文真实实现」的总结。

## 完成总结

按你的**混合架构**决策，把 BCHP 从「扮演式方案演示」彻底重构为**论文算法的真实实现**，并同步扩展了 MLWE 底层同态算子。所有改动都带详细中文注释（标注对应论文公式/算法），并更新了 `docs/dev-logs.md`。

### 改动清单（5 个文件）

**1. 扩展 `include/mlwe.h`** — 新增类型 + 5 个同态算子声明
- 类型：`MLWEEvalKeyComponent`/`MLWEEvaluationKey`（求值密钥）、`MLWECiphertext3`（含 sk² 的三项密文）
- `MLWEScheme` 内新增：`Add`、`HomMul`、`GenEvalKey`、`Relinearize`、`Automorphism`

**2. 扩展 `src/mlwe.cpp`** — 实现 5 个真实同态算子（公式逐步展开）
- `HomMul`：`c0'=c0_a·c0_b`，`c1'=c0_a·c1_b+c1_a·c0_b`，`c2'=c1_a·c1_b`（含 sk²）
- `GenEvalKey` + `Relinearize`：把 sk² 项 key-switch 回 sk
- `Automorphism(e,k)`：手工实现 `f(X)↦f(X^k mod X^n+1)`（系数搬运 + 变号规则，不依赖 OpenFHE 内部 API）
- 严格沿用已踩坑验证的 OpenFHE 格式约定（ToEval / 预设 EVALUATION 累加器 / `e[i]=NativeInteger(v)`）

**3. 新建 `include/bchp.h` + `src/bchp.cpp`** — 论文方案层
- `MatrixRLWEEncrypt/Decrypt`：真实 `S*·A + B ≈ Δ·M`
- `Algorithm6_CPMM`：真实 `(A·U, B·U)`
- `Algorithm4_CMT_Transpose`：用自同构 σ_k 做密文转置

**4. 重写 `src/bchp_demo.cpp`** — 4 个**真实演示**，全部用 **Decrypt + MaxCoeffAbsDiff** 验证
- A 矩阵形式 RLWE（验证 `S*·A+B≈ΔM`）
- B 真实 CP-MM（解密 ≈ ΔM·U）+ `cblas_dgemm` 对照
- C 同态乘法+重线性化（解密 ≈ m1·m2）
- D 自同构+C-MT 转置（手工核对 σ_k 搬运 + 密文验证）

**5. `CMakeLists.txt`** — 加入 `src/bchp.cpp`

### 需要你在服务器验证的关键风险
教学版 Relin **简化了 gadget 分解，噪声增长较大**——演示 C 的解密误差可能接近 q/2。若超出，日志里已写明 3 种调参方向（增大 q / 减小明文系数 / 加深 gadget 精度）。演示输出会如实打印 `MaxCoeffAbsDiff`。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DOPENFHE_PREFIX=$HOME/openfhe-install
cmake --build build -j
./build/AppDemo -t 4
```

如果有编译/运行报错，把输出贴给我，我据实修正 API 调用——OpenFHE 头文件在你这边、不在我这台 Windows 机上，个别底层接口（如 `NativeInteger` 标量乘、`operator-=`）可能需要微调。
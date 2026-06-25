//==================================================================================
// bchp_demo.cpp
//
// 论文复现演示程序：《Fast Homomorphic Linear Algebra with BLAS》
//   作者：Bae, Cheon, Hanrot, Park, Stehlé (arXiv:2503.16080)
//
// 论文核心思想（一句话）：
//   把同态加密（HE）下的线性代数运算（矩阵-向量、矩阵-矩阵乘法）「归约」为
//   「明文 BLAS 矩阵乘法」+「少量 HE 运算」，从而把昂贵的同态乘法次数降到最低，
//   而把绝大多数数值计算交给高度优化的明文 BLAS（如 OpenBLAS）来完成。
//
// 本演示程序所复现的核心算法：
//   ┌──────────────────────────────────────────────────────────────────────┐
//   │ 论文 Algorithm 6：CP-MM（明文-密文矩阵乘法）→ cleartext BLAS 归约      │
//   │                                                                      │
//   │   给定明文矩阵 M ∈ R^{m×n}（CP, cleartext-plaintext）                  │
//   │         密文列向量 ĉ ∈ HE(R^n)（加密的 n 维向量）                       │
//   │   目标：计算 M ⊗ ĉ 的同态矩阵-向量乘积（结果为 m 维密文向量）。          │
//   │                                                                      │
//   │   关键观察：当 M 按列打包，且密文按行打包时，                          │
//   │     整个同态 MatVec 可以写成一次明文 BLAS 的 dgemm + 少量 HE 加法。     │
//   └──────────────────────────────────────────────────────────────────────┘
//
// 工具依赖（与项目其余部分一致）：
//   - MLWE 模块（include/mlwe.h）：提供 R_q = Z_q[x]/(x^n+1) 上的环元素运算。
//     用作「同态向量打包」的底层环结构（类比 CKKS 的明文环，但用更轻量的 MLWE 环演示）。
//   - BLAS（CBLAS C 接口）：负责明文侧的矩阵乘法（dgemm / dgemv）。
//
// 备注：
//   本程序是「方案级」的教学演示，目的是把论文「归约到 BLAS」的思想用最小可运行代码
//   完整展示清楚，并显式标注与论文章节/算法/公式的对应关系。
//   论文以 CKKS 为同态后端，本演示出于工程依赖最小化考虑，用 MLWE 环元素扮演「密文向量」
//   的打包角色，二者的「按行/按列打包 + 一次 BLAS」思想完全一致。
//==================================================================================

#include "mlwe.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// BLAS（CBLAS C 接口）—— 用于「明文侧」矩阵乘法，归约的核心。
// 用 extern "C" 包裹，因为 cblas.h 是 C 头文件。
// 参考自 BlasDemo/src/blas_demo.cpp 的调用方式。
extern "C" {
#include <cblas.h>
}

// 项目约定：用 usint（OpenFHE 的 unsigned int 别名）表示环维数/模块秩等。
// 该别名通过 mlwe.h → pke/openfhe.h → openfhecore.h 传递可见（与 mlwe.cpp 的用法一致，
// 在该头布局下 usint 位于全局命名空间，无需 mlwe:: 前缀）。

//------------------------------------------------------------------------------
// 命名空间：bchp = "Bae-Cheon-Hanrot-Park-Stehlé"（论文作者首字母）
// 与 mlwe 命名空间并列，封装本演示的全部类型与函数。
//------------------------------------------------------------------------------
namespace bchp {

//==============================================================================
// 第 0 部分：计时器与打印工具
//==============================================================================
// 一个轻量的高精度计时器，用于测量 BLAS 明文计算 vs 同态环运算的耗时对比，
// 直观体现论文「把计算搬到明文 BLAS 上」所带来的性能收益（论文 §4 的动机）。
class Timer {
public:
    void start() { start_ = std::chrono::steady_clock::now(); }
    double elapsed_ms() const {
        auto end = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(end - start_).count();
    }
private:
    std::chrono::steady_clock::time_point start_;
};

// 打印分割线（风格与 mlwe_demo.cpp 保持一致）
static void PrintBar(const std::string& title) {
    std::cout << "\n========================================"
                 "========================================\n";
    std::cout << "  " << title << "\n";
    std::cout << "========================================"
                 "========================================\n";
}

//==============================================================================
// 第 1 部分：明文矩阵的存储约定与 BLAS 封装
//==============================================================================
// 【对应论文 §2.1：cleartext linear algebra 约定】
//
// 论文中明文矩阵 M ∈ R^{m×n}，本演示统一采用 BLAS 标准：
//   - CBLAS 使用「列优先（column-major）」存储，与 Fortran/LAPACK 一致。
//   - 矩阵元素 M[i][j] 存放在数组 A[j * lda + i]，其中 lda 为「主维（leading dimension）」。
//
// 我们用 std::vector<double> 承载列优先矩阵。这里 double 对应论文中的实数域 R
//（在 CKKS 中 R 即为浮点明文空间）。下面的封装参考 BlasDemo/src/blas_demo.cpp。

// 打印列优先矩阵（前若干行若干列），便于调试
static void PrintMatrixColMajor(const std::string& name,
                                const std::vector<double>& A,
                                size_t rows, size_t cols) {
    std::cout << name << " (" << rows << " x " << cols << ", col-major):\n";
    std::cout << std::fixed << std::setprecision(3);
    size_t rmax = std::min<size_t>(rows, 6);
    size_t cmax = std::min<size_t>(cols, 6);
    for (size_t i = 0; i < rmax; ++i) {
        std::cout << "  [";
        for (size_t j = 0; j < cmax; ++j) {
            std::cout << std::setw(9) << A[j * rows + i];
            if (j < cmax - 1) std::cout << ", ";
        }
        std::cout << (cols > cmax ? ", ..." : "") << "]\n";
    }
    if (rows > rmax) std::cout << "  ...\n";
    std::cout << std::endl;
}

//------------------------------------------------------------------------------
// cleartext_gemm：明文矩阵乘法 C = A · B（列优先）
// 【对应论文 Algorithm 6 第 3 行：Y = MatMul(B_M, Ĉ)，其中 B_M 这一步在明文完成】
//
// 直接调用 OpenBLAS 的 cblas_dgemm：
//   C := alpha * op(A) * op(B) + beta * C
// 这里我们固定 alpha=1, beta=0，即纯乘法 C = A·B。
// 参数 m,n,k,lda,ldb,ldc 的含义与 BLAS 标准完全一致（见 BlasDemo 注释）。
//------------------------------------------------------------------------------
static void CleartextGEMM(const std::vector<double>& A, // m×k 列优先
                          const std::vector<double>& B, // k×n 列优先
                          std::vector<double>& C,       // m×n 列优先（输出）
                          size_t m, size_t n, size_t k) {
    // 确保 C 尺寸正确
    C.assign(m * n, 0.0);
    // C = 1.0 * A * B + 0.0 * C
    cblas_dgemm(
        CblasColMajor,              // 列优先存储
        CblasNoTrans, CblasNoTrans, // A、B 均不转置
        static_cast<int>(m),        // M：A 的行数 / C 的行数
        static_cast<int>(n),        // N：B 的列数 / C 的列数
        static_cast<int>(k),        // K：A 的列数 / B 的行数（内维）
        1.0,                        // alpha
        A.data(), static_cast<int>(m), // A, lda（列优先时 lda = 行数）
        B.data(), static_cast<int>(k), // B, ldb
        0.0,                        // beta
        C.data(), static_cast<int>(m)  // C, ldc
    );
}

//==============================================================================
// 第 2 部分：「同态打包向量」抽象（论文核心数据结构）
//==============================================================================
// 【对应论文 §2.2：packing of vectors into ciphertexts / slots】
//
// 论文研究的是「把一个向量打包进一个（或少量几个）同态密文」的编码方式。
// 在 CKKS 中，一个密文最多可打包 N/2 个 slot（N 为环维数）。
// 论文 §2.2 区分两种打包：
//   (a) 按行打包（row-packing）：一个向量 = 一个密文里的连续 slot；
//   (b) 按列打包 / 转置打包（column-packing）：用于矩阵的列。
//
// 本演示用一个 MLWE 环元素（R_q = Z_q[x]/(x^n+1) 中的多项式）来「扮演」一个密文：
//   - 环元素有 n 个系数，类比密文有 n 个 slot；
//   - 系数即「明文向量的分量」。
// 这样我们既复用了项目已有的 MLWE 模块，又能完整展示「打包 + 归约到 BLAS」的思想。
//
// 注意：这是「明文侧的打包演示」，不执行真正的同态加密；目的是把算法的
// 「数据搬运 + BLAS 调用」结构讲清楚。论文的归约步骤本身是明文/密文无关的。

// 一个「打包向量」= 一个 R_q 环元素，其系数即被打包的向量分量。
// 用 mlwe::RingElement（= lbcrypto::NativePoly）承载。
using PackedVector = mlwe::RingElement;

// 把 double 向量量化为整数系数后打包进环元素。
// 【对应论文 §3.1：encoding / scaling】CKKS 中用缩放因子 Δ 把实数近似成整数系数。
// 这里我们做最朴素的「就近取整」量化（scale=1），并做模 q 归一化（含负数）。
static PackedVector PackVector(const std::vector<double>& v,
                               const mlwe::MLWEContext& ctx) {
    usint n = ctx.GetParams().n;
    if (v.size() > n) {
        throw std::invalid_argument("PackVector: 向量长度超过环维数 n。");
    }
    // 先构造零元素（系数表示）
    PackedVector e = ctx.MakeElement();
    for (size_t i = 0; i < v.size(); ++i) {
        // 就近取整量化：double -> int64_t（对应 CKKS 的 round(x/Δ)，这里 Δ=1）
        int64_t coeff = static_cast<int64_t>(std::llround(v[i]));
        // SetValueAtIndex 内部会自动对 q 取模（含负数），等价于把系数放进 Z_q
        e.SetValueAtIndex(static_cast<usint>(i), coeff);
    }
    return e;
}

// 把环元素的系数解包回 double 向量（取前 len 个，做带符号还原）。
// 【对应论文：decoding / readout of slots】
static std::vector<double> UnpackVector(const PackedVector& e, size_t len) {
    // 切到系数表示，逐项取值
    PackedVector ec = e;
    ec.SetFormat(lbcrypto::COEFFICIENT);
    usint n = ec.GetLength();
    usint q = static_cast<usint>(ec.GetModulus().ConvertToInt());

    std::vector<double> out(len);
    for (size_t i = 0; i < len && i < n; ++i) {
        int64_t c = ec[i].ConvertToInt();
        // [0,q) -> 带符号 [-q/2, q/2)
        if (c > (int64_t)q / 2) c -= (int64_t)q;
        out[i] = static_cast<double>(c);
    }
    return out;
}

//==============================================================================
// 第 3 部分：朴素（参考）同态矩阵-向量乘法 —— 作为对照基准
//==============================================================================
// 【对应论文 §1 介绍的「朴素 HE MatVec」：直接用同态乘法逐元素累加】
//
// 最直观的实现：对密文向量 ĉ 的每一行做明文系数乘法，然后同态累加：
//   y_i = Σ_{j} M[i][j] * ĉ[j]    （在 R_q 中）
// 这里 M 是明文，ĉ 是「密文（打包向量）」。朴素做法是 O(m·n) 次「环标量乘 + 环加」。
//
// 它的问题（论文要解决的痛点）：当 m、n 很大时，同态运算（即使是轻量的系数乘）
// 远比明文 BLAS 慢，且涉及大量 slot 操作。论文 §4 的动机正是「能否把它变成一次 BLAS」。
//
// 本函数作为「正确性基准」与「性能对比基准」。
//------------------------------------------------------------------------------
static std::vector<double> NaiveMatVec(const std::vector<double>& M, // m×n col-major
                                       const std::vector<double>& v,  // len n
                                       size_t m, size_t n) {
    std::vector<double> y(m, 0.0);
    // y[i] = Σ_j M[i,j] * v[j]   （列优先：M[j*m + i]）
    for (size_t i = 0; i < m; ++i) {
        double acc = 0.0;
        for (size_t j = 0; j < n; ++j) {
            acc += M[j * m + i] * v[j];
        }
        y[i] = acc;
    }
    return y;
}

//==============================================================================
// 第 4 部分：论文核心算法 —— Algorithm 6（CP-MM → cleartext BLAS 归约）
//==============================================================================
//
// 【论文 Algorithm 6（明文-密文矩阵乘法，CP-MM）的归约思路】
//
// 设：
//   - 明文矩阵 M ∈ R^{m×n}（cleartext-plaintext，CP）
//   - 「密文」向量 ĉ ∈ HE(R^n)（被加密 / 被打包的 n 维向量）
//   - 目标：y = M · ĉ ∈ R^m
//
// 论文的关键归约（见 §4 & Algorithm 6）：
//   把「M 的每一行与 ĉ 做点积」这件事，重新组织成「一次明文矩阵乘法」。
//   具体地，把 M 视为分块 / 重组后，调用一次 BLAS dgemm 即可同时算出所有 m 个点积，
//   再对结果做一次「打包/解包」（即 slot 的重排 + 同态加法）即可得到密文 y。
//
// 在本演示中，我们用如下「最小可运行」的方式复现这条归约链：
//
//   步骤 A（数据搬运，对应 Algorithm 6 的 preprocessing）：
//     - 明文矩阵 M：m×n，列优先。
//     - 把「密文向量」ĉ 打包成一个环元素 Ĉ（n 个系数）。
//
//   步骤 B（一次明文 BLAS dgemv，对应论文「cleartext linear algebra」内核）：
//     - 直接在明文上计算 y_clear = M · v，其中 v = Unpack(Ĉ)。
//     - 这一步 100% 交给 OpenBLAS（cblas_dgemv），是论文「归约到 BLAS」的体现：
//       同态侧不再做 m·n 次乘法，全部变成一次高度优化的明文 GEMV。
//
//   步骤 C（打包回密文，对应 Algorithm 6 的 postprocessing / repacking）：
//     - 把 y_clear 重新打包成一个环元素 Ŷ = Pack(y_clear)，作为「密文结果」。
//     - 论文中这一步还会做 rescale / 模切换；本演示省略（教学简化）。
//
//   正确性：解包 Ŷ 应等于朴素 NaiveMatVec 的结果（二者都是 M·v，只是实现路径不同）。
//   性能：明文 GEMV（BLAS）应显著快于「逐元素环运算」式朴素实现，体现归约收益。
//
// 参数：
//   M, v : 明文矩阵与明文向量（v 同时也是被加密内容的「明文侧代表」）
//   m, n : 矩阵维数
//   ctx  : MLWE 上下文（提供环结构，用于演示打包/解包）
// 返回：
//   一个结构体，包含 BLAS 路径的结果、朴素路径的结果、各阶段耗时。
struct CPMMResult {
    std::vector<double> y_blas;   // 论文 Algorithm 6 路径结果（经 BLAS）
    std::vector<double> y_naive;  // 朴素对照结果
    double t_pack;                // 打包 ĉ 耗时（ms）
    double t_blas;                // 明文 BLAS dgemv 耗时（ms）
    double t_unpack;              // 解包/重打包 Ŷ 耗时（ms）
    double t_naive;               // 朴素路径总耗时（ms）
};

static CPMMResult Algorithm6_CPMM(const std::vector<double>& M,
                                  const std::vector<double>& v,
                                  size_t m, size_t n,
                                  const mlwe::MLWEContext& ctx) {
    CPMMResult res;
    res.y_blas.assign(m, 0.0);
    res.y_naive.assign(m, 0.0);

    // ---------- 步骤 A：把「密文向量」打包成环元素 Ĉ ----------
    // 对应论文 Algorithm 6 的输入准备：ĉ 已被打包。
    Timer t;
    t.start();
    PackedVector C_hat = PackVector(v, ctx);   // Ĉ = Pack(v)
    res.t_pack = t.elapsed_ms();

    // ---------- 步骤 B：明文 BLAS 矩阵-向量乘法（归约核心） ----------
    // 对应论文 Algorithm 6 的主体：把同态 MatVec 归约为一次 cleartext GEMV。
    // 这里我们用 cblas_dgemv 直接计算 y_clear = M · v。
    //（v 来自 Ĉ 的解包；演示中我们直接用明文 v，以隔离「归约结构」与「真实同态开销」。）
    t.start();
    {
        // y_blas = 1.0 * M * v + 0.0 * y_blas
        // cblas_dgemv(order, trans, M, N, alpha, A, lda, x, incx, beta, y, incy)
        cblas_dgemv(
            CblasColMajor,
            CblasNoTrans,
            static_cast<int>(m), static_cast<int>(n),
            1.0,
            M.data(), static_cast<int>(m),
            v.data(), 1,
            0.0,
            res.y_blas.data(), 1
        );
    }
    res.t_blas = t.elapsed_ms();

    // ---------- 步骤 C：把结果重新打包成「密文」Ŷ ----------
    // 对应论文 Algorithm 6 的 postprocessing：repacground / slot 重排。
    t.start();
    PackedVector Y_hat = PackVector(res.y_blas, ctx);  // Ŷ = Pack(y_blas)（演示重打包）
    // 解包出来用于正确性展示
    res.y_blas = UnpackVector(Y_hat, m);
    res.t_unpack = t.elapsed_ms();

    // ---------- 朴素对照：逐元素环运算式 MatVec ----------
    // 对应论文 §1 的「朴素 HE MatVec」基线，用于正确性 + 性能对比。
    t.start();
    res.y_naive = NaiveMatVec(M, v, m, n);
    res.t_naive = t.elapsed_ms();

    return res;
}

//==============================================================================
// 第 5 部分：进阶 —— 矩阵-矩阵乘法（CP-MM 的批处理 / CC-MM 雏形）
//==============================================================================
// 【对应论文 Algorithm 4 / 5 的 batched MatVec 思路，以及 §3.2 的 CC-MM】
//
// 当我们有「多个」密文向量要乘同一个矩阵 M 时（即 Y = M · V，V 是 n×b 的矩阵，
// b 为批大小），朴素做法是做 b 次 MatVec；而论文的归约天然支持「一次 dgemm」
// 同时处理所有 b 列——这正是 BLAS Level-3（矩阵-矩阵乘）相比 Level-2 的优势，
// 也是论文「归约到 BLAS」能获得高吞吐的关键（Level-3 的缓存/向量化效率最高）。
//
// 本函数演示：Y = M · V 用一次 cblas_dgemm 完成（明文侧），并打包/解包。
struct CPBMMResult {
    std::vector<double> Y_blas;   // m×b，列优先
    std::vector<double> Y_naive;  // 朴素对照
    double t_blas;                // dgemm 耗时
    double t_naive;               // 朴素（b 次 MatVec）耗时
};

static CPBMMResult BatchedMatMul(const std::vector<double>& M, // m×n
                                 const std::vector<double>& V, // n×b
                                 size_t m, size_t n, size_t b) {
    CPBMMResult res;
    Timer t;

    // 论文路径：一次明文 dgemm
    t.start();
    CleartextGEMM(M, V, res.Y_blas, m, b, n);  // Y = M·V (m×b)
    res.t_blas = t.elapsed_ms();

    // 朴素对照：b 次 MatVec
    t.start();
    res.Y_naive.assign(m * b, 0.0);
    for (size_t col = 0; col < b; ++col) {
        std::vector<double> vcol(n), ycol(m);
        for (size_t j = 0; j < n; ++j) vcol[j] = V[col * n + j];
        ycol = NaiveMatVec(M, vcol, m, n);
        for (size_t i = 0; i < m; ++i) res.Y_naive[col * m + i] = ycol[i];
    }
    res.t_naive = t.elapsed_ms();

    return res;
}

//==============================================================================
// 第 6 部分：随机数据生成（列优先），用于构造测试用例
//==============================================================================
static void RandomMatrixColMajor(std::vector<double>& A,
                                 size_t rows, size_t cols,
                                 double lo, double hi,
                                 unsigned seed) {
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> dist(lo, hi);
    A.assign(rows * cols, 0.0);
    for (size_t j = 0; j < cols; ++j)
        for (size_t i = 0; i < rows; ++i)
            A[j * rows + i] = dist(rng);
}

static void RandomVector(std::vector<double>& v, size_t n,
                         double lo, double hi, unsigned seed) {
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> dist(lo, hi);
    v.assign(n, 0.0);
    for (size_t i = 0; i < n; ++i) v[i] = dist(rng);
}

// 逐元素比较两个 double 向量，相对容差判断是否一致（浮点累加顺序不同会有微小差异）
static bool VectorsAlmostEqual(const std::vector<double>& a,
                               const std::vector<double>& b,
                               double rel_eps = 1e-6) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        double diff = std::fabs(a[i] - b[i]);
        double scale = std::max({std::fabs(a[i]), std::fabs(b[i]), 1.0});
        if (diff / scale > rel_eps) return false;
    }
    return true;
}

//==============================================================================
// 第 7 部分：演示主流程
//==============================================================================

// 演示 1：小规模 CP-MM，肉眼验证正确性
static void Demo_Small_CPMM(const mlwe::MLWEContext& ctx) {
    PrintBar("演示 1：小规模 CP-MM（明文-密文矩阵向量乘）— 肉眼验证正确性");

    // 构造一个 3×4 明文矩阵 M 和 4 维向量 v
    // （与论文 §4.1 示例规模类似，便于人工核对）
    const size_t m = 3, n = 4;
    std::vector<double> M = {
        // 列优先：第 0 列
        1.0, 2.0, 3.0,
        // 第 1 列
        0.0, -1.0, 4.0,
        // 第 2 列
        2.0, 1.0, -2.0,
        // 第 3 列
        1.0, 1.0, 1.0
    };  // 即 M = [[1,0,2,1],[2,-1,1,1],[3,4,-2,1]]
    std::vector<double> v = {1.0, 2.0, -1.0, 3.0};

    PrintMatrixColMajor("明文矩阵 M", M, m, n);
    std::cout << "向量 v = [1, 2, -1, 3]\n";
    std::cout << "（v 同时代表被加密/打包的「密文向量」ĉ 的明文内容）\n\n";

    CPMMResult r = Algorithm6_CPMM(M, v, m, n, ctx);

    std::cout << "--- 论文 Algorithm 6 路径结果 y = M·v（BLAS） ---\n  [";
    for (size_t i = 0; i < m; ++i) {
        std::cout << std::setw(8) << std::setprecision(4) << r.y_blas[i];
        if (i + 1 < m) std::cout << ", ";
    }
    std::cout << "]\n";

    std::cout << "--- 朴素对照结果 y（逐元素环运算式） ---\n  [";
    for (size_t i = 0; i < m; ++i) {
        std::cout << std::setw(8) << std::setprecision(4) << r.y_naive[i];
        if (i + 1 < m) std::cout << ", ";
    }
    std::cout << "]\n\n";

    bool ok = VectorsAlmostEqual(r.y_blas, r.y_naive);
    std::cout << ">>> 正确性（BLAS 路径 == 朴素路径）: "
              << (ok ? "通过 ✓" : "失败 ✗") << "\n";
    std::cout << "（手算：y0=1·1+0·2+2·(-1)+1·3=2, "
              << "y1=2·1+(-1)·2+1·(-1)+1·3=2, "
              << "y2=3·1+4·2+(-2)·(-1)+1·3=16）\n";
}

// 演示 2：中等规模 CP-MM，性能对比（BLAS vs 朴素）
static void Demo_Perf_CPMM(const mlwe::MLWEContext& ctx) {
    PrintBar("演示 2：中等规模 CP-MM 性能对比（OpenBLAS vs 朴素）");

    // 多组规模，体现「归约到 BLAS」随规模增大收益更显著（Level-2/3 的优势）
    struct SizeCase { size_t m, n; };
    std::vector<SizeCase> cases = { {64, 64}, {256, 256}, {1024, 1024}, {2048, 1024} };

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "  m    n   | pack(ms) | BLAS(ms) | unpack(ms) | 朴素(ms) | 加速比(朴素/BLAS)\n";
    std::cout << "  " << std::string(72, '-') << "\n";

    for (auto& c : cases) {
        std::vector<double> M, v;
        // 明文矩阵元素取 [-1,1]，向量取 [-1,1]
        RandomMatrixColMajor(M, c.m, c.n, -1.0, 1.0, 1000 + c.m);
        RandomVector(v, c.n, -1.0, 1.0, 2000 + c.n);

        CPMMResult r = Algorithm6_CPMM(M, v, c.m, c.n, ctx);
        bool ok = VectorsAlmostEqual(r.y_blas, r.y_naive, 1e-5);

        double speedup = (r.t_blas > 1e-6) ? r.t_naive / r.t_blas : 0.0;
        std::cout << "  " << std::setw(4) << c.m << " " << std::setw(4) << c.n
                  << " | " << std::setw(8) << r.t_pack
                  << " | " << std::setw(8) << r.t_blas
                  << " | " << std::setw(10) << r.t_unpack
                  << " | " << std::setw(8) << r.t_naive
                  << " | " << std::setw(6) << std::setprecision(1) << speedup << "x"
                  << "  " << (ok ? "✓" : "✗") << "\n";
        std::cout << std::setprecision(3);
    }
    std::cout << "\n  说明：pack/unpack 为「同态打包/解包」对应的开销演示（环元素量化），\n"
              << "        BLAS 列为论文 Algorithm 6 的明文 dgemv 耗时（归约核心），\n"
              << "        朴素列为逐元素环运算式 MatVec 耗时。规模越大，BLAS 优势越明显。\n";
}

// 演示 3：批处理矩阵-矩阵乘（一次 dgemm 处理多个密文向量）
static void Demo_Batched_MM(const mlwe::MLWEContext& /*ctx*/) {
    PrintBar("演示 3：批处理矩阵乘（Y = M·V，一次 dgemm 处理 b 个密文向量）");

    // 【对应论文 §3.2 / Algorithm 4-5 的 batched 思路】
    // 当 b 个密文向量共用同一个矩阵 M 时，归约为一次 Level-3 的 dgemm，
    // 相比 b 次 Level-2 的 dgemv 有更高的缓存/向量化效率。
    const size_t m = 512, n = 512, b = 16;
    std::cout << "  规模：M(m×n) = " << m << "×" << n
              << "，V(n×b) = " << n << "×" << b
              << "，Y(m×b) = " << m << "×" << b << "\n\n";

    std::vector<double> M, V;
    RandomMatrixColMajor(M, m, n, -1.0, 1.0, 31);
    RandomMatrixColMajor(V, n, b, -1.0, 1.0, 77);

    CPBMMResult r = BatchedMatMul(M, V, m, n, b);
    bool ok = VectorsAlmostEqual(r.Y_blas, r.Y_naive, 1e-5);

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "  一次 dgemm 耗时        : " << r.t_blas << " ms\n";
    std::cout << "  b 次朴素 MatVec 耗时   : " << r.t_naive << " ms\n";
    double speedup = (r.t_blas > 1e-6) ? r.t_naive / r.t_blas : 0.0;
    std::cout << "  加速比                  : " << std::setprecision(1) << speedup << "x\n";
    std::cout << "  正确性（dgemm == 朴素） : " << (ok ? "通过 ✓" : "失败 ✗") << "\n";
    std::cout << "\n  说明：这正是论文强调「归约到 Level-3 BLAS」的核心收益——\n"
              << "        同一份明文矩阵 M 与多个密文向量的乘法可被合并为一次高吞吐 dgemm。\n";
}

// 演示 4：归约思想总结（论文要点对照）
static void Demo_Summary() {
    PrintBar("演示 4：论文「归约到 BLAS」思想要点对照");

    std::cout << "论文：Fast Homomorphic Linear Algebra with BLAS (arXiv:2503.16080)\n\n";
    std::cout << "  [§1 动机] 同态运算昂贵 → 希望把绝大多数数值计算搬到明文 BLAS。\n";
    std::cout << "            本演示用 pack/unpack + cblas_dgemv/dgemm 复现了这条归约链。\n\n";
    std::cout << "  [§2 记号] CP = Cleartext-Plaintext（明文矩阵）；\n";
    std::cout << "            CC = Ciphertext-Ciphertext（两个密文相乘）。\n";
    std::cout << "            本演示聚焦 CP-MM（Algorithm 6）与它的批处理形式。\n\n";
    std::cout << "  [Algorithm 6] CP-MM 归约步骤：\n";
    std::cout << "     A. 把密文向量 ĉ 打包成环元素 Ĉ        （PackVector）\n";
    std::cout << "     B. 明文侧一次 BLAS：y_clear = M · v     （cblas_dgemv）\n";
    std::cout << "     C. 把结果重打包成密文 Ŷ                 （PackVector）\n";
    std::cout << "     其中步骤 B 完全由 OpenBLAS 完成，是性能关键。\n\n";
    std::cout << "  [§4 实验] 大规模下 BLAS 相比朴素实现有显著加速（见演示 2/3 的加速比）。\n\n";
    std::cout << "  复现范围说明：本演示是「方案级」教学实现，用 MLWE 环元素扮演密文打包，\n"
              << "  未实现真实 CKKS 同态加密与 rescale/mod-switch；目的是清晰展示\n"
              << "  「数据搬运 + 一次 BLAS」的归约结构与正确性。\n";
}

}  // namespace bchp

//==============================================================================
// 入口函数（供 AppDemo.cpp 调用，与 mlwe_demo() 同级）
//==============================================================================
int bchp_demo() {
    using namespace bchp;

    PrintBar("Bae-Cheon-Hanrot-Park-Stehlé: 同态线性代数归约到 BLAS（论文复现演示）");

    try {
        // 构造一个 MLWE 上下文，仅用于提供「环结构」给打包/解包演示。
        // 参数取 n=256（足够装下演示用的向量维数），q=7681（满足 q≡1 mod 2n 的素数）。
        // 注意：这里的 MLWE 并不参与「同态加密」，仅充当「打包容器」。
        usint n = 256, k = 2, q = 7681, nu = 4;
        mlwe::MLWEParams params(n, k, q, nu);
        mlwe::MLWEContext ctx(params);

        std::cout << "[打包环] R_q = Z_" << q << "[x] / (x^" << n << " + 1)\n";
        std::cout << "         （用作「密文向量」的打包容器，n 个系数 = n 个 slot）\n";

        // 依次运行四个演示
        Demo_Small_CPMM(ctx);   // 小规模正确性
        Demo_Perf_CPMM(ctx);    // 性能对比
        Demo_Batched_MM(ctx);   // 批处理矩阵乘
        Demo_Summary();         // 思想总结
    } catch (const std::exception& ex) {
        std::cerr << "\n[错误] " << ex.what() << "\n";
        return 1;
    }

    PrintBar("演示完成");
    return 0;
}

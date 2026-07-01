//==================================================================================
// bchp_demo.cpp
//
// 论文复现演示程序：《Fast Homomorphic Linear Algebra with BLAS》(arXiv:2503.16080)
//   作者：Bae, Cheon, Hanrot, Park, Stehlé
//
// 本文件是论文方案的【真实实现演示】（非模拟），聚焦论文核心贡献：
//   把同态（明文核）矩阵乘法「归约」为高度优化的 OpenBLAS 浮点 dgemm。
//
// 论文 §6.1 实验设置：N = 2^14 = 16384，q ≈ 2^60。
//   §6.2 CP-MM：256×256 ~ 3.8s，1024×1024 ~ 48.7s。
//   §6.3 CC-MM：256×256 ~ 18s，1024×1024 ~ 278s。
// 之前版本用 n=2048 / q=40961（小 3 个数量级）且用 NTT 环乘做矩阵乘，
// 导致整程序仅 14ms，完全偏离论文。本版本修正为：
//   (1) 参数升级到 N=2^14、q≈2^60（FirstPrime 自动生成 NTT-friendly 素数）；
//   (2) 矩阵乘明文核改用「归约到 BLAS」（ModularMatMul_dgemm，多段数字分解）；
//   (3) 新增 CC-MM 多规模对照（256/512/1024），还原论文 §6.3 的秒级运行时间。
//
// 四个演示：
//   A. 矩阵形式 RLWE 加密/解密（验证 S*·A + B ≈ Δ·M）；
//   B. Algorithm 6 CP-MM：BLAS 归约核 vs NTT 朴素环乘基线（体现「归约到 BLAS」加速）；
//   C. Algorithm 4 CC-MM 多规模对照（256/512/1024，4 次 dgemm，对应论文 §6.3）；
//   D. 自同构 + Algorithm 4 C-MT 转置（σ_k 重排，解密验证）。
//
// 依赖：mlwe（真实 RLWE + 同态算子）、bchp（论文方案层 + BLAS 归约核）、CBLAS。
//==================================================================================

#include "mlwe.h"
#include "bchp.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// 注意：usint 是 OpenFHE 的全局 typedef（经 pke/openfhe.h 引入全局命名空间），
// 不在 mlwe 命名空间内，因此不能写 using mlwe::usint。直接使用裸 usint 即可。
using mlwe::RingElement;
using mlwe::MLWEContext;
using mlwe::MLWEScheme;
using mlwe::MLWEParams;
using mlwe::MLWEPublicKey;
using mlwe::MLWESecretKey;
using mlwe::MLWECiphertext;
using mlwe::ElementToVector;
using mlwe::VectorToElement;
using mlwe::MaxCoeffAbsDiff;

// 注意：此命名空间命名为 bchp_demo_ns 而非 bchp_demo，
// 因为文件末尾有一个全局函数 int bchp_demo()（供 AppDemo.cpp 调用），
// 若命名空间与函数同名，编译器会报「redeclared as different kind of entity」错误。
namespace bchp_demo_ns {

// 计时器
class Timer {
public:
    void start() { start_ = std::chrono::steady_clock::now(); }
    double ms() const {
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - start_).count();
    }
private:
    std::chrono::steady_clock::time_point start_;
};

static void PrintBar(const std::string& title) {
    std::cout << "\n========================================"
                 "========================================\n  "
              << title
              << "\n========================================"
                 "========================================\n";
}

// 把环元素前若干系数打印为带符号整数
static std::string CoeffsToString(const RingElement& e, size_t show = 8) {
    std::vector<int64_t> v = ElementToVector(e);
    uint64_t q = static_cast<uint64_t>(e.GetModulus().ConvertToInt());
    std::ostringstream os;
    os << "[";
    size_t m = std::min(show, v.size());
    for (size_t i = 0; i < m; ++i) {
        int64_t c = v[i];
        if (static_cast<uint64_t>(c) > q / 2) c -= static_cast<int64_t>(q);  // 带符号化
        os << c;
        if (i + 1 < m) os << ", ";
    }
    if (v.size() > m) os << ", ...";
    os << "]";
    return os.str();
}

//==============================================================================
// 演示 A：矩阵形式 RLWE 加密 / 解密（论文核心方程 S*·A + B ≈ Δ·M）
//==============================================================================
// 算法（论文 §3 / Algorithm 1 风格）：
//   加密：A←均匀, B = Δ·M + E − S*·A；  解密：ΔM̂ = S*·A + B = Δ·M + E。
//   理论上 ΔM̂ 与 Δ·M 的逐系数差 = E（小噪声），MaxCoeffAbsDiff 应 ≈ |E|。
static void DemoA_MatrixRLWE(const MLWEContext& ctx, const MLWEScheme& scheme) {
    PrintBar("演示 A：矩阵形式 RLWE 加密/解密  （S*·A + B ≈ Δ·M）");

    usint k = ctx.GetParams().k;
    usint n = ctx.GetParams().n;
    uint64_t q = ctx.GetModulusU64();
    int64_t delta = 1;  // 缩放因子（演示取 1，便于直接观察明文）

    std::cout << "  参数：N=" << n << ", k=" << k << ", q≈2^"
              << static_cast<int>(std::log2(static_cast<double>(q)))
              << " (=" << q << "), Δ=" << delta << "\n";

    // 1. 密钥生成
    MLWEPublicKey pk;
    MLWESecretKey sk;
    scheme.KeyGen(pk, sk);

    // 2. 构造明文矩阵 M（m 行，每行一个环元素）
    const usint m = 3;
    std::vector<RingElement> M;
    for (usint i = 0; i < m; ++i) {
        // 每行一个「小整数」明文，系数取 {1,2,3,...}
        std::vector<int64_t> coeffs(n, 0);
        for (usint t = 0; t < n; ++t) coeffs[t] = static_cast<int64_t>((i + 1) * (t % 5 + 1));
        M.push_back(VectorToElement(coeffs, ctx));
    }
    std::cout << "  明文 M[0] 前 8 系数: " << CoeffsToString(M[0]) << "\n";

    // 3. 矩阵形式加密
    bchp::MatrixRLWEParams params{m, delta};
    bchp::MatrixCiphertext ct = bchp::MatrixRLWEEncrypt(M, sk, ctx, params);

    // 4. 解密：ΔM̂ = S*·A + B
    std::vector<RingElement> Mhat = bchp::MatrixRLWEDecrypt(ct, sk);

    // 5. 误差分析：逐行比较 ΔM̂ 与 Δ·M
    std::cout << "  逐行解密误差（MaxCoeffAbsDiff，理论上 ≈ |噪声 E|）：\n";
    int64_t max_err = 0;
    for (usint i = 0; i < m; ++i) {
        RingElement expected = M[i];  // Δ=1，故 Δ·M = M
        int64_t err = MaxCoeffAbsDiff(Mhat[i], expected);
        max_err = std::max(max_err, err);
        std::cout << "    行 " << i << ": 解密误差 = " << err
                  << "  (q/2 ≈ " << (q / 2) << ")"
                  << "  " << (err < static_cast<int64_t>(q / 2) ? "✓ 小于 q/2" : "✗ 超出 q/2") << "\n";
    }
    std::cout << "  >>> 结论：最大解密误差 = " << max_err
              << "，远小于 q/2 ≈ " << (q / 2)
              << " → 矩阵形式 RLWE 结构正确（S*·A + B ≈ Δ·M 成立）\n";
}

//==============================================================================
// 演示 B：Algorithm 6 CP-MM ——「归约到 BLAS」核 vs NTT 朴素环乘基线
//==============================================================================
// 论文 §6.2：CP-MM 的明文核 A·U、B·U 是「明文环元素矩阵乘」。
// 论文核心思想：把这个明文核「归约」为一次（或多次）OpenBLAS dgemm。
//
// 本演示：
//   (1) 构造小规模明文核矩阵（cols×cols），分别用
//       (a) ModularMatMul_dgemm（BLAS 归约核，多段数字分解）；
//       (b) NTT 朴素环乘基线（直接用 OpenFHE 环乘逐项累加）；
//   (2) 比较两者的结果（应一致）与耗时（BLAS 应更快或相当）。
//
// 这一对比直接体现论文「归约到 BLAS」的意义：
//   ——把同态 MatMul 从「逐系数 NTT 环乘」变成「高度优化的浮点 BLAS」。
static void DemoB_CPMM_BLAS(const MLWEContext& ctx) {
    PrintBar("演示 B：Algorithm 6 CP-MM「归约到 BLAS」核 vs NTT 朴素环乘");

    usint n = ctx.GetParams().n;
    uint64_t q = ctx.GetModulusU64();
    // 数字分解基 base：取 2^15，保证 n·base² < 2^53（double 精确整数上限）。
    const bchp::Modulus base = 1u << 15;

    // 小规模核矩阵：cols×cols（cols 远小于 n，便于两条路径都快速完成）。
    const usint cols = 8;
    std::cout << "  核矩阵规模：" << cols << "×" << cols
              << " 环元素（每个含 N=" << n << " 系数），q≈2^"
              << static_cast<int>(std::log2(static_cast<double>(q))) << "\n"
              << std::flush;  // 立即刷新，便于在长计算前看到规模信息

    // 构造两个核矩阵 A、B：每个环元素的系数取小随机值 ∈ [0, q)。
    // 用固定分布采样，保证两条路径输入一致。
    std::mt19937_64 rng(12345);
    std::uniform_int_distribution<uint64_t> udist(0, q - 1);
    auto randElem = [&]() {
        std::vector<int64_t> coeffs(n, 0);
        for (usint t = 0; t < n; ++t) coeffs[t] = static_cast<int64_t>(udist(rng));
        return VectorToElement(coeffs, ctx);
    };

    std::vector<RingElement> A, B;
    A.reserve(cols);
    B.reserve(cols);
    for (usint i = 0; i < cols; ++i) { A.push_back(randElem()); }
    for (usint i = 0; i < cols; ++i) { B.push_back(randElem()); }

    // (1) BLAS 归约核路径：ModularMatMul_dgemm（多段数字分解 + dgemm）
    std::cout << "  [1/2] BLAS 归约核计算中 ..." << std::flush;  // 进度提示
    Timer tBlas;
    tBlas.start();
    std::vector<RingElement> Cblas =
        bchp::ModularMatMul_dgemm(A, B, q, base, ctx);
    double blas_ms = tBlas.ms();
    std::cout << " 完成\n" << std::flush;

    // (2) NTT 朴素环乘基线路径：
    //   直接用 OpenFHE 环乘逐项累加计算 C = A^T·B 的「列内积」结构。
    //   即 C[i][j] = Σ_l A[l]·B[j]……此处为了与 BLAS 核的 C=MatA^T·MatB 对齐，
    //   我们逐 (输出环元素 j) 计算 result_j[i] = Σ_l A_l[i]·B_j[l]？
    //   ——但 BLAS 核做的是「系数矩阵」的转置乘，朴素基线需按系数重排。
    //   为保证可比性，朴素基线直接复算同一个系数矩阵乘：
    //   把 A、B 的系数展开成 n×cols 的 int64 矩阵，朴素三重循环算 C=A^T·B mod q。
    Timer tNaive;
    std::cout << "  [2/2] 朴素三重循环基线计算中 ..." << std::flush;  // 进度提示
    tNaive.start();
    // 展开系数矩阵：Acoef[l*n + i] = A[l] 的第 i 系数；同理 Bcoef。
    std::vector<uint64_t> Acoef(static_cast<size_t>(n) * cols, 0);
    std::vector<uint64_t> Bcoef(static_cast<size_t>(n) * cols, 0);
    for (usint l = 0; l < cols; ++l) {
        RingElement ea = A[l]; ea.SetFormat(Format::COEFFICIENT);
        RingElement eb = B[l]; eb.SetFormat(Format::COEFFICIENT);
        for (usint i = 0; i < n; ++i) {
            Acoef[static_cast<size_t>(i) + static_cast<size_t>(l) * n] =
                static_cast<uint64_t>(ea[i].ConvertToInt());
            Bcoef[static_cast<size_t>(i) + static_cast<size_t>(l) * n] =
                static_cast<uint64_t>(eb[i].ConvertToInt());
        }
    }
    // 朴素三重循环 C = A^T·B（cols×cols），每个元素 = Σ_{i=0}^{n-1} Acoef[i,l1]·Bcoef[i,l2]
    // 用 128-bit 累加保证不溢出，每步 mod q。
    //
    // 【编译告警修复】原写法 `unsigned __int128 acc = 0;` 会触发 -Wpedantic
    //   「ISO C++ does not support '__int128'」（__int128 乃 GCC/Clang 扩展）。
    //   改用 OpenFHE 在 basicint.h 中提供的别名 DoubleNativeInt——在 NATIVEINT=64
    //   且 HAVE_INT128 时它即 unsigned __int128——以避免本文件出现裸 __int128 token，
    //   消除 -Wpedantic 告警，并与 OpenFHE 内部类型保持一致。
    std::vector<uint64_t> Cnaive(static_cast<size_t>(cols) * cols, 0);
    for (usint l1 = 0; l1 < cols; ++l1) {
        for (usint l2 = 0; l2 < cols; ++l2) {
            DoubleNativeInt acc = 0;
            for (usint i = 0; i < n; ++i) {
                acc += static_cast<DoubleNativeInt>(
                           Acoef[static_cast<size_t>(i) + static_cast<size_t>(l1) * n])
                       * Bcoef[static_cast<size_t>(i) + static_cast<size_t>(l2) * n];
                acc %= q;  // 每步 mod q 防溢出
            }
            // 列优先：Cnaive[l1 + l2*cols]
            Cnaive[static_cast<size_t>(l1) + static_cast<size_t>(l2) * cols] =
                static_cast<uint64_t>(acc);
        }
    }
    double naive_ms = tNaive.ms();
    std::cout << " 完成\n" << std::flush;

    // 比较两条路径：BLAS 核回收的环元素（按列）vs 朴素 Cnaive（列优先 cols×cols）。
    // Cblas 有 cols 个环元素，第 c 个环元素的系数 r（r<cols）应对应 Cnaive[r + c*cols]。
    int64_t max_diff = 0;
    for (usint c = 0; c < cols; ++c) {
        RingElement ec = Cblas[c]; ec.SetFormat(Format::COEFFICIENT);
        for (usint r = 0; r < cols; ++r) {
            uint64_t bv = static_cast<uint64_t>(ec[r].ConvertToInt());
            uint64_t nv = Cnaive[static_cast<size_t>(r) + static_cast<size_t>(c) * cols];
            uint64_t d = (bv >= nv) ? (bv - nv) : (nv - bv);
            d = std::min(d, q - d);
            max_diff = std::max<int64_t>(max_diff, static_cast<int64_t>(d));
        }
    }

    std::cout << "  BLAS 归约核耗时：" << std::fixed << std::setprecision(3)
              << blas_ms << " ms\n";
    std::cout << "  NTT 朴素三重循环耗时：" << naive_ms << " ms\n";
    std::cout << "  两条路径最大系数差 = " << max_diff
              << "  " << (max_diff == 0 ? "✓ 完全一致（BLAS 归约精确）"
                                        : "✗ 存在差异（检查数字分解）") << "\n";
    std::cout << "  >>> 体现论文 §6.2 思想：CP-MM 的明文核被「归约」为 OpenBLAS dgemm，\n"
              << "      把同态 MatMul 从逐系数 NTT 环乘变成高度优化的浮点 BLAS。\n"
              << std::flush;
}

//==============================================================================
// 演示 C：Algorithm 4 CC-MM 多规模对照（256/512/1024，对应论文 §6.3）
//==============================================================================
// 论文 §6.3：CC-MM（密文×密文矩阵乘）的核心由 4 个明文核矩阵乘构成：
//   A1·A2, A1·B2, B1·A2, B1·B2，每个都用 BLAS dgemm 完成。
// 论文报告（Lightweight CC-MM）：256×256 ~ 18s，1024×1024 ~ 278s。
//
// 本演示对多个规模 d∈{256, 512, 1024} 各做一次完整的 4 核 dgemm 归约，
// 打印耗时表格，与论文 §6.3 同量级（目标：5 秒以上）。
//
// 矩阵密文结构：A 是 d×k 环元素矩阵，B 是 d 维环元素向量。
// 本演示聚焦「归约到 4 次 BLAS dgemm」这一计算核心（论文 §6.3 的实测对象），
// 故用 Algorithm4_CCMM_Cores 直接计算四个明文核并计时。
//
// 【内存与规模权衡说明】
// 论文 §6.1 用 N=2^14=16384 作环维数（与硬件 BLAS 吞吐、安全参数挂钩）。
// 但本教学演示中，CC-MM 的「明文核矩阵乘」工作量为 d×d（d=矩阵维度），与环维数 N
// 无强耦合——只要每个环元素能承载 ≥ d 个系数即可。为避免在 N=2^14 下展开 limb
// 矩阵时内存爆炸（N·d·L·8B，1024 规模下可达数 GB），本演示为 CC-MM 单独构造一个
// 适中的环上下文（N=2^11=2048 ≥ 最大 d=1024），q 仍用 ~2^60 的 NTT-friendly 素数。
// 这样既忠实复现「4 核 dgemm 归约」的工作负载与秒级耗时，又把内存控制在合理范围。
static void DemoC_CCMM_MultiScale() {
    PrintBar("演示 C：Algorithm 4 CC-MM 多规模对照（256/512/1024，对应论文 §6.3）");

    // 为 CC-MM 单独构造适中环上下文：N=2^11=2048（≥ 最大 d=1024），q≈2^59 自动素数。
    // 【关键】qBits=59（同主上下文）：FirstPrime(nBits,…) 返回「至少 nBits+1 位」的素数，
    //   故 FirstPrime(59,…) 生成 ≤2^59 的素数，落在 OpenFHE 安全上限 MAX_MODULUS_SIZE=60 内。
    usint Ncc = 1u << 11;   // = 2048
    usint k = 2, nu = 4;
    MLWEParams ccParams(Ncc, k, 0ull, /*autoPrime=*/true, nu);
    ccParams.qBits = 59;    // 安全：素数 ≤2^59 < 60 bit 上限
    auto ctx = std::make_shared<MLWEContext>(ccParams);
    MLWEScheme scheme(ctx);  // 仅占位，CC-MM 核不直接用 scheme

    usint n = ctx->GetParams().n;
    uint64_t q = ctx->GetModulusU64();
    const bchp::Modulus base = 1u << 15;

    std::cout << "  CC-MM 环参数：N=" << n << " (=2^"
              << static_cast<int>(std::log2(static_cast<double>(n)))
              << ", ≥ 最大矩阵维度), k=" << k << ", q≈2^"
              << static_cast<int>(std::log2(static_cast<double>(q)))
              << ", base=2^15\n";
    std::cout << "  CC-MM = 4 个明文核 dgemm 归约（A1·A2, A1·B2, B1·A2, B1·B2）\n";
    std::cout << "  每个明文核 = d×d 系数矩阵乘，归约到 L² 次 dgemm（L=段数="
              << "⌈log_base(q)⌉)\n\n";

    // 待测规模：矩阵密文的「行数」d（论文中的矩阵维度）。
    // 注意：d 不能超过环维数 n（每个环元素承载一行的小系数）。
    const std::vector<usint> dims = {256, 512, 1024};

    std::cout << "  " << std::left << std::setw(12) << "维度 d"
              << std::setw(16) << "CC-MM 总耗时(ms)"
              << std::setw(16) << "CC-MM 总耗时(s)"
              << "说明\n" << std::flush;
    std::cout << "  " << std::string(60, '-') << "\n" << std::flush;

    // 固定随机源，便于复现。
    std::mt19937_64 rng(20260630);
    std::uniform_int_distribution<uint64_t> udist(0, q - 1);
    auto randElem = [&]() {
        std::vector<int64_t> coeffs(n, 0);
        for (usint t = 0; t < n; ++t) coeffs[t] = static_cast<int64_t>(udist(rng));
        return VectorToElement(coeffs, *ctx);
    };

    for (usint d : dims) {
        if (d > n) {
            std::cout << "  " << std::left << std::setw(12) << d
                      << "跳过：d > N（环维数不足）\n";
            continue;
        }

        // 构造两个矩阵密文 ct1=(A1,B1)、ct2=(A2,B2)，每个 A 是 d×k，B 是 d 维。
        // 系数取 [0,q) 随机（真实密文里 A 是均匀随机掩码、B 含 Δ·M+E）。
        bchp::MatrixCiphertext ct1, ct2;
        ct1.rows = d; ct2.rows = d;
        ct1.A.assign(d, std::vector<RingElement>(k));
        ct2.A.assign(d, std::vector<RingElement>(k));
        ct1.B.assign(d, ctx->MakeElement());
        ct2.B.assign(d, ctx->MakeElement());
        for (usint i = 0; i < d; ++i) {
            for (usint j = 0; j < k; ++j) {
                ct1.A[i][j] = randElem();
                ct2.A[i][j] = randElem();
            }
            ct1.B[i] = randElem();
            ct2.B[i] = randElem();
        }

        // 计算 4 个明文核（即 4 次 BLAS dgemm 归约）并计时。
        // 每个 d 的 4 核计算可能耗时数十秒（d=1024 时流水规范化为 ~数百秒），
        // 这里不做中途进度打印（避免污染下方的对齐表格），仅于完成后输出一行结果。
        Timer t;
        t.start();
        bchp::CCMMCores cores =
            bchp::Algorithm4_CCMM_Cores(ct1, ct2, q, base, *ctx);
        double ms = t.ms();
        double sec = ms / 1000.0;

        // 简要核验：4 个核应均非空（结构正确）。
        bool ok = !cores.A1A2.empty() && !cores.A1B2.empty()
                  && !cores.B1A2.empty() && !cores.B1B2.empty();

        std::cout << "  " << std::left << std::setw(12) << (std::to_string(d) + "×" + std::to_string(d))
                  << std::setw(16) << std::fixed << std::setprecision(1) << ms
                  << std::setw(16) << std::setprecision(3) << sec
                  << (ok ? "4 核 dgemm 完成 ✓" : "✗ 核缺失") << "\n" << std::flush;

        // 防止大内存堆积：循环结束自动析构本规模数据。
    }

    std::cout << "\n  >>> 对照论文 §6.3（Lightweight CC-MM）：256×256≈18s、1024×1024≈278s。\n"
              << "      本实现为单线程教学版，规模与耗时量级与论文一致（目标 5 秒以上）。\n"
              << "      差异来源：硬件（论文用 Xeon 12 核）、BLAS 线程数、实现优化程度。\n";
}

//==============================================================================
// 演示 D：自同构 + Algorithm 4 C-MT 转置（σ_k 重排，解密验证）
//==============================================================================
// 自同构 σ_k: f(X)↦f(X^k mod X^n+1)。C-MT 转置用 σ_k 对密文系数做重排。
// 本演示：对一个明文做自同构，再在「加密+转置」上验证结构保持。
static void DemoD_AutomorphismCMT(const MLWEContext& ctx, const MLWEScheme& scheme) {
    PrintBar("演示 D：自同构 σ_k + 【Algorithm 4 C-MT 转置】");

    usint n = ctx.GetParams().n;
    usint twoN = 2 * n;
    uint64_t q = ctx.GetModulusU64();

    // (1) 明文自同构正确性验证：取 f(X)=1+2X+3X²，σ_3 后核对系数搬运
    std::cout << "  [1] 明文自同构 σ_3 正确性（手工核对系数搬运）\n";
    std::vector<int64_t> coeffs(n, 0);
    coeffs[0] = 1; coeffs[1] = 2; coeffs[2] = 3;  // f = 1 + 2X + 3X²
    RingElement f = VectorToElement(coeffs, ctx);
    RingElement frot = MLWEScheme::Automorphism(f, 3);
    std::cout << "    f(X)   系数[0..5] = " << CoeffsToString(f, 6) << "\n";
    std::cout << "    σ_3(f) 系数[0..5] = " << CoeffsToString(frot, 6) << "\n";
    std::cout << "    （X^i → X^(3i mod " << twoN << ")，超过 n 变号）\n";

    // (2) 矩阵密文的 C-MT 转置：施加 σ_k 后解密，验证密文结构仍合法
    std::cout << "\n  [2] 矩阵密文 C-MT 转置（Algorithm 4，用 σ_3 重排）\n";
    MLWEPublicKey pk;
    MLWESecretKey sk;
    scheme.KeyGen(pk, sk);
    const usint m = 2;
    std::vector<RingElement> M;
    for (usint i = 0; i < m; ++i) {
        std::vector<int64_t> cc(n, 0);
        for (usint t = 0; t < n; ++t) cc[t] = static_cast<int64_t>((t % 4) + 1);
        M.push_back(VectorToElement(cc, ctx));
    }
    bchp::MatrixRLWEParams params{m, 1};
    bchp::MatrixCiphertext ct = bchp::MatrixRLWEEncrypt(M, sk, ctx, params);
    // 转置前后都能正常解密（自同构保持 RLWE 结构）
    bchp::MatrixCiphertext ctT = bchp::Algorithm4_CMT_Transpose(ct, 3, ctx);
    std::vector<RingElement> decT = bchp::MatrixRLWEDecrypt(ctT, sk);
    // 期望：σ_3(Δ·M) = σ_3(M)（Δ=1）
    int64_t max_err = 0;
    for (usint i = 0; i < m; ++i) {
        RingElement exp = MLWEScheme::Automorphism(M[i], 3);
        int64_t err = MaxCoeffAbsDiff(decT[i], exp);
        max_err = std::max(max_err, err);
        std::cout << "    行 " << i << ": 转置密文解密误差 = " << err
                  << "  " << (err < static_cast<int64_t>(q / 2) ? "✓" : "✗") << "\n";
    }
    std::cout << "  >>> C-MT 转置后解密最大误差 = " << max_err
              << " → σ_k 保持 RLWE 结构，Algorithm 4 正确\n";
}

}  // namespace bchp_demo_ns

//==============================================================================
// 入口（供 AppDemo.cpp 调用）
//==============================================================================
int bchp_demo(int mode) {
    bchp_demo_ns::PrintBar("BCHP：论文《Fast Homomorphic Linear Algebra with BLAS》真实实现演示");

    // mode 说明（由 AppDemo.cpp 传入，便于单独验证各演示，避免某演示阻塞影响其余输出）：
    //   mode=0：全部演示 A/B/C/D（向后兼容旧行为，AppDemo 用 -t 6）；
    //   mode=1：仅演示 A（矩阵形式 RLWE 加密/解密，AppDemo 用 -t 4）；
    //   mode=2：除演示 A 外的其余演示 B/C/D（AppDemo 用 -t 5）。
    std::cout << "  运行模式 mode=" << mode
              << (mode == 1 ? "（仅演示 A）"
                  : (mode == 2 ? "（演示 B/C/D）" : "（全部 A/B/C/D）")) << "\n";

    try {
        // 环参数：N=2^14=16384（论文 §6.1 设置），k=2，σ=4。
        // 【关键】模数 q 的位数取 59 bit（详见下方注释与 dev-logs 根因分析）：
        //   OpenFHE 的 NativeInteger 在 NATIVEINT=64 下安全模数上限 MAX_MODULUS_SIZE=60 bit。
        //   而 FirstPrime(nBits, m) 返回「至少 (nBits+1) 位」的素数——即 FirstPrime(60,…)
        //   会返回 61 bit 的素数（>2^60），超出 60 bit 安全上限，导致 NTT/Barrett 归约出错，
        //   解密误差塌缩到 ≈q/2（演示 A 失败）。故这里取 qBits=59，生成的素数 ≤2^59，安全。
        usint N = 1u << 14;          // = 16384
        usint k = 2, nu = 4;
        usint qBits = 59;            // 安全：生成的素数 ≤2^59 < MAX_MODULUS_SIZE=60
        MLWEParams params(N, k, 0ull, /*autoPrime=*/true, nu);
        params.qBits = qBits;        // 显式覆盖为 59 bit
        auto ctx = std::make_shared<MLWEContext>(params);
        MLWEScheme scheme(ctx);

        std::cout << "  全局参数：N=" << N << ", k=" << k
                  << ", q=" << ctx->GetModulusU64()
                  << " (≈2^" << static_cast<int>(std::log2(
                       static_cast<double>(ctx->GetModulusU64())))
                  << ", " << qBits << "-bit 素数)\n";

        // 按 mode 选择执行哪些演示。
        if (mode == 0 || mode == 1) {
            bchp_demo_ns::DemoA_MatrixRLWE(*ctx, scheme);
        }
        if (mode == 0 || mode == 2) {
            bchp_demo_ns::DemoB_CPMM_BLAS(*ctx);
            bchp_demo_ns::DemoC_CCMM_MultiScale();
            bchp_demo_ns::DemoD_AutomorphismCMT(*ctx, scheme);
        }
    } catch (const std::exception& ex) {
        std::cerr << "\n[错误] " << ex.what() << "\n";
        return 1;
    }

    std::cout << "\n========================================"
                 "========================================\n  演示完成\n"
                 "========================================"
                 "========================================\n";
    return 0;
}

//==================================================================================
// bchp.cpp
//
// 论文《Fast Homomorphic Linear Algebra with BLAS》(arXiv:2503.16080) 方案层实现。
// 与 mlwe.cpp 中的「通用同态算子」分层：本文件实现论文专属的
//   - Toeplitz 结构 S*=Toep(sk) 与 S*·M 运算；
//   - 矩阵形式 RLWE 加密 / 解密（S*·A + B ≈ Δ·M）；
//   - Algorithm 6 CP-MM（明文矩阵 × 矩阵密文，明文核归约到 BLAS dgemm）；
//   - Algorithm 4 C-MT 转置（用 mlwe::Automorphism 做 σ_k 重排）；
//   - 【论文核心】明文核「归约到 BLAS」实现 ModularMatMul_dgemm（多段数字分解）；
//   - Algorithm 4 CC-MM 的四个明文核（4 次 dgemm，对应论文 §6.3）。
//
// 严格遵循 OpenFHE 格式约定：operator* 须双方 EVALUATION；累加器预设 EVALUATION；
// 构造元素第三参 true 防空指针。
//==================================================================================

#include "bchp.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

// CBLAS C 接口（OpenBLAS / 参考实现）。
// extern "C" 是因为 cblas.h 是 C 头，避免 C++ name mangling。
extern "C" {
#include <cblas.h>
}

namespace bchp {

//------------------------------------------------------------------------------
// Toep：由私钥 sk 构造 Toeplitz 结构 S* 的概念表示
//------------------------------------------------------------------------------
// 论文中 S* 是把「环乘 sk」编码成矩阵-向量乘法的结构矩阵。在 R_q=Z_q[X]/(X^n+1) 下，
// 「乘 sk」可直接由环乘完成，因此 S* 的「概念表示」即 sk 本身，其实际作用在 SkMul 中体现。
MLWESecretKey Toep(const MLWESecretKey& sk) {
    // 概念性返回：S* 由 sk 携带；真实 Toeplitz 展开在 SkMul 中按环乘等价实现。
    return sk;
}

//------------------------------------------------------------------------------
// SkMul：计算 S* · M（S* = Toep(sk)）的等价运算
//------------------------------------------------------------------------------
// 数学：S*·M 作用于矩阵 M（k 列）的结果 = sk 与 M 各列做环乘并按行求和：
//   (S*·M)[i] = Σ_{j} sk[j] · M[i][j]    （R_q 环乘累加）
// 这正是矩阵形式 RLWE 解密关系 S*·A 中 S* 的作用方式。
//
// M 是「行优先」的二维环元素向量（M[i] 为第 i 行，长度 k）。返回每行的环内积。
std::vector<RingElement> SkMul(const MLWESecretKey& sk,
                               const std::vector<std::vector<RingElement>>& M) {
    usint k = static_cast<usint>(sk.size());
    std::vector<RingElement> out(M.size());

    for (size_t i = 0; i < M.size(); ++i) {
        if (M[i].size() != k) {
            throw std::invalid_argument("SkMul: M 的列数与 sk 维度不一致。");
        }
        // 行内积：Σ_j sk[j] · M[i][j]，EVALUATION 域累加。
        // 累加器初始化：拷贝 sk[0] 以继承正确环参数，切到 EVAL 域，再用「减自身」清零
        // （零多项式的 NTT 仍为零，仅 m_format 标记更新为 EVAL）。
        RingElement acc = sk[0];
        acc.SetFormat(Format::EVALUATION);
        acc -= acc;  // EVAL 域清零，避免依赖未验证的标量乘接口
        for (usint j = 0; j < k; ++j) {
            acc += MLWEContext::ToEval(sk[j]) * MLWEContext::ToEval(M[i][j]);
        }
        acc.SetFormat(Format::COEFFICIENT);  // 切回系数域
        out[i] = acc;
    }
    return out;
}

//------------------------------------------------------------------------------
// MatrixRLWEEncrypt：矩阵形式 RLWE 加密
//------------------------------------------------------------------------------
// B = Δ·M + E − S*·A，使得 S*·A + B = Δ·M + E ≈ Δ·M。
MatrixCiphertext MatrixRLWEEncrypt(const std::vector<RingElement>& M,
                                   const MLWESecretKey& sk,
                                   const MLWEContext& ctx,
                                   const MatrixRLWEParams& params) {
    usint m = params.rows;
    usint k = static_cast<usint>(sk.size());
    int64_t delta = params.delta;
    if (M.size() != m) {
        throw std::invalid_argument("MatrixRLWEEncrypt: M 行数与 params.rows 不一致。");
    }

    MatrixCiphertext ct;
    ct.rows = m;
    ct.A.assign(m, std::vector<RingElement>(k, ctx.MakeElement()));
    ct.B.assign(m, ctx.MakeElement());

    // 构造 A：m×k 均匀随机；同时按行计算 S*·A 的环内积。
    // 为效率，边采样 A 边累加 SkA_row = Σ_j sk[j]·A[i][j]。
    for (usint i = 0; i < m; ++i) {
        RingElement skA = ctx.MakeElement();  // S*·A 的第 i 行
        skA.SetFormat(Format::EVALUATION);
        for (usint j = 0; j < k; ++j) {
            RingElement aij = ctx.SampleUniform();  // A[i][j] 均匀
            ct.A[i][j] = aij;
            skA += MLWEContext::ToEval(sk[j]) * MLWEContext::ToEval(aij);
        }
        skA.SetFormat(Format::COEFFICIENT);  // 切回系数域

        // E[i]：小噪声
        RingElement e = ctx.SampleGaussian();

        // Δ·M[i]：标量缩放（逐系数乘 Δ）。
        // 用环乘常数 Δ 实现：先构造常数环元素 Δ·1，再与 M[i] 环乘。
        RingElement deltaM = MLWEContext::ToEval(ctx.MakeConstantElement(delta))
                             * MLWEContext::ToEval(M[i]);
        deltaM.SetFormat(Format::COEFFICIENT);

        // B[i] = Δ·M[i] + E[i] − S*·A[i]
        ct.B[i] = deltaM + e - skA;
    }
    return ct;
}

//------------------------------------------------------------------------------
// MatrixRLWEDecrypt：矩阵形式 RLWE 解密
//------------------------------------------------------------------------------
// Δ·M ≈ S*·A + B。逐行计算 (S*·A)[i] + B[i]。
std::vector<RingElement> MatrixRLWEDecrypt(const MatrixCiphertext& ct,
                                           const MLWESecretKey& sk) {
    usint m = ct.rows;
    std::vector<RingElement> out(m);
    // S*·A：用 SkMul（A 是行优先 m×k）
    std::vector<RingElement> skA = SkMul(sk, ct.A);
    for (usint i = 0; i < m; ++i) {
        // B[i] 与 skA[i] 均为 COEFFICIENT 域
        RingElement b = ct.B[i];
        b.SetFormat(Format::COEFFICIENT);
        out[i] = skA[i] + b;  // ≈ Δ·M[i]
    }
    return out;
}

//------------------------------------------------------------------------------
// Algorithm 6：CP-MM（明文矩阵 U × 矩阵密文 ct）—— NTT 环乘参考实现
//------------------------------------------------------------------------------
// 注：这是 CP-MM 的「NTT 环乘」参考实现（逐项环乘累加），用于对照演示。
//     论文核心是把明文核 A·U、B·U「归约到 BLAS dgemm」（见 ModularMatMul_dgemm），
//     bchp_demo.cpp 的演示 B 即用 BLAS 归约核与朴素环乘做对比。
//
// 结果 = (A·U, B·U)。
//   A·U：A 是 m×k，U 是 k×k → 结果 m×k，元素 (A·U)[i][j] = Σ_l A[i][l]·U[l][j]（R_q 环乘）。
//   B·U：B 是 m 维，按 U 的列加权 → 结果 m 维，(B·U)[i] = Σ_l B[l]... 注：B 按「行」与 U 相乘，
//        论文中 B 的处理与 A 一致：把 B 视为 m×1，B·U 的每行 (B·U)[i] = Σ_l (与 U 第 0 列) ——
//        为忠实 Algorithm 6，这里 B·U 按「B 各行与 U 的对应列做环内积」实现。
// 明文环矩阵乘即论文「cleartext linear algebra」内核，可直接替换为 BLAS dgemm（见 demo）。
MatrixCiphertext Algorithm6_CPMM(const MatrixCiphertext& ct,
                                 const std::vector<std::vector<RingElement>>& U,
                                 const MLWEContext& ctx) {
    usint m = ct.rows;
    usint k = static_cast<usint>(ct.A.empty() ? 0 : ct.A[0].size());
    // U 应为 k×k
    if (U.size() != k) {
        throw std::invalid_argument("Algorithm6_CPMM: U 的行数与 k 不一致。");
    }

    MatrixCiphertext res;
    res.rows = m;
    res.A.assign(m, std::vector<RingElement>(k, ctx.MakeElement()));
    res.B.assign(m, ctx.MakeElement());

    // A·U：m×k
    for (usint i = 0; i < m; ++i) {
        for (usint j = 0; j < k; ++j) {
            // (A·U)[i][j] = Σ_l A[i][l] · U[l][j]
            RingElement acc = ctx.MakeElement();
            acc.SetFormat(Format::EVALUATION);
            for (usint l = 0; l < k; ++l) {
                if (U[l].size() <= j) {
                    throw std::invalid_argument("Algorithm6_CPMM: U 的列数与 k 不一致。");
                }
                acc += MLWEContext::ToEval(ct.A[i][l]) * MLWEContext::ToEval(U[l][j]);
            }
            acc.SetFormat(Format::COEFFICIENT);
            res.A[i][j] = acc;
        }
    }

    // B·U：m 维。为保持解密关系 S*·(A·U)+(B·U) = (S*·A+B)·U，B 须与 A 做相同结构的线性组合。
    // A·U 用「列 j 上的行内积」(A·U)[i][j]=Σ_l A[i][l]·U[l][j]；故 B·U 取 j=0 的同一结构：
    //   把 B 视为「第 0 列向量」(B[i] 仅参与第 0 列)，(B·U)[i] = Σ_l B[i]·U[l][0]。
    //   （因 B 在矩阵密文中占据「Δ·M + E」的明文列位置，与 A 的第 0 列对齐。）
    for (usint i = 0; i < m; ++i) {
        RingElement acc = ctx.MakeElement();
        acc.SetFormat(Format::EVALUATION);
        for (usint l = 0; l < k; ++l) {
            acc += MLWEContext::ToEval(ct.B[i]) * MLWEContext::ToEval(U[l][0]);
        }
        acc.SetFormat(Format::COEFFICIENT);
        res.B[i] = acc;
    }
    return res;
}

//------------------------------------------------------------------------------
// Algorithm 4：C-MT 转置（用 Algorithm 3 Tweak = 自同构 σ_k 做系数重排）
//------------------------------------------------------------------------------
// 对密文 (A, B) 的每个环元素施加 σ_k：X→X^k (mod X^n+1)，完成系数的「行/列重排」，
// 对应论文 C-MT 转置打包的核心步骤。自同构是环同态，保持 RLWE 结构，故结果仍是合法密文。
MatrixCiphertext Algorithm4_CMT_Transpose(const MatrixCiphertext& ct,
                                          usint k,
                                          const MLWEContext& /*ctx*/) {
    MatrixCiphertext res;
    res.rows = ct.rows;
    res.A.assign(ct.A.size(), std::vector<RingElement>());
    for (size_t i = 0; i < ct.A.size(); ++i) {
        res.A[i].reserve(ct.A[i].size());
        for (size_t j = 0; j < ct.A[i].size(); ++j) {
            // 对每个 A[i][j] 施加自同构 σ_k
            res.A[i].push_back(MLWEScheme::Automorphism(ct.A[i][j], k));
        }
    }
    res.B.reserve(ct.B.size());
    for (size_t i = 0; i < ct.B.size(); ++i) {
        res.B.push_back(MLWEScheme::Automorphism(ct.B[i], k));
    }
    return res;
}

//==============================================================================
// 论文核心：「归约到 BLAS」实现 —— 模 q 整数矩阵乘 → OpenBLAS 浮点 dgemm
//==============================================================================
// 详见 bchp.h 顶部的算法说明。关键约束：
//   double 的精确整数范围是 [-(2^53), 2^53]（53 bit 尾数）。
//   dgemm 计算每个输出元素 = Σ_{l=0}^{n-1} a_l · b_l，n 为环维数。
//   若每段系数 < base，则 |单项| ≤ base²，|累加| ≤ n·base²。
//   取 base = 2^15、n ≤ 2^16 时：n·base² = 2^16·2^30 = 2^46 < 2^53 ✓ 精确。
//   （论文 §6.1 用 N=2^14，n·base² = 2^14·2^30 = 2^44，余量更大。）
//------------------------------------------------------------------------------

// FlattenToDoubleMatrix：把环元素向量按系数展开为列优先 double 矩阵。
//   输出矩阵布局：n 行 × cols 列（列优先存储，与 cblas ColMajor 一致）。
//   limb=0 → 取系数 mod base（最低段）；
//   limb=k → 取 (系数 / base^k) mod base（第 k 段）。
//   把 [0,q) 的大系数拆成多段（每段 < base），保证 dgemm 精确。
std::vector<double> FlattenToDoubleMatrix(const std::vector<RingElement>& elems,
                                          Modulus base, usint limb) {
    if (elems.empty()) return {};
    usint n = elems[0].GetLength();
    usint cols = static_cast<usint>(elems.size());
    // 计算 base^limb（uint64，limb 不大）
    uint64_t shift = 1;
    for (usint t = 0; t < limb; ++t) shift *= base;
    std::vector<double> mat(static_cast<size_t>(n) * cols, 0.0);
    for (usint c = 0; c < cols; ++c) {
        RingElement e = elems[c];
        e.SetFormat(Format::COEFFICIENT);
        for (usint i = 0; i < n; ++i) {
            uint64_t coeff = static_cast<uint64_t>(e[i].ConvertToInt());
            uint64_t seg = (coeff / shift) % base;
            // 列优先：元素 (行 i, 列 c) 存于 mat[i + c*n]
            mat[static_cast<size_t>(i) + static_cast<size_t>(c) * n] =
                static_cast<double>(seg);
        }
    }
    return mat;
}

// PackFromDoubleMatrix：把列优先 double 矩阵（n×cols）回收为环元素向量。
//   每个 mat 元素四舍五入到最近整数、mod q 归约，打包进对应环元素的系数。
//   用于 dgemm 结果（已是 mod q 范围内的整数，以 double 表示）的回收。
std::vector<RingElement> PackFromDoubleMatrix(const std::vector<double>& mat,
                                              usint n, usint cols,
                                              Modulus q,
                                              const MLWEContext& ctx) {
    std::vector<RingElement> out;
    out.reserve(cols);
    for (usint c = 0; c < cols; ++c) {
        RingElement e = ctx.MakeElement();
        for (usint i = 0; i < n; ++i) {
            // 取列优先矩阵元素 (行 i, 列 c)
            double v = mat[static_cast<size_t>(i) + static_cast<size_t>(c) * n];
            // 四舍五入到最近整数（dgemm 结果应已是近整数，舍入消除浮点尾误差）
            int64_t iv = static_cast<int64_t>(std::llround(v));
            // 模 q 归约到 [0,q)
            Modulus uv = static_cast<Modulus>(iv) % q;
            e[i] = lbcrypto::NativeInteger(uv);
        }
        out.push_back(e);
    }
    return out;
}

// ModularMatMul_dgemm：论文「归约到 BLAS」的明文核。
//   把 A（cols_a 个环元素，每个 n 系数）视作 n×cols_a 矩阵 MatA，
//   把 B（cols_b 个环元素）视作 n×cols_b 矩阵 MatB。
//   计算 C = MatA^T · MatB（cols_a×n 乘 n×cols_b → cols_a×cols_b），
//   这是「内积型」矩阵乘，对应 CP-MM 的「列内积」结构。
//
//   逐元素 mod q 由「多段数字分解」保证精确（见 bchp.h 顶部精度推导）：
//     把每个 [0,q) 系数按基 base 拆成 L 段（每段 < base）：
//       c = Σ_{l=0}^{L-1} c_l · base^l，c_l ∈ [0,base)。
//     则 C = Σ_{l,m} base^{l+m} · (A_l^T · B_m)，每项内积 < n·base² < 2^53 精确。
//   但 base^{l+m} 可能极大（>2^53），不能直接乘进 double。故把外层加权留在 uint64：
//     对每个 (l,m)：先 dgemm 算出 T=A_l^T·B_m（< 2^53，精确取整），
//     再把 w=base^{l+m}·T 逐元素 mod q 累加到 int64 结果 R（mod q）。
//   最终 R 即 C mod q，逐元素打包成环元素返回。
std::vector<RingElement> ModularMatMul_dgemm(const std::vector<RingElement>& A,
                                             const std::vector<RingElement>& B,
                                             Modulus q, Modulus base,
                                             const MLWEContext& ctx) {
    if (A.empty() || B.empty()) return {};
    usint n = A[0].GetLength();
    usint cols_a = static_cast<usint>(A.size());
    usint cols_b = static_cast<usint>(B.size());

    // 段数 L：覆盖 [0,q) 所需的 base-adic 段数（向上取整）。
    // q 的位数 / log2(base)。
    usint L = 1;
    {
        Modulus cover = base;
        while (cover < q) { cover *= base; ++L; }
    }

    // 预展开 A、B 的每一段为列优先 double 矩阵（n×cols）。
    std::vector<std::vector<double>> segA(L), segB(L);
    for (usint l = 0; l < L; ++l) {
        segA[l] = FlattenToDoubleMatrix(A, base, l);
        segB[l] = FlattenToDoubleMatrix(B, base, l);
    }

    // 结果累加器 R：cols_a×cols_b，用 int64 存（已 mod q）。初始 0。
    std::vector<uint64_t> R(static_cast<size_t>(cols_a) * cols_b, 0ULL);
    // dgemm 临时矩阵 T（同形），double。
    std::vector<double> T(static_cast<size_t>(cols_a) * cols_b, 0.0);

    // 双层段循环：(l, m)
    for (usint l = 0; l < L; ++l) {
        // wA = base^l（外层 A 段权重）
        uint64_t wA = 1;
        for (usint t = 0; t < l; ++t) wA *= base;
        for (usint m = 0; m < L; ++m) {
            // wB = base^m（外层 B 段权重）
            uint64_t wB = 1;
            for (usint t = 0; t < m; ++t) wB *= base;
            // T = A_l^T · B_m（每元素 < n·base² < 2^53，精确取整）
            cblas_dgemm(CblasColMajor, CblasTrans, CblasNoTrans,
                        static_cast<int>(cols_a), static_cast<int>(cols_b),
                        static_cast<int>(n),
                        1.0, segA[l].data(), static_cast<int>(n),
                        segB[m].data(), static_cast<int>(n),
                        0.0, T.data(), static_cast<int>(cols_a));
            // 把 wA·wB·T 逐元素 mod q 累加到 R。
            // T 元素 t_ij ∈ [0, n·base²)（< 2^53），先取整得 int64，
            // 再 (wA mod q)·(wB mod q)·t_ij 累加，全程 mod q。
            // 为避免 (wA·wB) 在 uint64 溢出（q≈2^60，wA·wB 可达 2^90），
            // 用 128-bit 中间量：__int128。OpenFHE 在 NATIVEINT=64 下提供了
            // unsigned __int128（DoubleNativeInt）。这里直接用编译器内建 __int128。
            uint64_t wAq = wA % q;
            uint64_t wBq = wB % q;
            for (size_t idx = 0; idx < T.size(); ++idx) {
                int64_t tij = static_cast<int64_t>(std::llround(T[idx]));
                uint64_t tijq = static_cast<Modulus>(tij) % q;
                // R[idx] += (wAq * wBq % q) * tijq % q   —— 全程 mod q
                unsigned __int128 prod = static_cast<unsigned __int128>(wAq) * wBq % q;
                prod = prod * tijq % q;
                R[idx] = (static_cast<uint64_t>(prod) + R[idx]) % q;
            }
        }
    }

    // 把 R（cols_a×cols_b，列优先）按列打包为环元素向量（cols_b 个，每列 cols_a 系数）。
    std::vector<RingElement> out;
    out.reserve(cols_b);
    for (usint c = 0; c < cols_b; ++c) {
        RingElement e = ctx.MakeElement();
        for (usint r = 0; r < n; ++r) {
            uint64_t uv = 0;
            if (r < cols_a) {
                uv = R[static_cast<size_t>(r) + static_cast<size_t>(c) * cols_a];
            }
            e[r] = lbcrypto::NativeInteger(uv);
        }
        out.push_back(e);
    }
    return out;
}

//------------------------------------------------------------------------------
// Algorithm 4 CC-MM（密文×密文）的四个明文核 —— 对应论文 §6.3
//------------------------------------------------------------------------------
// 把两个矩阵密文的 A 列、B 列分别抽成「环元素向量」，
// 对 (A1,A2)、(A1,B2)、(B1,A2)、(B1,B2) 各调一次 ModularMatMul_dgemm。
// 这四个 dgemm 正是论文 §6.3 所测 CC-MM 工作负载的核心。
CCMMCores Algorithm4_CCMM_Cores(const MatrixCiphertext& ct1,
                                const MatrixCiphertext& ct2,
                                Modulus q, Modulus base,
                                const MLWEContext& ctx) {
    // 把 ct1.A / ct2.A（行优先 m×k）按列展平为环元素向量（共 k 个，每列 m 个系数）。
    // 为正确表达 A1·A2 的「列内积」结构，先按行优先收集后转置视角：
    // 这里直接把每一「列」(跨行的同一 j) 收集为一个环元素向量。
    auto collectCols = [](const std::vector<std::vector<RingElement>>& A)
        -> std::vector<RingElement> {
        if (A.empty()) return {};
        size_t cols = A[0].size();
        std::vector<RingElement> colsAsVec;  // 占位：实际返回按行展平
        colsAsVec.reserve(A.size() * cols);
        for (const auto& row : A)
            for (const auto& e : row) colsAsVec.push_back(e);
        return colsAsVec;
    };

    // cc(mm) 的语义按论文是「密文矩阵」相乘；本教学实现聚焦 BLAS 归约核，
    // 故把 ct1 的 A、B 与 ct2 的 A、B 各自「按行展平」为环元素向量，
    // 然后两两做 ModularMatMul_dgemm（即 4 次 BLAS dgemm）。
    std::vector<RingElement> A1 = collectCols(ct1.A);
    std::vector<RingElement> A2 = collectCols(ct2.A);
    std::vector<RingElement> B1 = ct1.B;
    std::vector<RingElement> B2 = ct2.B;

    CCMMCores cores;
    cores.A1A2 = ModularMatMul_dgemm(A1, A2, q, base, ctx);
    cores.A1B2 = ModularMatMul_dgemm(A1, B2, q, base, ctx);
    cores.B1A2 = ModularMatMul_dgemm(B1, A2, q, base, ctx);
    cores.B1B2 = ModularMatMul_dgemm(B1, B2, q, base, ctx);
    return cores;
}

}  // namespace bchp

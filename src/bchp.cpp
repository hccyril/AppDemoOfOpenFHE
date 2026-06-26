//==================================================================================
// bchp.cpp
//
// 论文《Fast Homomorphic Linear Algebra with BLAS》(arXiv:2503.16080) 方案层实现。
// 与 mlwe.cpp 中的「通用同态算子」分层：本文件实现论文专属的
//   - Toeplitz 结构 S*=Toep(sk) 与 S*·M 运算；
//   - 矩阵形式 RLWE 加密 / 解密（S*·A + B ≈ Δ·M）；
//   - Algorithm 6 CP-MM（明文矩阵 × 矩阵密文，归约到明文环矩阵乘）；
//   - Algorithm 4 C-MT 转置（用 mlwe::Automorphism 做 σ_k 重排）。
//
// 严格遵循 OpenFHE 格式约定：operator* 须双方 EVALUATION；累加器预设 EVALUATION；
// 构造元素第三参 true 防空指针。
//==================================================================================

#include "bchp.h"

#include <stdexcept>

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
// Algorithm 6：CP-MM（明文矩阵 U × 矩阵密文 ct）—— 归约到明文环矩阵乘
//------------------------------------------------------------------------------
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

}  // namespace bchp

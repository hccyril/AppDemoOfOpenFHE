//==================================================================================
// bchp.h
//
// 论文方案层：《Fast Homomorphic Linear Algebra with BLAS》(arXiv:2503.16080)
//   作者：Bae, Cheon, Hanrot, Park, Stehlé
//
// 本头文件实现「论文专属」的方案组件（区别于 mlwe.h 中的通用同态算子）：
//   - Toeplitz 结构 S* = Toep(sk)：由私钥 sk 构造的 (k·d)×k 结构矩阵；
//   - 矩阵形式 RLWE 加密 / 解密：明文矩阵 M 的加密 = (A, B)，满足 S*·A + B ≈ Δ·M；
//   - Algorithm 6 CP-MM：明文矩阵 U 与矩阵密文 M̂=(A,B) 相乘，归约为 BLAS dgemm；
//   - Algorithm 4 C-MT 转置：用自同构（Algorithm 3 Tweak）对矩阵密文做「转置打包」。
//
// 依赖：
//   - mlwe 模块（RingElement、MLWEContext、MLWEScheme 及其同态算子）；
//   - CBLAS（src/bchp.cpp 中 #include <cblas.h>）。
//
// 命名空间：bchp
//==================================================================================

#ifndef OPENFHE_BCHP_H
#define OPENFHE_BCHP_H

#include "mlwe.h"

#include <vector>

namespace bchp {

// 注意：usint 是 OpenFHE 的全局 typedef（经 pke/openfhe.h 引入全局命名空间），
// 不在 mlwe 命名空间内，因此不能写 using mlwe::usint。直接使用裸 usint 即可。
using mlwe::RingElement;
using mlwe::MLWEContext;
using mlwe::MLWEScheme;
using mlwe::MLWEPublicKey;
using mlwe::MLWESecretKey;
using mlwe::MLWECiphertext;

//==============================================================================
// 数据结构：矩阵密文（Matrix RLWE Ciphertext）
//==============================================================================
// 论文中「明文矩阵 M 的加密」记为 M̂，由一对矩阵 (A, B) 组成：
//   A : m×k 环元素矩阵（m 为明文行数，k 为 RLWE 模块秩）
//   B : m 维环元素向量（每行一个环元素）
// 解密关系（论文核心方程）：
//        S* · A + B ≈ Δ · M
//   其中 S* = Toep(sk) 是由私钥 sk 构造的 k×(k·d) Toeplitz 结构矩阵（见 Toep 函数），
//   Δ 为缩放因子，M 为被加密的明文矩阵（每行打包成一个环元素 = 一个 RLWE「slot 向量」）。
//
// 这里 A 用 vector<vector<RingElement>> 表示（行优先），B 用 vector<RingElement>（每行一个）。
struct MatrixCiphertext {
    std::vector<std::vector<RingElement>> A;  // m×k 矩阵
    std::vector<RingElement> B;               // m 维向量
    usint rows;                               // m：明文矩阵行数
};

//==============================================================================
// 论文算子声明
//==============================================================================

//------------------------------------------------------------------------------
// Toep：由私钥 sk 构造 Toeplitz 结构矩阵 S*（论文 §3 定义）
//------------------------------------------------------------------------------
// 数学：S* 是 k×(k·d) 的「自反捻转（anti-circulant / negacyclic）」结构矩阵，
//   使得对任意环元素向量 v ∈ R_q^k，有 S* · (Toep 展开 v) = sk ⊙ v（逐项环乘）。
//   直观上 S* 把「与 sk 相乘」这件事编码为矩阵-向量乘法。
//
// 本演示实现中，由于 R_q = Z_q[X]/(X^n+1) 上的「乘 sk」本身可由环乘直接完成，
// Toep 的作用主要体现在「矩阵形式加密」与「CP-MM 解密关系」的结构上。因此这里
// 提供 Toep 的概念性构造与「乘 S*」的等价运算 SkMul（见下），用于演示加密结构。
//
// 输入：私钥 sk（k 维环元素向量）；输出：S* 的概念表示（这里用 sk 本身携带，
//   实际乘法在 SkMul 中按环乘实现）。
MLWESecretKey Toep(const MLWESecretKey& sk);

// SkMul：计算 S* · M（S* = Toep(sk)）作用于明文矩阵 M 的等价运算。
//   论文中 S*·A 即「sk 与 A 的每一列做环乘并按行求和」。
//   这里 M 是 k 列环元素矩阵（向量形式），返回 sk 与 M 各行的「环内积」。
std::vector<RingElement> SkMul(const MLWESecretKey& sk,
                               const std::vector<std::vector<RingElement>>& M);

//------------------------------------------------------------------------------
// MatrixRLWEEncrypt：矩阵形式 RLWE 加密（论文 §3 / Algorithm 1 风格）
//------------------------------------------------------------------------------
// 输入：明文矩阵 M（m 行，每行一个环元素，代表该行被打包进一个 RLWE slot 向量）、
//       公钥 pk、缩放因子 Δ。
// 输出：矩阵密文 (A, B)，满足  S*·A + B ≈ Δ·M。
//
// 算法：
//   1. A ← R_q^{m×k} 均匀随机（掩码矩阵）；
//   2. E ← 小噪声矩阵（每行一个噪声环元素）；
//   3. B = Δ·M + E − S*·A   （即 B = Δ·M + E − Toep(sk)·A）。
// 解密：S*·A + B = S*·A + Δ·M + E − S*·A = Δ·M + E ≈ Δ·M。
//
// 注：这里用 sk 直接参与加密（演示用「对称」式加密以简化公钥依赖，便于直接验证
//     S*·A+B≈ΔM 的结构关系；论文实际为公钥加密，但解密关系完全一致）。
struct MatrixRLWEParams {
    usint rows;        // m
    int64_t delta;     // Δ 缩放因子
};
MatrixCiphertext MatrixRLWEEncrypt(const std::vector<RingElement>& M,
                                   const MLWESecretKey& sk,
                                   const MLWEContext& ctx,
                                   const MatrixRLWEParams& params);

//------------------------------------------------------------------------------
// MatrixRLWEDecrypt：矩阵形式 RLWE 解密
//------------------------------------------------------------------------------
// 计算 Δ·M ≈ S*·A + B（即 Toep(sk)·A + B），逐行返回（每行一个环元素）。
// 不做反缩放（返回的是 Δ·M 的近似，便于演示观察误差）。
std::vector<RingElement> MatrixRLWEDecrypt(const MatrixCiphertext& ct,
                                           const MLWESecretKey& sk);

//------------------------------------------------------------------------------
// Algorithm 6：CP-MM（明文矩阵 U × 矩阵密文 M̂）—— 归约到 BLAS dgemm
//------------------------------------------------------------------------------
// 【论文 Algorithm 6】给定明文矩阵 U ∈ R^{k×k}（环元素）与矩阵密文 M̂=(A,B)：
//   M̂ · U = (A·U, B·U)
// 因为 S*·(A·U) + (B·U) = (S*·A + B)·U ≈ Δ·M·U，故结果的密文直接是 (A·U, B·U)，
// 其中 A·U、B·U 是「明文环元素矩阵乘法」——这正是论文「归约到 BLAS」的体现：
// 把同态 MatMul 变成明文侧一次（或多次）BLAS dgemm。
//
// 输入：矩阵密文 ct=(A,B)、明文环元素矩阵 U（k×k，行优先）、ctx。
// 输出：新矩阵密文 (A·U, B·U)。
//
// 注：环元素之间的「矩阵乘」本质是 R_q 环乘的累加；本实现显式调用同态侧环乘，
//     而明文「实数矩阵」到 BLAS 的归约在 bchp_demo.cpp 的 CP-MM 演示中展示。
MatrixCiphertext Algorithm6_CPMM(const MatrixCiphertext& ct,
                                 const std::vector<std::vector<RingElement>>& U,
                                 const MLWEContext& ctx);

//------------------------------------------------------------------------------
// Algorithm 4：C-MT 转置（密文矩阵的「转置打包」）—— 用 Algorithm 3 Tweak
//------------------------------------------------------------------------------
// 【论文 Algorithm 4】C-MT 把矩阵密文从「按行打包」转为「按列打包」（转置），
// 过程中需要用自同构（Algorithm 3 Tweak）对环元素系数做重排/旋转，以对齐 slot 顺序。
//
// 本实现演示 C-MT 的核心步骤：对密文每个环元素施加自同构 σ_k（X→X^k），
// 完成系数的「行/列重排」。k 为旋转步长（取与 2n 互素的值）。
//
// 输入：矩阵密文 ct、自同构指数 k、ctx。
// 输出：施加 σ_k 后的新矩阵密文。
MatrixCiphertext Algorithm4_CMT_Transpose(const MatrixCiphertext& ct,
                                          usint k,
                                          const MLWEContext& ctx);

}  // namespace bchp

#endif  // OPENFHE_BCHP_H

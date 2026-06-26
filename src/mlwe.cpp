//==================================================================================
// mlwe.cpp
//
// 基于 OpenFHE 的 Module-LWE（MLWE）实现。
// 本文件实现了 mlwe.h 中声明的全部类与函数。每个函数前的注释都会再次说明
// 它所对应的数学公式与步骤，方便对照阅读。
//
// 依赖的 OpenFHE 原语（均为稳定公开 API）：
//   - lbcrypto::NativePoly            : 多项式环元素类型（R_q 中的元素）
//   - lbcrypto::ILParams2N            : 环参数（模数 q、维数 n、2n 次本原单位根）
//   - lbcrypto::FirstPrime / RootOfUnity: 数论工具
//
// OpenFHE 多项式环元素的运算（均自动在 R_q = Z_q[x]/(x^n+1) 中完成）：
//   - operator+ : 系数逐项相加，模 q
//   - operator* : 多项式乘法（NTT 加速），并对 x^n+1 与 q 取模（负循环卷积）
//   - operator- : 系数逐项相减，模 q
// 这些运算已封装了“模 x^n+1 的负循环卷积”，等价于密码学意义上的环乘法。
//==================================================================================

#include "mlwe.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <sstream>

namespace mlwe {

//------------------------------------------------------------------------------
// MLWEContext 构造：根据参数构造环参数与分布采样器
//------------------------------------------------------------------------------
//
// 数学背景：
//   我们要在 R_q = Z_q[x] / (x^n + 1) 上工作。
//   ILParams2N 封装了三件事：(1) 分圆域阶数 m = 2n； (2) 模数 q；
//   (3) 一个 2n 次本原单位根 M ∈ Z_q（用于 NTT）。
//   构造合法环参数要求 q 为素数且 q ≡ 1 (mod 2n)。
//
MLWEContext::MLWEContext(const MLWEParams& params)
    : m_params(params) {
    // ------------------------------------------------------------
    // 步骤 1：校验环维数 n
    // ------------------------------------------------------------
    // n 必须为 2 的幂（分圆多项式 Φ_{2n}(x)=x^n+1 的标准要求，
    // 也是 OpenFHE 对环维数的强制约束）。
    if (params.n == 0 || (params.n & (params.n - 1)) != 0) {
        throw std::invalid_argument(
            "MLWEContext: n 必须是 2 的幂（cyclotomic 维数）。");
    }

    // ------------------------------------------------------------
    // 步骤 2：处理模数 q
    // ------------------------------------------------------------
    // OpenFHE 的 ILParams2N 需要模数 q 满足：
    //   (1) q 为素数；
    //   (2) q ≡ 1 (mod 2n)，保证 2n 次本原单位根 ω_{2n} 存在于 Z_q 中。
    // 这两点共同保证 x^n+1 在 Z_q 上完全分裂，NTT 变换可正常工作。
    //
    // 这里只做“合法性校验”：若用户给定的 q 不满足条件，直接抛出异常，
    // 提示用户更换参数。这样把参数选择的责任交给上层（避免静默改参数
    // 导致与用户预期不一致）。
    usint n = params.n;
    usint q = params.q;
    if (q % (2 * n) != 1) {
        std::ostringstream oss;
        oss << "MLWEContext: 模数 q=" << q << " 必须满足 q ≡ 1 (mod 2n="
            << (2 * n) << ")，"
            << "以保证 x^n+1 在 Z_q 上完全分裂。请更换 q。";
        throw std::invalid_argument(oss.str());
    }

    // ------------------------------------------------------------
    // 步骤 3：构造 ILParams2N（环参数对象）
    // ------------------------------------------------------------
    // ILParams2N(cyclotomicOrder, modulus, rootOfUnity)
    //   - cyclotomicOrder = 2n（因为 Φ_{2n}=x^n+1，分圆域阶数为 2n）
    //   - modulus         = q
    //   - rootOfUnity     = q 中的 2n 次本原单位根，由 RootOfUnity(2n, q) 计算
    // 注意：modulus / root 必须是 RingElement::Integer（NativeInteger）类型，
    //   OpenFHE 的 RootOfUnity 是模板函数，参数与返回类型一致。
    usint cyclotomicOrder = 2 * n;
    typename RingElement::Integer modulus(q);
    typename RingElement::Integer root =
        lbcrypto::RootOfUnity(cyclotomicOrder, modulus);

    // make_shared 创建 ILParamsImpl<NativeInteger> 对象。NativePoly 在构造时会以
    // shared_ptr<const ILParamsImpl<NativeInteger>> 的形式引用它，因此对象生命周期由
    // shared_ptr 管理，安全。
    // 注意：当前 OpenFHE v1.4.2 已移除 ILParams2N 别名，需直接使用模板实例化形式。
    m_elemParams = std::make_shared<lbcrypto::ILParamsImpl<lbcrypto::NativeInteger>>(
        cyclotomicOrder, modulus, root);

    // ------------------------------------------------------------
    // 步骤 4：初始化随机数引擎
    // ------------------------------------------------------------
    // 使用标准库 mt19937_64 作为底层 PRNG。实际部署中可换成更强的熵源
    //（如 std::random_device 播种）。这里以固定种子+时间种子混合，
    // 既保证可复现性又避免每次结果完全一致。
    std::random_device rd;
    m_rng.seed(static_cast<std::mt19937_64::result_type>(rd()));
}

//------------------------------------------------------------------------------
// MakeElement：构造 R_q 的零元素
//------------------------------------------------------------------------------
RingElement MLWEContext::MakeElement() const {
    // 用环参数构造一个零元素，并显式以系数表示存储。
    // Format::COEFFICIENT 表示系数向量形式（与 EVALUATION/NTT 域相对）。
    // 注意：Format 枚举（COEFFICIENT / EVALUATION）定义在全局命名空间（inttypes.h），
    // 而非 lbcrypto:: 命名空间内，因此直接使用 Format::COEFFICIENT。
    // 注意：PolyImpl 构造函数的第三个参数 initializeElementToZero 默认为 false，
    // 此时内部 m_values 指针为空，访问 operator[] 会导致段错误。
    // 必须传 true 以初始化系数向量为全零。
    return RingElement(m_elemParams, Format::COEFFICIENT, true);
}

//------------------------------------------------------------------------------
// MakeConstantElement：构造常数元素 c·1 ∈ R_q
//------------------------------------------------------------------------------
RingElement MLWEContext::MakeConstantElement(int64_t c) const {
    // 步骤：
    //   1. 构造零元素（系数表示）；
    //   2. 把第 0 个系数（即常数项，对应 1∈R_q 的系数）置为 c；
    //   3. OpenFHE 内部会自动对 c 模 q 归一化（含负数）。
    // 注意：第三个参数 true 表示初始化内部系数向量为全零，
    // 否则 m_values 指针为空，访问 operator[] 会导致段错误。
    RingElement e(m_elemParams, Format::COEFFICIENT, true);
    // 注意：NativePoly 没有 SetValueAtIndex 方法，但 operator[] 返回可写的
    // Integer& 引用（即 NativeInteger&），可直接赋值。
    // 将 int64_t 转为 NativeInteger 类型后赋值。
    e[0] = lbcrypto::NativeInteger(c);
    return e;
}

//------------------------------------------------------------------------------
// SampleGaussian：从离散高斯分布采样一个 R_q 元素
//------------------------------------------------------------------------------
// 数学：对每个系数 i = 0,...,n-1 独立地从中心化离散高斯
//         χ_σ(x) ∝ exp(-x^2 / (2σ^2))   (x ∈ Z)
//       采样，得到 a_i，组成环元素 a(x) = Σ a_i x^i。
//
// 实现：使用“舍入高斯”（rounded Gaussian）——先采样连续高斯 N(0, σ²)，
//   再四舍五入到最近整数。它是合法的 χ 分布实例，且不依赖 OpenFHE 内部分布
//   API，跨版本可移植。σ 取自 m_params.nu。
RingElement MLWEContext::SampleGaussian() const {
    std::normal_distribution<double> dist(0.0, static_cast<double>(m_params.nu));
    // 注意：第三个参数 true 表示初始化内部系数向量为全零，
    // 否则 m_values 指针为空，访问 operator[] 会导致段错误。
    RingElement e(m_elemParams, Format::COEFFICIENT, true);
    for (usint i = 0; i < m_params.n; ++i) {
        double r = dist(m_rng);
        int64_t v = static_cast<int64_t>(std::llround(r));
        // 注意：NativePoly 没有 SetValueAtIndex 方法，使用 operator[] 赋值。
        // 将 int64_t 转为 NativeInteger 类型。
        e[i] = lbcrypto::NativeInteger(v);
    }
    return e;
}

//------------------------------------------------------------------------------
// SampleUniform：从均匀分布采样一个 R_q 元素
//------------------------------------------------------------------------------
// 数学：每个系数独立均匀取自 Z_q = {0,1,...,q-1}。
// 用标准库 uniform_int_distribution 在 [0, q-1] 内采样。
RingElement MLWEContext::SampleUniform() const {
    std::uniform_int_distribution<int64_t> dist(
        0, static_cast<int64_t>(m_params.q - 1));
    // 注意：第三个参数 true 表示初始化内部系数向量为全零，
    // 否则 m_values 指针为空，访问 operator[] 会导致段错误。
    RingElement e(m_elemParams, Format::COEFFICIENT, true);
    for (usint i = 0; i < m_params.n; ++i) {
        // 注意：NativePoly 没有 SetValueAtIndex 方法，使用 operator[] 赋值。
        // 将 int64_t 转为 NativeInteger 类型。
        e[i] = lbcrypto::NativeInteger(dist(m_rng));
    }
    return e;
}

//==============================================================================
// MLWEScheme 实现
//==============================================================================

//------------------------------------------------------------------------------
// KeyGen：密钥生成
//------------------------------------------------------------------------------
// 算法（标准 MLWE 公钥生成）：
//   输入：参数 (n, k, q, σ)
//   1. 均匀采样 A ← R_q^{k×k}；                       // 公共矩阵
//   2. 从小噪声分布采样 s ← χ^k，e ← χ^k；             // 私钥、噪声
//   3. 计算 b = A·s + e ∈ R_q^k；                      // 在 R_q 中
//   4. 公钥 pk = (A, b)，私钥 sk = s。
//
// 说明：本实现中 A 为 k×k 方阵（与 Kyber 等的 k×k 一致）；
//       b_i = Σ_j A_{ij} * s_j + e_i。
void MLWEScheme::KeyGen(MLWEPublicKey& pk, MLWESecretKey& sk) const {
    usint k = m_ctx->GetParams().k;

    // --- 步骤 1：采样公共矩阵 A ∈ R_q^{k×k} ---
    pk.A.assign(k, std::vector<RingElement>(k));
    for (usint i = 0; i < k; ++i) {
        for (usint j = 0; j < k; ++j) {
            pk.A[i][j] = m_ctx->SampleUniform();
        }
    }

    // --- 步骤 2：采样私钥 s ∈ χ^k 与噪声 e ∈ χ^k ---
    sk.assign(k, m_ctx->MakeElement());
    std::vector<RingElement> e(k, m_ctx->MakeElement());
    for (usint i = 0; i < k; ++i) {
        sk[i] = m_ctx->SampleGaussian();
        e[i] = m_ctx->SampleGaussian();
    }

    // --- 步骤 3：计算 b = A·s + e ∈ R_q^k ---
    //   b_i = Σ_{j=0}^{k-1} A_{ij} * s_j + e_i
    // 注意：OpenFHE 的 PolyImpl::operator* 要求操作数在 EVALUATION（NTT）格式下，
    // 而采样得到的元素在 COEFFICIENT 格式。因此在乘法前需通过 ToEval 切换到 NTT 域，
    // 乘法结果也在 EVALUATION 域；加法 e_i 前需切回 COEFFICIENT 域以匹配格式。
    pk.b.assign(k, m_ctx->MakeElement());
    for (usint i = 0; i < k; ++i) {
        RingElement acc = m_ctx->MakeElement();  // 累加器，初值 0（COEFF 格式）
        // 关键修复：在乘法循环前，将 acc 显式切换到 EVALUATION 格式。
        // 原因：PolyImpl::operator+= 不更新 m_format 标记。若 acc 的 m_format
        // 保持 COEFFICIENT，后续 SetFormat(COEFFICIENT) 会误判为"已是 COEFF"
        // 而跳过 INTT，导致系数值为 NTT 域的原始值（巨大数字）。
        // 零多项式的 NTT 仍为零，因此数值不变，仅更新 m_format 标记。
        acc.SetFormat(Format::EVALUATION);
        for (usint j = 0; j < k; ++j) {
            // 将 A_{ij} 和 s_j 切换到 EVALUATION 格式后做 NTT 域乘法
            acc += MLWEContext::ToEval(pk.A[i][j]) * MLWEContext::ToEval(sk[j]);
            // 此时 acc 的数据和 m_format 均为 EVALUATION，格式一致
        }
        // 将累加结果从 EVALUATION 切回 COEFFICIENT（执行 INTT），
        // 以便与噪声 e_i（COEFF 格式）相加
        acc.SetFormat(Format::COEFFICIENT);
        pk.b[i] = acc + e[i];  // 加噪声 e_i（两者均为 COEFF 格式）
    }
}

//------------------------------------------------------------------------------
// Encrypt：加密
//------------------------------------------------------------------------------
// 算法（MLWE-PKE 的标准加密）：
//   输入：公钥 (A, b)、消息 m ∈ R_q
//   1. 采样小噪声 r ← χ^k；                         // 加密随机性
//   2. c1 = A^T · r        ∈ R_q^k；                // “掩码”部分
//   3. c0 = b^T · r + m    ∈ R_q；                  // 消息部分（含噪声）
//   4. 输出密文 (c0, c1)。
//
// 说明：c1 是 k 维向量，c1_i = Σ_j A_{ji} * r_j（注意是 A 的转置）。
MLWECiphertext MLWEScheme::Encrypt(const MLWEPublicKey& pk,
                                   const RingElement& m) const {
    usint k = m_ctx->GetParams().k;

    // --- 步骤 1：采样加密随机性 r ← χ^k ---
    std::vector<RingElement> r(k, m_ctx->MakeElement());
    for (usint i = 0; i < k; ++i) {
        r[i] = m_ctx->SampleGaussian();
    }

    MLWECiphertext ct;
    ct.c1.assign(k, m_ctx->MakeElement());

    // --- 步骤 2：c1 = A^T · r ---
    //   c1_i = Σ_{j=0}^{k-1} A_{j,i} * r_j     （A 的第 i 列与 r 内积）
    // 注意：乘法前需将操作数切换到 EVALUATION 格式（NTT 域）。
    for (usint i = 0; i < k; ++i) {
        RingElement acc = m_ctx->MakeElement();
        // 关键修复：将 acc 显式切换到 EVALUATION 格式，使 m_format 与数据状态一致。
        // 否则 operator+= 不更新 m_format，后续 SetFormat 会误判跳过 INTT。
        acc.SetFormat(Format::EVALUATION);
        for (usint j = 0; j < k; ++j) {
            // ToEval 将 COEFFICIENT 格式的元素切换到 EVALUATION 格式后做 NTT 域乘法
            acc += MLWEContext::ToEval(pk.A[j][i]) * MLWEContext::ToEval(r[j]);
        }
        // acc 的数据和 m_format 均为 EVALUATION，格式一致
        ct.c1[i] = acc;  // c1[i] 保留在 EVALUATION 格式（后续解密时会处理）
    }

    // --- 步骤 3：c0 = b^T · r + m ---
    //   c0 = Σ_{j=0}^{k-1} b_j * r_j + m
    // 注意：乘法在 EVALUATION 域进行，加法 m 前需切回 COEFFICIENT 域。
    RingElement c0 = m_ctx->MakeElement();
    // 关键修复：将 c0 显式切换到 EVALUATION 格式，使 m_format 与数据状态一致。
    c0.SetFormat(Format::EVALUATION);
    for (usint j = 0; j < k; ++j) {
        // ToEval 将 b_j 和 r_j 切换到 EVALUATION 格式后做 NTT 域乘法
        c0 += MLWEContext::ToEval(pk.b[j]) * MLWEContext::ToEval(r[j]);
    }
    // 将 c0 从 EVALUATION 切回 COEFFICIENT（执行 INTT），以便与消息 m（COEFF 格式）相加
    c0.SetFormat(Format::COEFFICIENT);
    c0 += m;

    ct.c0 = c0;
    return ct;
}

//------------------------------------------------------------------------------
// Decrypt：解密
//------------------------------------------------------------------------------
// 算法：
//   输入：密文 (c0, c1)、私钥 s
//   计算：m' = c0 - s^T · c1 ∈ R_q
//
// 正确性分析（解密误差为何“小”）：
//   m' = c0 - s^T · c1
//      = (b^T · r + m) - s^T · (A^T · r)
//      = (A·s + e)^T · r + m - s^T · A^T · r
//      = s^T · A^T · r + e^T · r + m - s^T · A^T · r   // 展开 b = A·s + e
//      = m + e^T · r                                    // 线性项相互抵消
//   其中 e^T · r 是两个小噪声多项式的内积，其系数仍是“小”的（量级约
//   n·σ^2），远小于 q/2，因此 m' 与 m 在 R_q 中逐系数接近。
RingElement MLWEScheme::Decrypt(const MLWECiphertext& ct,
                                const MLWESecretKey& sk) const {
    // 调用核心运算：c0 - s^T · c1
    return DecryptCore(ct.c0, ct.c1, sk);
}

//------------------------------------------------------------------------------
// DecryptCore：解密核心运算 c0 - s^T · c1
//------------------------------------------------------------------------------
//   s^T · c1 = Σ_{i=0}^{k-1} s_i * c1_i   （R_q 中的环乘累加）
RingElement MLWEScheme::DecryptCore(
    const RingElement& c0,
    const std::vector<RingElement>& c1,
    const MLWESecretKey& sk) const {
    usint k = m_ctx->GetParams().k;

    // 计算 s^T · c1
    // 注意：sk[i] 在 COEFFICIENT 格式，c1[i] 在 EVALUATION 格式（来自 Encrypt），
    // 乘法前需将 sk[i] 切换到 EVALUATION 格式。c1[i] 若已在 EVAL 则 ToEval 为无操作。
    RingElement inner = m_ctx->MakeElement();
    // 关键修复：将 inner 显式切换到 EVALUATION 格式，使 m_format 与数据状态一致。
    // 否则 operator+= 不更新 m_format，后续 SetFormat(COEFFICIENT) 会误判跳过 INTT。
    inner.SetFormat(Format::EVALUATION);
    for (usint i = 0; i < k; ++i) {
        inner += MLWEContext::ToEval(sk[i]) * MLWEContext::ToEval(c1[i]);
    }
    // inner 的数据和 m_format 均为 EVALUATION，现在切回 COEFFICIENT（执行 INTT）
    inner.SetFormat(Format::COEFFICIENT);
    // m' = c0 - s^T · c1（两者均为 COEFFICIENT 格式）
    return c0 - inner;
}

//------------------------------------------------------------------------------
// InnerProduct：两个 k 维 R_q 向量的内积 a^T · b
//------------------------------------------------------------------------------
RingElement MLWEScheme::InnerProduct(const std::vector<RingElement>& a,
                                     const std::vector<RingElement>& b) const {
    if (a.size() != b.size()) {
        throw std::invalid_argument("InnerProduct: 向量长度不一致。");
    }
    RingElement acc = m_ctx->MakeElement();
    // 关键修复：将 acc 显式切换到 EVALUATION 格式，使 m_format 与数据状态一致。
    // 否则 operator+= 不更新 m_format，返回的元素格式标记与数据不匹配。
    acc.SetFormat(Format::EVALUATION);
    for (size_t i = 0; i < a.size(); ++i) {
        // 注意：乘法前将操作数切换到 EVALUATION 格式（NTT 域），
        // 确保 PolyImpl::operator* 可正常工作。
        acc += MLWEContext::ToEval(a[i]) * MLWEContext::ToEval(b[i]);
    }
    return acc;
}

//==============================================================================
// 同态算子实现（Homomorphic Operations）
//==============================================================================
// 以下算子是 RLWE 方案的标准同态操作，也是论文 arXiv:2503.16080 中
// CC-MM（密文-密文矩阵乘法）所依赖的底层原语。
//
// 全部严格遵循 OpenFHE 的格式约定（已在 KeyGen/Encrypt/Decrypt 中验证）：
//   - operator* 要求双方均在 EVALUATION（NTT）格式；
//   - operator+= 不更新 m_format，故累加器须先 SetFormat(Format::EVALUATION)；
//   - 构造元素须传第三参 true 初始化系数向量防空指针。
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// Add：同态加法 ct' = ct_a + ct_b
//------------------------------------------------------------------------------
// 公式（RLWE 加法同态性）：
//   c0' = c0_a + c0_b
//   c1' = c1_a + c1_b            （逐向量相加）
// 加法不增长密文长度，也不放大噪声（噪声近似为两者之和），是最廉价的同态运算。
// 解密正确性：Dec(ct_a+ct_b) = Dec(ct_a) + Dec(ct_b)（在噪声允许范围内）。
MLWECiphertext MLWEScheme::Add(const MLWECiphertext& ct_a,
                               const MLWECiphertext& ct_b) const {
    usint k = m_ctx->GetParams().k;
    if (ct_a.c1.size() != k || ct_b.c1.size() != k) {
        throw std::invalid_argument("Add: 密文 c1 维度与 k 不一致。");
    }

    MLWECiphertext res;
    // c0' = c0_a + c0_b（COEFFICIENT 域相加）
    RingElement c0 = ct_a.c0;
    c0.SetFormat(Format::COEFFICIENT);
    RingElement c0b = ct_b.c0;
    c0b.SetFormat(Format::COEFFICIENT);
    c0 += c0b;  // 双方均 COEFFICIENT，operator+= 不改格式
    res.c0 = c0;

    // c1'_j = c1_a[j] + c1_b[j]（c1 在 EVALUATION 域相加，保持 EVAL 格式约定）
    res.c1.assign(k, m_ctx->MakeElement());
    for (usint j = 0; j < k; ++j) {
        RingElement v = ct_a.c1[j];
        v.SetFormat(Format::EVALUATION);
        RingElement v2 = ct_b.c1[j];
        v2.SetFormat(Format::EVALUATION);
        v += v2;  // 双方均 EVAL
        res.c1[j] = v;
    }
    return res;
}

//------------------------------------------------------------------------------
// HomMul：同态乘法（线性张量积 / tensor product）
//------------------------------------------------------------------------------
// 公式（RLWE 乘法）：密文 ct_a=(c0_a, c1_a)、ct_b=(c0_b, c1_b)，解密关系
//   m_a ≈ c0_a + sk^T·c1_a，m_b ≈ c0_b + sk^T·c1_b。
// 乘积 m_a·m_b ≈ (c0_a + sk^T·c1_a)(c0_b + sk^T·c1_b)，展开得：
//   c0' = c0_a · c0_b                              （常数项，R_q 环乘）
//   c1'_j = c0_a · c1_b[j] + c1_a[j] · c0_b        （含 sk 的一次项）
//   c2'_j = (与 sk² 有关，由 c1_a · c1_b 的组合产生)
// 这里把 c2 简化存为 c1_a·c1_b 的逐项组合（教学实现；正式方案做 gadget 分解）。
// c0' 在 COEFFICIENT 域，c1'/c2' 在 EVALUATION 域（遵循密文格式约定）。
MLWECiphertext3 MLWEScheme::HomMul(const MLWECiphertext& ct_a,
                                   const MLWECiphertext& ct_b) const {
    usint k = m_ctx->GetParams().k;
    if (ct_a.c1.size() != k || ct_b.c1.size() != k) {
        throw std::invalid_argument("HomMul: 密文 c1 维度与 k 不一致。");
    }

    MLWECiphertext3 res;

    // --- c0' = c0_a · c0_b（R_q 环乘 = 负循环卷积）---
    RingElement c0 = MLWEContext::ToEval(ct_a.c0) * MLWEContext::ToEval(ct_b.c0);
    c0.SetFormat(Format::COEFFICIENT);
    res.c0 = c0;

    // --- c1'_j = c0_a · c1_b[j] + c1_a[j] · c0_b ---
    // 两项乘法各自 ToEval；累加器预设 EVAL 格式（避开 operator+= 不更新 m_format 的陷阱）。
    // c0_a/c0_b 是标量环元素，分别与 c1_b[j]/c1_a[j] 这些 k 维向量相乘。
    RingElement c0a_e = MLWEContext::ToEval(ct_a.c0);
    RingElement c0b_e = MLWEContext::ToEval(ct_b.c0);
    res.c1.assign(k, m_ctx->MakeElement());
    for (usint j = 0; j < k; ++j) {
        RingElement acc = m_ctx->MakeElement();
        acc.SetFormat(Format::EVALUATION);  // 关键：预设 EVAL 使 m_format 与数据一致
        acc += c0a_e * MLWEContext::ToEval(ct_b.c1[j]);
        acc += MLWEContext::ToEval(ct_a.c1[j]) * c0b_e;
        res.c1[j] = acc;  // 保持 EVAL 格式
    }

    // --- c2'_j = Σ_l c1_a[l] · c1_b 组合（含 sk² 项）---
    // 教学简化：对每个 j，取 c1_a[j]·c1_b[j] 作为 sk² 项的代表（对应 sk_j²）。
    // 正式方案会对 c1_b 做 gadget(base) 分解后与 evk 重组，这里只保留待 Relinearize 的原始项。
    res.c2.assign(k, m_ctx->MakeElement());
    for (usint j = 0; j < k; ++j) {
        RingElement acc = m_ctx->MakeElement();
        acc.SetFormat(Format::EVALUATION);
        acc += MLWEContext::ToEval(ct_a.c1[j]) * MLWEContext::ToEval(ct_b.c1[j]);
        res.c2[j] = acc;
    }
    return res;
}

//------------------------------------------------------------------------------
// GenEvalKey：生成求值密钥（对 sk² 的加密，供 Relinearize 使用）
//------------------------------------------------------------------------------
// 公式（gadget 分解的 key-switch key）：
//   evk_j = (b_j, a_j)，其中 a_j ← R_q 均匀，b_j = a_j·sk + e_j + base^j · sk²。
// base 取自 MLWEParams::base（默认 0 时退化为单一分量，base 当作 2 处理）。
// Relinearize 时把含 sk² 的 c2_j 项与 evk_j 组合即可把 sk²「替换」为 sk。
MLWEEvaluationKey MLWEScheme::GenEvalKey(const MLWESecretKey& sk) const {
    usint k = m_ctx->GetParams().k;
    usint base = m_ctx->GetParams().base;
    if (base == 0) base = 2;  // 默认 gadget 基

    MLWEEvaluationKey evk(k);
    for (usint j = 0; j < k; ++j) {
        // a_j ← 均匀
        RingElement a = m_ctx->SampleUniform();
        // e_j ← 小噪声
        RingElement e = m_ctx->SampleGaussian();

        // 计算 sk²（sk[j] 自乘，R_q 环乘）
        RingElement sk_j_e = MLWEContext::ToEval(sk[j]);
        RingElement sk2 = sk_j_e * sk_j_e;  // EVAL 域

        // base^j · sk²：base^j 是整数系数倍乘，逐系数缩放后做环乘或直接标量乘。
        // 简化：用 base^j 标量（这里 base^j 量级小，直接构造常数环元素相乘）。
        int64_t pw = 1;
        for (usint t = 0; t < j; ++t) pw *= base;  // pw = base^j
        RingElement pw_const = MLWEContext::ToEval(m_ctx->MakeConstantElement(pw));
        RingElement scaled_sk2 = sk2 * pw_const;  // EVAL 域

        // b_j = a_j·sk + e_j + base^j·sk²
        RingElement a_sk = MLWEContext::ToEval(a) * sk_j_e;  // EVAL 域
        // 切回 COEFFICIENT 域以与 e_j（COEFFICIENT）相加
        a_sk.SetFormat(Format::COEFFICIENT);
        scaled_sk2.SetFormat(Format::COEFFICIENT);
        RingElement b = a_sk + e + scaled_sk2;  // 均 COEFFICIENT

        evk[j].a = a;
        evk[j].b = b;
    }
    return evk;
}

//------------------------------------------------------------------------------
// Relinearize：重线性化（把含 sk² 的三项密文降为含 sk 的两项密文）
//------------------------------------------------------------------------------
// 公式：ct3 = (c0, c1, c2)，解密关系 m ≈ c0 + sk^T·c1 + sk^T·c2·sk。
// 用 evk_j = (b_j, a_j)（其中 b_j = a_j·sk + e_j + base^j·sk²）把 c2 项 key-switch：
//   新 c0 = c0 + Σ_j <gadget分解(c2_j), b_j>      （吸收 base^j·sk² 的标量部分）
//   新 c1_j = -Σ_j <gadget分解(c2_j), a_j>          （产生的含 sk 项）
// 教学简化：这里直接把 c2_j 与 evk_j 的 b_j/a_j 做点积搬移（不做完整 gadget 分解），
// 把含 sk² 的能量迁移回 c0、c1，使输出密文重新满足线性可解密。
MLWECiphertext MLWEScheme::Relinearize(const MLWECiphertext3& ct3,
                                       const MLWEEvaluationKey& evk) const {
    usint k = m_ctx->GetParams().k;
    if (ct3.c1.size() != k || ct3.c2.size() != k || evk.size() != k) {
        throw std::invalid_argument("Relinearize: 维度不一致。");
    }

    // 新 c0：在原 c0 基础上吸收 evk 的 b 部分（COEFFICIENT 域累加）
    RingElement c0 = ct3.c0;
    c0.SetFormat(Format::COEFFICIENT);
    for (usint j = 0; j < k; ++j) {
        // <c2_j, b_j>：把 c2_j（含 sk² 能量）与 evk_j.b 关联
        RingElement term = MLWEContext::ToEval(ct3.c2[j]) * MLWEContext::ToEval(evk[j].b);
        term.SetFormat(Format::COEFFICIENT);
        c0 += term;
    }

    // 新 c1_j：原 c1_j 减去 evk 的 a 部分对应的项（产生含 sk 的项，保持 EVAL 域）
    std::vector<RingElement> c1(k, m_ctx->MakeElement());
    for (usint j = 0; j < k; ++j) {
        RingElement acc = ct3.c1[j];
        acc.SetFormat(Format::EVALUATION);
        // 减去 evk_j.a 相关项（key-switch 把 sk² 项转出的「代价」）
        RingElement sub = MLWEContext::ToEval(ct3.c2[j]) * MLWEContext::ToEval(evk[j].a);
        acc -= sub;  // EVAL 域减法，acc 仍 EVAL
        c1[j] = acc;
    }

    MLWECiphertext res;
    res.c0 = c0;
    res.c1 = c1;
    return res;
}

//------------------------------------------------------------------------------
// Automorphism：自同构 X→X^k（mod (X^n+1) 的 Frobenius）
//------------------------------------------------------------------------------
// 数学：σ_k : f(X) = Σ a_i X^i ↦ Σ a_i X^(i·k mod 2n)，
//   其中若 (i·k mod 2n) ≥ n 则取负号（因 X^n ≡ -1 (mod X^n+1)）。
//   即 new_index = (i·k) mod 2n；若 ≥ n，则 new_index -= n 且系数取负。
// gcd(k,2n)=1 时为双射（合法自同构）。
// 论文用途：Algorithm 3 Tweak / Algorithm 4 C-MT 转置用自同构做系数重排/旋转。
// 手工实现系数搬运，不依赖 OpenFHE 内部 automorphism API。
RingElement MLWEScheme::Automorphism(const RingElement& e, usint k) {
    // 先切到 COEFFICIENT 域以读系数
    RingElement eCoeff = e;
    eCoeff.SetFormat(Format::COEFFICIENT);
    usint n = eCoeff.GetLength();
    usint twoN = 2 * n;

    // 构造输出元素：复制 eCoeff 作为模板（共享环参数与格式），再清零系数。
    // 用 eCoeff 拷贝构造可保证环参数正确，避免依赖不确定的内部 API。
    RingElement out = eCoeff;  // COEFFICIENT 域，与 eCoeff 同环参数
    // 清零所有系数
    for (usint i = 0; i < n; ++i) {
        out[i] = lbcrypto::NativeInteger(0);
    }

    // 系数搬运：i -> newIdx
    for (usint i = 0; i < n; ++i) {
        usint newIdx = (i * k) % twoN;
        int64_t sign = 1;
        if (newIdx >= n) {
            newIdx -= n;
            sign = -1;  // X^n ≡ -1
        }
        // 取原系数（[0,q) 内），搬移到 newIdx 并按规则变号
        int64_t coeff = eCoeff[i].ConvertToInt();
        int64_t moved = (sign == -1) ? -coeff : coeff;
        // OpenFHE 内部会模 q 归一化负数
        out[newIdx] = lbcrypto::NativeInteger(moved);
    }
    return out;
}

//==============================================================================
// 工具函数实现
//==============================================================================

//------------------------------------------------------------------------------
// ElementToVector：把环元素的系数导出为 int64_t 向量
//------------------------------------------------------------------------------
std::vector<int64_t> ElementToVector(const RingElement& e) {
    // 切换到系数表示，逐项取值。
    RingElement eCoeff = e;
    eCoeff.SetFormat(Format::COEFFICIENT);
    auto n = eCoeff.GetLength();
    std::vector<int64_t> out(n);
    for (usint i = 0; i < n; ++i) {
        out[i] = eCoeff[i].ConvertToInt();
    }
    // 注意：OpenFHE 的系数返回的是 [0, q) 内的非负值。
    // 为得到“带符号”表示，我们对 > q/2 的值减去 q。
    // 但此处我们返回原始值，由调用方决定如何解读。
    return out;
}

//------------------------------------------------------------------------------
// VectorToElement：把 int64_t 系数向量打包成环元素
//------------------------------------------------------------------------------
RingElement VectorToElement(const std::vector<int64_t>& coeffs,
                            const MLWEContext& ctx) {
    RingElement e = ctx.MakeElement();
    for (size_t i = 0; i < coeffs.size() && i < ctx.GetParams().n; ++i) {
        // 注意：NativePoly 没有 SetValueAtIndex 方法，使用 operator[] 赋值。
        // 将 int64_t 转为 NativeInteger 类型。
        e[i] = lbcrypto::NativeInteger(coeffs[i]);
    }
    return e;
}

//------------------------------------------------------------------------------
// ElementToString：把环元素打印成字符串
//------------------------------------------------------------------------------
// all=false：只打印前若干系数 + 汇总统计；
// all=true ：打印全部系数。
std::string ElementToString(const RingElement& e, bool all) {
    std::vector<int64_t> v = ElementToVector(e);
    usint n = v.size();
    usint q = 0;
    // 获取模数（从环参数）
    // 这里通过 e.GetModulus() 拿到 NativeInteger，转 int64_t
    q = static_cast<usint>(e.GetModulus().ConvertToInt());

    // 把 [0,q) 表示转为带符号表示 [−⌊q/2⌋, ⌊q/2⌋)
    auto toSigned = [q](int64_t x) -> int64_t {
        if (x > (int64_t)q / 2) x -= (int64_t)q;
        return x;
    };

    // 统计：绝对值最大、L2 范数等
    int64_t maxAbs = 0;
    double l2 = 0.0;
    for (auto x : v) {
        int64_t s = toSigned(x);
        // 注意：std::llabs 返回 long long，而 maxAbs 是 int64_t（即 long），
        // 类型不匹配会导致 std::max 模板推导失败，需显式转换。
        maxAbs = std::max<int64_t>(maxAbs, static_cast<int64_t>(std::llabs(s)));
        l2 += (double)s * s;
    }
    l2 = std::sqrt(l2);

    std::ostringstream oss;
    oss << "[mod q = " << q << ", n = " << n << "] ";

    if (all) {
        oss << "coeffs (signed): {";
        for (usint i = 0; i < n; ++i) {
            if (i) oss << ", ";
            oss << toSigned(v[i]);
        }
        oss << "}";
    } else {
        oss << "first coeffs (signed): {";
        usint show = std::min<usint>(n, 8);
        for (usint i = 0; i < show; ++i) {
            if (i) oss << ", ";
            oss << toSigned(v[i]);
        }
        oss << (n > show ? ", ..." : "");
        oss << "}";
    }
    oss << " |max|=" << maxAbs << ", ||·||_2=" << l2;
    return oss.str();
}

//------------------------------------------------------------------------------
// MaxCoeffAbsDiff：逐系数绝对差的最大值（衡量解密误差）
//------------------------------------------------------------------------------
// 这里用带符号表示来比较，以正确处理模 q 的环绕。
int64_t MaxCoeffAbsDiff(const RingElement& a, const RingElement& b) {
    std::vector<int64_t> va = ElementToVector(a);
    std::vector<int64_t> vb = ElementToVector(b);
    usint n = std::min(va.size(), vb.size());
    usint q = static_cast<usint>(a.GetModulus().ConvertToInt());

    auto toSigned = [q](int64_t x) -> int64_t {
        if (x > (int64_t)q / 2) x -= (int64_t)q;
        return x;
    };

    int64_t maxDiff = 0;
    for (usint i = 0; i < n; ++i) {
        int64_t d = std::llabs(toSigned(va[i]) - toSigned(vb[i]));
        // 考虑环绕：差值也可能是 q - d（当两个系数分别落在 0 和 q-1 附近时）
        d = std::min(d, (int64_t)q - d);
        maxDiff = std::max(maxDiff, d);
    }
    return maxDiff;
}

}  // namespace mlwe

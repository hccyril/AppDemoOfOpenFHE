#define PROFILE
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cmath>

// 说明: 头文件路径采用与 CMake 中 target_include_directories 保持一致的相对前缀。
// - OpenFHE PKE 顶层接口: 提供 BFVrns 等方案的上下文与操作
// - 调试/计时工具: 提供 TIC/TOC 计时宏和 TimeVar 类型
// - 标准格参数查询: 通过公开 API 查表（例如 FindMaxQ）以便对照标准表
#include "pke/openfhe.h"
#include "core/utils/debug.h"
#include "lattice/stdlatticeparms.h"

using namespace lbcrypto;

// 运行配置:
// - secs: 要求的安全级别集合（同时覆盖 classic 与 quantum）
// - depths: 乘法深度列表，用于让库自动选择合适的参数
// - ptm: 明文模数。65537 为较小且常用的素数，便于 BFV/ BFVrns 自动选参
// - useAllDists: 是否遍历所有密钥分布；默认仅 ternary（与 BFVrns 常用设置一致）
struct RunConfig {
    std::vector<SecurityLevel> secs { HEStd_128_classic, HEStd_192_classic, HEStd_256_classic,
                                      HEStd_128_quantum, HEStd_192_quantum, HEStd_256_quantum };
    std::vector<uint32_t> depths {1,2,3};
    PlaintextModulus ptm = 65537;
    bool useAllDists = false;
};

// 将 SecurityLevel 枚举值转为字符串，供 CSV 输出
static std::string ToString(SecurityLevel sl){ std::ostringstream os; os<<sl; return os.str(); }
// 将分布枚举值转为字符串，供 CSV 输出
static const char* DistName(DistributionType d){ switch(d){case HEStd_uniform: return "HEStd_uniform"; case HEStd_error: return "HEStd_error"; case HEStd_ternary: return "HEStd_ternary"; default: return "UNKNOWN";} }
// 计时工具封装: 返回 TOC(t) 毫秒值
static double Ms(TimeVar& t){ return TOC(t); }

// 主测试例: 根据安全级别/深度/分布生成上下文，测量关键操作用时并输出 CSV 表
void runtest2(){
    RunConfig cfg;
    // 默认仅测试 ternary 分布；如需更多分布，打开 useAllDists
    std::vector<DistributionType> dists = { HEStd_ternary };
    if(cfg.useAllDists) dists = { HEStd_uniform, HEStd_error, HEStd_ternary };

    // CSV 表头说明:
    // - scheme: 加密方案（这里固定 BFVrns）
    // - sec: 安全级别（classic/quantum 的 128/192/256）
    // - dist: 密钥分布类型（uniform/error/ternary）
    // - reqDepth: 请求的乘法深度（库将基于此自动选参）
    // - ptm: 明文模数（PlaintextModulus）
    // - ringDim: 实际使用的环维度（n = CyclotomicOrder/2）
    // - log2q: 累加各 CRT 塔素数模数的 log2 近似值
    // - towers: CRT 塔层数（质数模数数量）
    // - standardMaxLogQ: 根据标准格参数（dist+sec+ringDim）查询到的最大 logQ（供对照参考）
    // - *_ms: 关键操作的毫秒耗时（KeyGen, EvalMultKeysGen, Encrypt 两次, EvalAdd, EvalMult, Decrypt）
    std::cout << "scheme,sec,dist,reqDepth,ptm,ringDim,log2q,towers,standardMaxLogQ,keygen_ms,evalmultkeys_ms,enc1_ms,enc2_ms,add_ms,mult_ms,dec_ms" << std::endl;

    for(auto dist : dists){
        for(auto sec : cfg.secs){
            for(auto reqDepth : cfg.depths){
                // 通过 CCParams 设定请求的参数。注意：不手动设置 ringDim，交由库自动选取以满足安全与深度需求
                CCParams<CryptoContextBFVRNS> params;
                params.SetPlaintextModulus(cfg.ptm);
                params.SetMultiplicativeDepth(reqDepth);
                params.SetSecurityLevel(sec);
                params.SetMaxRelinSkDeg(3); // 典型的重线性化最大阶
                // 密钥分布选择：
                // - ternary: 统一采用 UNIFORM_TERNARY（BFVrns 常用）
                // - uniform/error: 分别设为 UNIFORM_TERNARY / GAUSSIAN（实验用）
                switch(dist){
                    case HEStd_ternary: params.SetSecretKeyDist(UNIFORM_TERNARY); break;
                    case HEStd_uniform: params.SetSecretKeyDist(UNIFORM_TERNARY); break;
                    case HEStd_error:   params.SetSecretKeyDist(GAUSSIAN); break;
                    default:            params.SetSecretKeyDist(UNIFORM_TERNARY); break;
                }

                // 生成加密上下文；如遇到无效组合（安全级别/深度/ptm 不可满足），跳过该行
                CryptoContext<DCRTPoly> cc;
                try { cc = GenCryptoContext(params); }
                catch(const std::exception&){ continue; }
                // 启用所需功能模块
                cc->Enable(PKE);
                cc->Enable(KEYSWITCH);
                cc->Enable(LEVELEDSHE);
                cc->Enable(ADVANCEDSHE);

                // 采集派生的参数：n、log2(q) 近似值、CRT 塔层数
                usint ringDim = cc->GetCryptoParameters()->GetElementParams()->GetCyclotomicOrder()/2;
                const auto elemParams = cc->GetCryptoParameters()->GetElementParams();
                double log2q = 0.0; const auto& crtParams = elemParams->GetParams();
                for(const auto& p : crtParams){ log2q += std::log2(p->GetModulus().ConvertToDouble()); }
                size_t towers = crtParams.size();
                // 标准格参数对照查询：用于参考标准表中的最大 logQ
                usint standardMaxLogQ = StdLatticeParm::FindMaxQ(dist, sec, ringDim);

                // 计时各阶段的耗时（毫秒）
                TimeVar t; double keygen_ms=0, evmk_ms=0, enc1_ms=0, enc2_ms=0, add_ms=0, mult_ms=0, dec_ms=0;
                TIC(t); auto kp = cc->KeyGen(); keygen_ms = Ms(t); if(!kp.good()) continue; // 若 KeyGen 失败，跳过
                TIC(t); cc->EvalMultKeysGen(kp.secretKey); evmk_ms = Ms(t); // 生成乘法评估密钥

                // 构造两组打包明文并分别加密，单独计时
                std::vector<int64_t> v1{1,2,3,4,5,6,7,8,9,10,11,12};
                std::vector<int64_t> v2{2,3,4,5,6,7,8,9,10,11,12,13};
                auto p1 = cc->MakePackedPlaintext(v1); auto p2 = cc->MakePackedPlaintext(v2);
                TIC(t); auto c1 = cc->Encrypt(kp.publicKey, p1); enc1_ms = Ms(t);
                TIC(t); auto c2 = cc->Encrypt(kp.publicKey, p2); enc2_ms = Ms(t);

                // 评估加法与乘法，分别计时
                TIC(t); auto cadd = cc->EvalAdd(c1,c2); add_ms = Ms(t);
                TIC(t); auto cmul = cc->EvalMult(c1,c2); mult_ms = Ms(t);

                // 解密乘法结果并设置输出长度，计时
                Plaintext pout; TIC(t); cc->Decrypt(kp.secretKey, cmul, &pout); dec_ms = Ms(t); pout->SetLength(p1->GetLength());

                // 输出一行 CSV 结果，便于后续导入到数据表或论文结果对照
                std::cout << "BFVrns" << ',' << ToString(sec) << ',' << DistName(dist) << ',' << reqDepth << ','
                          << cfg.ptm << ',' << ringDim << ',' << std::fixed << std::setprecision(2) << log2q << ','
                          << towers << ',' << standardMaxLogQ << ','
                          << std::setprecision(3) << keygen_ms << ',' << evmk_ms << ',' << enc1_ms << ',' << enc2_ms << ','
                          << add_ms << ',' << mult_ms << ',' << dec_ms << std::endl;
            }
        }
    }
    // 结束提示
    std::cout << "runtest2 done!" << std::endl;
}

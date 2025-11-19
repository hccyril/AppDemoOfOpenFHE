#define PROFILE
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cmath>

// Working relative includes (same style as AppDemoOpenFHE.cpp)
#include "pke/openfhe.h"
#include "core/utils/debug.h"
#include "lattice/stdlatticeparms.h"

using namespace lbcrypto;

struct RunConfig {
    std::vector<SecurityLevel> secs { HEStd_128_classic, HEStd_192_classic, HEStd_256_classic,
                                      HEStd_128_quantum, HEStd_192_quantum, HEStd_256_quantum };
    std::vector<uint32_t> depths {1,2,3};
    PlaintextModulus ptm = 65537;
    bool useAllDists = false;
};

static std::string ToString(SecurityLevel sl){ std::ostringstream os; os<<sl; return os.str(); }
static const char* DistName(DistributionType d){ switch(d){case HEStd_uniform: return "HEStd_uniform"; case HEStd_error: return "HEStd_error"; case HEStd_ternary: return "HEStd_ternary"; default: return "UNKNOWN";} }
static double Ms(TimeVar& t){ return TOC(t); }

void runtest2(){
    RunConfig cfg;
    std::vector<DistributionType> dists = { HEStd_ternary };
    if(cfg.useAllDists) dists = { HEStd_uniform, HEStd_error, HEStd_ternary };

    std::cout << "scheme,sec,dist,reqDepth,ptm,ringDim,log2q,towers,standardMaxLogQ,keygen_ms,evalmultkeys_ms,enc1_ms,enc2_ms,add_ms,mult_ms,dec_ms" << std::endl;

    for(auto dist : dists){
        for(auto sec : cfg.secs){
            for(auto reqDepth : cfg.depths){
                CCParams<CryptoContextBFVRNS> params;
                params.SetPlaintextModulus(cfg.ptm);
                params.SetMultiplicativeDepth(reqDepth);
                params.SetSecurityLevel(sec);
                params.SetMaxRelinSkDeg(3);
                switch(dist){
                    case HEStd_ternary: params.SetSecretKeyDist(UNIFORM_TERNARY); break;
                    case HEStd_uniform: params.SetSecretKeyDist(UNIFORM_TERNARY); break;
                    case HEStd_error:   params.SetSecretKeyDist(GAUSSIAN); break;
                    default:            params.SetSecretKeyDist(UNIFORM_TERNARY); break;
                }

                CryptoContext<DCRTPoly> cc;
                try { cc = GenCryptoContext(params); }
                catch(const std::exception&){ continue; }
                cc->Enable(PKE); cc->Enable(KEYSWITCH); cc->Enable(LEVELEDSHE); cc->Enable(ADVANCEDSHE);

                usint ringDim = cc->GetCryptoParameters()->GetElementParams()->GetCyclotomicOrder()/2;
                const auto elemParams = cc->GetCryptoParameters()->GetElementParams();
                double log2q = 0.0; const auto& crtParams = elemParams->GetParams();
                for(const auto& p : crtParams){ log2q += std::log2(p->GetModulus().ConvertToDouble()); }
                size_t towers = crtParams.size();
                usint standardMaxLogQ = StdLatticeParm::FindMaxQ(dist, sec, ringDim);

                TimeVar t; double keygen_ms=0, evmk_ms=0, enc1_ms=0, enc2_ms=0, add_ms=0, mult_ms=0, dec_ms=0;
                TIC(t); auto kp = cc->KeyGen(); keygen_ms = Ms(t); if(!kp.good()) continue;
                TIC(t); cc->EvalMultKeysGen(kp.secretKey); evmk_ms = Ms(t);

                std::vector<int64_t> v1{1,2,3,4,5,6,7,8,9,10,11,12};
                std::vector<int64_t> v2{2,3,4,5,6,7,8,9,10,11,12,13};
                auto p1 = cc->MakePackedPlaintext(v1); auto p2 = cc->MakePackedPlaintext(v2);
                TIC(t); auto c1 = cc->Encrypt(kp.publicKey, p1); enc1_ms = Ms(t);
                TIC(t); auto c2 = cc->Encrypt(kp.publicKey, p2); enc2_ms = Ms(t);
                TIC(t); auto cadd = cc->EvalAdd(c1,c2); add_ms = Ms(t);
                TIC(t); auto cmul = cc->EvalMult(c1,c2); mult_ms = Ms(t);
                Plaintext pout; TIC(t); cc->Decrypt(kp.secretKey, cmul, &pout); dec_ms = Ms(t); pout->SetLength(p1->GetLength());

                std::cout << "BFVrns" << ',' << ToString(sec) << ',' << DistName(dist) << ',' << reqDepth << ','
                          << cfg.ptm << ',' << ringDim << ',' << std::fixed << std::setprecision(2) << log2q << ','
                          << towers << ',' << standardMaxLogQ << ','
                          << std::setprecision(3) << keygen_ms << ',' << evmk_ms << ',' << enc1_ms << ',' << enc2_ms << ','
                          << add_ms << ',' << mult_ms << ',' << dec_ms << std::endl;
            }
        }
    }
    std::cout << "runtest2 done!" << std::endl;
}

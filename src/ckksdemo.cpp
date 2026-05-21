/*

Example for CKKS bootstrapping with full packing

*/

#define PROFILE

#include "openfhe.h"

using namespace lbcrypto;

void SimpleBootstrapExample() {
    CCParams<CryptoContextCKKSRNS> parameters;
    // A. Specify main parameters
    /*  A1) Secret key distribution
    * The secret key distribution for CKKS should either be SPARSE_TERNARY or UNIFORM_TERNARY.
    * The SPARSE_TERNARY distribution was used in the original CKKS paper,
    * but in this example, we use UNIFORM_TERNARY because this is included in the homomorphic
    * encryption standard.
    */
    SecretKeyDist secretKeyDist = UNIFORM_TERNARY;
    parameters.SetSecretKeyDist(secretKeyDist);

    /*  A2) Desired security level based on FHE standards.
    * In this example, we use the "NotSet" option, so the example can run more quickly with
    * a smaller ring dimension. Note that this should be used only in
    * non-production environments, or by experts who understand the security
    * implications of their choices. In production-like environments, we recommend using
    * HEStd_128_classic, HEStd_192_classic, or HEStd_256_classic for 128-bit, 192-bit,
    * or 256-bit security, respectively. If you choose one of these as your security level,
    * you do not need to set the ring dimension.
    */
    parameters.SetSecurityLevel(HEStd_NotSet);
    parameters.SetRingDim(1 << 12);

    /*  A3) Scaling parameters.
    * By default, we set the modulus sizes and rescaling technique to the following values
    * to obtain a good precision and performance tradeoff. We recommend keeping the parameters
    * below unless you are an FHE expert.
    */
#if NATIVEINT == 128
    ScalingTechnique rescaleTech = FIXEDAUTO;
    usint dcrtBits               = 78;
    usint firstMod               = 89;
#else
    ScalingTechnique rescaleTech = FLEXIBLEAUTO;
    usint dcrtBits               = 59;
    usint firstMod               = 60;
#endif

    parameters.SetScalingModSize(dcrtBits);
    parameters.SetScalingTechnique(rescaleTech);
    parameters.SetFirstModSize(firstMod);

    /*  A4) Multiplicative depth.
    * The goal of bootstrapping is to increase the number of available levels we have, or in other words,
    * to dynamically increase the multiplicative depth. However, the bootstrapping procedure itself
    * needs to consume a few levels to run. We compute the number of bootstrapping levels required
    * using GetBootstrapDepth, and add it to levelsAvailableAfterBootstrap to set our initial multiplicative
    * depth. We recommend using the input parameters below to get started.
    */
    std::vector<uint32_t> levelBudget = {4, 4};

    // Note that the actual number of levels avalailable after bootstrapping before next bootstrapping 
    // will be levelsAvailableAfterBootstrap - 1 because an additional level
    // is used for scaling the ciphertext before next bootstrapping (in 64-bit CKKS bootstrapping)
    uint32_t levelsAvailableAfterBootstrap = 10;
    usint depth = levelsAvailableAfterBootstrap + FHECKKSRNS::GetBootstrapDepth(levelBudget, secretKeyDist);
    parameters.SetMultiplicativeDepth(depth);

    CryptoContext<DCRTPoly> cryptoContext = GenCryptoContext(parameters);

    cryptoContext->Enable(PKE);
    cryptoContext->Enable(KEYSWITCH);
    cryptoContext->Enable(LEVELEDSHE);
    cryptoContext->Enable(ADVANCEDSHE);
    cryptoContext->Enable(FHE);

    usint ringDim = cryptoContext->GetRingDimension();
    // This is the maximum number of slots that can be used for full packing.
    usint numSlots = ringDim / 2;
    std::cout << "CKKS scheme is using ring dimension " << ringDim << std::endl << std::endl;

    cryptoContext->EvalBootstrapSetup(levelBudget);

    auto keyPair = cryptoContext->KeyGen();
    cryptoContext->EvalMultKeyGen(keyPair.secretKey);
    cryptoContext->EvalBootstrapKeyGen(keyPair.secretKey, numSlots);

    std::vector<double> x = {0.25, 0.5, 0.75, 1.0, 2.0, 3.0, 4.0, 5.0};
    std::vector<double> y = {1.5, 0.25, 2.0, 0.5, 1.25, 0.75, 1.0, 1.5};
    size_t encodedLength  = x.size();
    std::vector<double> expected(encodedLength);
    for (size_t i = 0; i < encodedLength; ++i) {
        expected[i] = x[i] * y[i] * y[i];
    }

    // We start with a depleted ciphertext that has used up all of its levels.
    Plaintext ptxtA = cryptoContext->MakeCKKSPackedPlaintext(x, 1, depth - 1);
    // 中文说明：这里构造一个同态相乘用的第二个明文向量
    Plaintext ptxtB = cryptoContext->MakeCKKSPackedPlaintext(y, 1, depth - 1);

    ptxtA->SetLength(encodedLength);
    ptxtB->SetLength(encodedLength);
    std::cout << "InputA: " << ptxtA << std::endl;
    std::cout << "InputB: " << ptxtB << std::endl;

    Ciphertext<DCRTPoly> ciphA = cryptoContext->Encrypt(keyPair.publicKey, ptxtA);
    // 中文说明：创建第二个密文 ctB，用于同态相乘
    Ciphertext<DCRTPoly> ciphB = cryptoContext->Encrypt(keyPair.publicKey, ptxtB);

    std::cout << "Initial number of levels remaining: " << depth - ciphA->GetLevel() << std::endl;

    // 中文说明：在消耗完层数的情况下先做一次同态乘法，通常会得到不正确的结果
    auto ciphertextBeforeMult = cryptoContext->EvalMult(ciphA, ciphB);
    ciphertextBeforeMult = cryptoContext->EvalMult(ciphertextBeforeMult, ciphB);
    Plaintext expectedPtxt = cryptoContext->MakeCKKSPackedPlaintext(expected);
    expectedPtxt->SetLength(encodedLength);
    try {
        Plaintext resultBeforeMult;
        cryptoContext->Decrypt(keyPair.secretKey, ciphertextBeforeMult, &resultBeforeMult);
        resultBeforeMult->SetLength(encodedLength);
        std::cout << "Output before bootstrapping (ct * ctB * ctB) \n\t" << resultBeforeMult << std::endl;
    }
    catch (const std::exception& ex) {
        std::cout << "Output before bootstrapping (ct * ctB * ctB) exception: " << ex.what() << std::endl;
    }
    std::cout << "Expected (x * y * y) \n\t" << expectedPtxt << std::endl;

    // Perform the bootstrapping operation. The goal is to increase the number of levels remaining
    // for HE computation.
    auto ciphertextAfterA = cryptoContext->EvalBootstrap(ciphA);
	// B也要进行bootstrap才能继续进行同态乘法，否则会解密失败
    auto ciphertextAfterB = cryptoContext->EvalBootstrap(ciphB);


    std::cout << "Number of levels remaining after bootstrapping: "
              << depth - ciphertextAfterA->GetLevel() - (ciphertextAfterA->GetNoiseScaleDeg() - 1) << std::endl
              << std::endl;

    Plaintext result;
    cryptoContext->Decrypt(keyPair.secretKey, ciphertextAfterA, &result);
    result->SetLength(encodedLength);
    std::cout << "Output after bootstrapping \n\t" << result << std::endl;

    // 中文说明：bootstrap 后再次进行同态乘法，结果应接近期望值
    auto ciphertextAfterMult = cryptoContext->EvalMult(ciphertextAfterA, ciphertextAfterB);
    ciphertextAfterMult = cryptoContext->EvalMult(ciphertextAfterMult, ciphertextAfterB);
    try {
        Plaintext resultAfterMult;
        cryptoContext->Decrypt(keyPair.secretKey, ciphertextAfterMult, &resultAfterMult);
        resultAfterMult->SetLength(encodedLength);
        std::cout << "Output after bootstrapping (ct * ctB * ctB) \n\t" << resultAfterMult << std::endl;
    }
    catch (const std::exception& ex) {
        std::cout << "Output after bootstrapping (ct * ctB * ctB) exception: " << ex.what() << std::endl;
    }
    std::cout << "Expected (x * y * y) \n\t" << expectedPtxt << std::endl;
}

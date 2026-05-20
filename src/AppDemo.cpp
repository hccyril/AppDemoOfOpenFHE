#include "../include/common.h"

#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

// Forward declaration of experiment classes
void runtest1();
void runtest2();

namespace {

bool IsValidOutputFileName(const std::string& fileName) {
    if (fileName.empty()) {
        return false;
    }

    for (unsigned char ch : fileName) {
        if (std::iscntrl(ch)) {
            return false;
        }
    }

    static const std::string invalidChars = "<>:\"/\\|?*";
    return fileName.find_first_of(invalidChars) == std::string::npos;
}

void ParseCommandLine(int argc, char* argv[], std::optional<int>& testId, std::optional<std::string>& outputFile) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-t") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for -t option." << std::endl;
                continue;
            }

            std::string value = argv[++i];
            try {
                testId = std::stoi(value);
            }
            catch (...) {
                std::cerr << "Invalid -t value: " << value << std::endl;
            }
        }
        else if (arg == "-o") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for -o option." << std::endl;
                continue;
            }

            std::string value = argv[++i];
            if (!IsValidOutputFileName(value)) {
                std::cerr << "Invalid output file name: " << value << std::endl;
                continue;
            }

            if (std::filesystem::exists(value)) {
                std::cerr << "Output file already exists: " << value << std::endl;
                continue;
            }

            outputFile = value;
        }
    }
}

}

/**
 * @brief Main function - FHE Evaluation System entry point
 * 
 */
int main(int argc, char* argv[]) {

    std::optional<int> commandLineTestId;
    std::optional<std::string> outputFileName;
    ParseCommandLine(argc, argv, commandLineTestId, outputFileName);

    std::ofstream outputFileStream;
    std::streambuf* oldCoutBuffer = nullptr;
    if (outputFileName.has_value()) {
        outputFileStream.open(*outputFileName, std::ios::out | std::ios::trunc);
        if (outputFileStream.is_open()) {
            oldCoutBuffer = std::cout.rdbuf(outputFileStream.rdbuf());
        }
        else {
            std::cerr << "Failed to open output file: " << *outputFileName << std::endl;
        }
    }

    std::cout << "========================================" << std::endl;
    std::cout << "FHE Evaluation System" << std::endl;
    std::cout << "========================================" << std::endl;

    int t = 1;
#ifndef NDEBUG
    // Specify target test ID here directly during debugging
    constexpr bool kIsDebugBuild = true;
    constexpr int kDebugDefaultTestId = 7;
#else
    constexpr bool kIsDebugBuild = false;
#endif

    if (kIsDebugBuild || commandLineTestId.has_value()) {
        t = commandLineTestId.value_or(
#ifndef NDEBUG
            kDebugDefaultTestId
#else
            1
#endif
        );
    }
    else {
        std::cout << "Select test to run:" << std::endl;
        std::cout << "  1: Lab Stage A (PolyDecmp/Prune basics)" << std::endl;
        std::cout << "  2: Lab Stage B (MV-CC)" << std::endl;
        std::cout << "  3: Lab Stage C (MM-CC)" << std::endl;
        std::cout << "  4: Lab C TimeCheck MMCC" << std::endl;
        std::cout << "  5: Base Operator Time Test" << std::endl;
        std::cout << "  6: BCHP Paper C-MT Algorithm (Small Data)" << std::endl;
        std::cout << "  7: Lab Stage C Large (Blocked MM-CC 64x64 * 64x32)" << std::endl;
        std::cout << "Enter test id (1-8, default=1): ";
        if (!(std::cin >> t)) {
            std::cerr << "Invalid input, using default (1)" << std::endl;
            t = 1;
        }
    }

    int exitCode = 0;
    const auto startTime = std::chrono::steady_clock::now();

    switch(t) {
        case 1:
            std::cout << "\nRunning Test 1: Lab Stage A" << std::endl;
            fhe_eval::RunLabADemo();
            break;
        case 2:
            std::cout << "\nRunning Test 2: Lab Stage B" << std::endl;
            fhe_eval::RunLabBDemo();
            break;
        case 3:
            std::cout << "\nRunning Test 3: Lab Stage C" << std::endl;
            fhe_eval::RunLabCDemo();
            break;
        case 4:
            std::cout << "\nRunning Test 4: Lab C TimeCheck MMCC" << std::endl;
            fhe_eval::RunTimeCheckMMCC();
			break;
        case 5:
            std::cout << "\nRunning Test 5: Base Operator Time Test" << std::endl;
            fhe_eval::RunBaseOperatorTimeTestDemo();
            break;
        case 6:
            std::cout << "\nRunning Test 5: BCHP Paper C-MT" << std::endl;
            fhe_eval::bchp_test_run();
            break;
        case 7:
            std::cout << "\nRunning Test 7: Lab Stage C Large (Blocked MM-CC)" << std::endl;
            fhe_eval::RunLabCLargeDemo();
            break;
        default:
            std::cerr << "Unknown test id: " << t << std::endl;
            exitCode = 2;
            break;
    }

    const auto endTime = std::chrono::steady_clock::now();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

    if (exitCode == 0) {
        std::cout << "\n========================================" << std::endl;
        std::cout << "Test completed successfully! RunTime = " << elapsedMs << " ms" << std::endl;
        std::cout << "========================================" << std::endl;
    }
    else {
		std::cerr << "\nTest ERROR! exitCode=" << exitCode << std::endl;
    }

    if (oldCoutBuffer != nullptr) {
        std::cout.rdbuf(oldCoutBuffer);
    }

    return exitCode;
}

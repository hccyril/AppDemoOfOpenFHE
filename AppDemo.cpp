#include <iostream>

// Forward declarations of the two test runners
void runtest1();
void runtest2();

int main() {
    //std::cout << "Enter test id (1 or 2): ";
    //int t; if(!(std::cin >> t)) { std::cerr << "Invalid input" << std::endl; return 1; }
    int t = 1;
    if (t == 1) {
        runtest1();
    } else if (t == 2) {
        runtest2();
    } else {
        std::cerr << "Unknown test id: " << t << std::endl;
        return 2;
    }
    return 0;
}

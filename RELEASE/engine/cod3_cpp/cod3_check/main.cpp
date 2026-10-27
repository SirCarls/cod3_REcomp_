// cod3_check_cpp: answers tools/decomp/decomp.py check with the C++ version,
// one request per line on stdin, one reply per line on stdout (protocol.h).
// [acct2-decomp]
#include <iostream>
#include <string>

#include "cod3_check/protocol.h"

int main() {
    std::ios::sync_with_stdio(false);
    std::string line;
    while (std::getline(std::cin, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        std::cout << cod3::check::answer(line) << '\n' << std::flush;
        if (!std::cout) {
            break;
        }
    }
    return 0;
}

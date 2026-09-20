#include "test_support.h"

#include <exception>
#include <iostream>

// Flush after every line rather than only at exit: a crashing test would
// otherwise take its whole pass/fail history down with the stdout buffer,
// which is exactly when that history matters most.
int main() {
    int failures = 0;

    for (const auto& test : test_support::registry()) {
        try {
            test.function();
            std::cout << "PASS " << test.name << std::endl;
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "FAIL " << test.name << ": " << error.what() << std::endl;
        } catch (...) {
            ++failures;
            std::cout << "FAIL " << test.name << ": unknown exception" << std::endl;
        }
    }

    return failures == 0 ? 0 : 1;
}

#include "test_support.h"

#include <exception>
#include <iostream>

int main() {
    int failures = 0;

    for (const auto& test : test_support::registry()) {
        try {
            test.function();
            std::cout << "PASS " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "FAIL " << test.name << ": " << error.what() << '\n';
        } catch (...) {
            ++failures;
            std::cout << "FAIL " << test.name << ": unknown exception\n";
        }
    }

    return failures == 0 ? 0 : 1;
}

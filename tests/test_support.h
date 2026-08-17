#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace test_support {

using TestFunction = std::function<void()>;

struct TestCase {
    std::string name;
    TestFunction function;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

class Registrar {
public:
    Registrar(std::string name, TestFunction function) {
        registry().push_back({std::move(name), std::move(function)});
    }
};

inline void require(bool condition, const char* expression, const char* file, int line) {
    if (!condition) {
        throw std::runtime_error(std::string(file) + ":" + std::to_string(line) +
                                 ": requirement failed: " + expression);
    }
}

}  // namespace test_support

namespace test {

class ScopedTempDir {
public:
    explicit ScopedTempDir(const std::filesystem::path& prefix) {
        static std::atomic<unsigned long long> counter{0};
        const auto stamp = std::chrono::high_resolution_clock::now()
                               .time_since_epoch()
                               .count();
        path_ = std::filesystem::temp_directory_path() / prefix;
        path_ += std::filesystem::path(
            "-" + std::to_string(stamp) + "-" +
            std::to_string(counter.fetch_add(1, std::memory_order_relaxed)));
        if (!std::filesystem::create_directory(path_)) {
            throw std::runtime_error("failed to create unique temporary directory");
        }
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    ~ScopedTempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const noexcept {
        return path_;
    }

    std::filesystem::path write_text(const std::filesystem::path& relative,
                                     const std::string& content) const {
        if (relative.empty() || relative.is_absolute()) {
            throw std::invalid_argument("temporary file path must be relative");
        }
        const auto file = path_ / relative;
        std::filesystem::create_directories(file.parent_path());
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        output.flush();
        if (!output) {
            throw std::runtime_error("failed to write temporary file");
        }
        return file;
    }

private:
    std::filesystem::path path_;
};

}  // namespace test

#define TEST_CASE(name) \
    static void name(); \
    static const test_support::Registrar name##_registrar(#name, name); \
    static void name()

#define REQUIRE(expression) \
    test_support::require(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

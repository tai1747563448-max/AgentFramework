#include <nlohmann/json.hpp>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string stdin_bytes() {
    return {std::istreambuf_iterator<char>(std::cin),
            std::istreambuf_iterator<char>()};
}

bool environment_has(const char* name) {
    return std::getenv(name) != nullptr;
}

#if defined(_WIN32)
std::wstring quote_windows(const std::wstring& value) {
    std::wstring quoted(1, L'"');
    std::size_t slashes = 0;
    for (const auto character : value) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        if (character == L'"') {
            quoted.append(slashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
            slashes = 0;
            continue;
        }
        quoted.append(slashes, L'\\');
        slashes = 0;
        quoted.push_back(character);
    }
    quoted.append(slashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

bool spawn_child(const std::filesystem::path& executable,
                 const std::filesystem::path& ready_marker,
                 const std::filesystem::path& survival_marker,
                 const std::string& delay) {
    auto command = quote_windows(executable.native()) + L" tree-child " +
                   quote_windows(ready_marker.native()) + L" " +
                   quote_windows(survival_marker.native()) + L" " +
                   std::wstring(delay.begin(), delay.end());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        executable.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (created == FALSE) {
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}
#else
bool spawn_child(const std::filesystem::path& executable,
                 const std::filesystem::path& ready_marker,
                 const std::filesystem::path& survival_marker,
                 const std::string& delay) {
    const pid_t child = fork();
    if (child < 0) {
        return false;
    }
    if (child == 0) {
        execl(executable.c_str(), executable.c_str(), "tree-child",
              ready_marker.c_str(), survival_marker.c_str(), delay.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }
    return true;
}
#endif

}  // namespace

int fixture_main(const std::vector<std::string>& arguments) {
    if (arguments.size() < 2) {
        return 64;
    }
    const std::string& mode = arguments[1];
    if (mode == "inspect") {
        const auto input = stdin_bytes();
        nlohmann::json received = nlohmann::json::array();
        for (std::size_t index = 2; index < arguments.size(); ++index) {
            received.push_back(arguments[index]);
        }
        nlohmann::json output{
            {"cwd", std::filesystem::current_path().generic_u8string()},
            {"arguments", std::move(received)},
            {"stdin", input},
            {"environment",
             {{"AGENT_API_KEY", environment_has("AGENT_API_KEY")},
              {"AGENT_AUTH_TOKEN", environment_has("AGENT_AUTH_TOKEN")},
              {"SAMPLE_SECRET", environment_has("SAMPLE_SECRET")},
              {"PATH", environment_has("PATH")}}}};
        std::cout << output.dump();
        return 0;
    }
    if (mode == "spam" && arguments.size() == 5) {
        const auto stdout_size = std::stoull(arguments[2]);
        const auto stderr_size = std::stoull(arguments[3]);
        const int exit_code = std::stoi(arguments[4]);
        std::thread stdout_writer([&] {
            for (std::size_t index = 0; index < stdout_size; ++index) {
                std::cout.put(index == 0 ? 'H' :
                              (index + 1 == stdout_size ? 'T' : 'o'));
            }
            std::cout.flush();
        });
        std::thread stderr_writer([&] {
            for (std::size_t index = 0; index < stderr_size; ++index) {
                std::cerr.put(index == 0 ? 'H' :
                              (index + 1 == stderr_size ? 'T' : 'e'));
            }
            std::cerr.flush();
        });
        stdout_writer.join();
        stderr_writer.join();
        return exit_code;
    }
    if (mode == "raw-invalid" && arguments.size() == 2) {
        constexpr char invalid[] = {static_cast<char>(0xFF),
                                    static_cast<char>(0xFF),
                                    static_cast<char>(0xFF)};
        std::cout.write(invalid, sizeof(invalid));
        std::cout.flush();
        return 0;
    }
    if (mode == "raw-mixed" && arguments.size() == 2) {
        const std::string output =
            std::string(u8"前🙂") + static_cast<char>(0xFF) + u8"后";
        std::cout.write(output.data(),
                        static_cast<std::streamsize>(output.size()));
        std::cout.flush();
        return 0;
    }
    if (mode == "raw-utf8-boundary" && arguments.size() == 2) {
        constexpr const char* output = u8"🙂🙂";
        std::cout.write(output, static_cast<std::streamsize>(std::strlen(output)));
        std::cout.flush();
        return 0;
    }
    if (mode == "tree-child" && arguments.size() == 5) {
        std::ofstream(std::filesystem::u8path(arguments[2]),
                      std::ios::binary | std::ios::trunc)
            << "ready";
        std::this_thread::sleep_for(
            std::chrono::milliseconds(std::stoll(arguments[4])));
        std::ofstream(std::filesystem::u8path(arguments[3]),
                      std::ios::binary | std::ios::trunc)
            << "alive";
        return 0;
    }
    if (mode == "tree-parent" && arguments.size() == 5) {
        if (!spawn_child(
                std::filesystem::absolute(
                    std::filesystem::u8path(arguments[0])),
                std::filesystem::u8path(arguments[2]),
                std::filesystem::u8path(arguments[3]), arguments[4])) {
            return 70;
        }
        std::this_thread::sleep_for(std::chrono::seconds(10));
        return 0;
    }
    return 65;
}

#if defined(_WIN32)
int wmain(int argc, wchar_t* argv[]) {
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        const auto length = static_cast<int>(wcslen(argv[index]));
        const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                              argv[index], length, nullptr, 0,
                                              nullptr, nullptr);
        if (bytes < 0) {
            return 66;
        }
        std::string converted(static_cast<std::size_t>(bytes), '\0');
        if (bytes != 0 &&
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[index],
                                length, converted.data(), bytes, nullptr,
                                nullptr) != bytes) {
            return 66;
        }
        arguments.push_back(std::move(converted));
    }
    return fixture_main(arguments);
}
#else
int main(int argc, char* argv[]) {
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }
    return fixture_main(arguments);
}
#endif

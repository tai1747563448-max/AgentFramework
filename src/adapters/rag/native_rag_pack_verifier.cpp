#include "adapters/rag/native_rag_pack_verifier.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

namespace agent {
namespace {

constexpr std::uintmax_t kMaximumPackManifestBytes =
    32U * 1024U * 1024U;
constexpr std::uintmax_t kMaximumRuntimeLockBytes = 16U * 1024U * 1024U;
constexpr auto kProgressInterval = std::chrono::seconds(5);

struct FileRecord {
    std::string path;
    std::uintmax_t bytes{0};
    std::string sha256;
};

constexpr std::array<std::uint32_t, 64> kSha256Constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

std::uint32_t rotate_right(std::uint32_t value, unsigned int count) {
    return (value >> count) | (value << (32U - count));
}

class StreamingSha256 final {
public:
    void update(const std::uint8_t* data, std::size_t size) {
        total_bytes_ += static_cast<std::uint64_t>(size);
        while (size != 0) {
            const auto count = std::min(size, buffer_.size() - buffered_);
            std::copy_n(data, count, buffer_.begin() +
                                         static_cast<std::ptrdiff_t>(buffered_));
            buffered_ += count;
            data += count;
            size -= count;
            if (buffered_ == buffer_.size()) {
                transform(buffer_.data());
                buffered_ = 0;
            }
        }
    }

    std::string finish() {
        const auto bit_length = total_bytes_ * 8U;
        buffer_[buffered_++] = 0x80U;
        if (buffered_ > 56U) {
            std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_),
                      buffer_.end(), std::uint8_t{0});
            transform(buffer_.data());
            buffered_ = 0;
        }
        std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_),
                  buffer_.begin() + 56, std::uint8_t{0});
        for (std::size_t index = 0; index < 8U; ++index) {
            buffer_[56U + index] = static_cast<std::uint8_t>(
                bit_length >> ((7U - index) * 8U));
        }
        transform(buffer_.data());

        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (const auto word : hash_) {
            output << std::setw(8) << word;
        }
        return output.str();
    }

private:
    void transform(const std::uint8_t* data) {
        std::array<std::uint32_t, 64> schedule{};
        for (std::size_t index = 0; index < 16U; ++index) {
            const auto offset = index * 4U;
            schedule[index] =
                (static_cast<std::uint32_t>(data[offset]) << 24U) |
                (static_cast<std::uint32_t>(data[offset + 1]) << 16U) |
                (static_cast<std::uint32_t>(data[offset + 2]) << 8U) |
                static_cast<std::uint32_t>(data[offset + 3]);
        }
        for (std::size_t index = 16U; index < schedule.size(); ++index) {
            const auto previous = schedule[index - 15U];
            const auto earlier = schedule[index - 2U];
            const auto sigma0 = rotate_right(previous, 7U) ^
                                rotate_right(previous, 18U) ^
                                (previous >> 3U);
            const auto sigma1 = rotate_right(earlier, 17U) ^
                                rotate_right(earlier, 19U) ^
                                (earlier >> 10U);
            schedule[index] = schedule[index - 16U] + sigma0 +
                              schedule[index - 7U] + sigma1;
        }

        auto a = hash_[0];
        auto b = hash_[1];
        auto c = hash_[2];
        auto d = hash_[3];
        auto e = hash_[4];
        auto f = hash_[5];
        auto g = hash_[6];
        auto h = hash_[7];
        for (std::size_t index = 0; index < schedule.size(); ++index) {
            const auto big_sigma1 = rotate_right(e, 6U) ^
                                    rotate_right(e, 11U) ^
                                    rotate_right(e, 25U);
            const auto choice = (e & f) ^ ((~e) & g);
            const auto temporary1 = h + big_sigma1 + choice +
                                    kSha256Constants[index] + schedule[index];
            const auto big_sigma0 = rotate_right(a, 2U) ^
                                    rotate_right(a, 13U) ^
                                    rotate_right(a, 22U);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temporary2 = big_sigma0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        hash_[0] += a;
        hash_[1] += b;
        hash_[2] += c;
        hash_[3] += d;
        hash_[4] += e;
        hash_[5] += f;
        hash_[6] += g;
        hash_[7] += h;
    }

    std::array<std::uint32_t, 8> hash_{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_{0};
    std::uint64_t total_bytes_{0};
};

bool path_is_link_or_reparse(const std::filesystem::path& path,
                             std::error_code& error) noexcept {
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || std::filesystem::is_symlink(status)) {
        return true;
    }
#if defined(_WIN32)
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        error = std::error_code(static_cast<int>(GetLastError()),
                                std::system_category());
        return true;
    }
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return false;
#endif
}

bool trusted_components(const std::filesystem::path& path) noexcept {
    try {
        std::error_code error;
        auto current = path.root_path();
        for (const auto& component : path.relative_path()) {
            current /= component;
            if (path_is_link_or_reparse(current, error) || error) {
                return false;
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<nlohmann::json> read_strict_json(
    const std::filesystem::path& path, std::uintmax_t maximum_bytes) {
    try {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error) || error ||
            path_is_link_or_reparse(path, error) || error ||
            std::filesystem::hard_link_count(path, error) != 1 || error ||
            std::filesystem::file_size(path, error) > maximum_bytes || error) {
            return std::nullopt;
        }
        std::ifstream input(path, std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>()};
        if ((!input.good() && !input.eof()) || bytes.empty()) {
            return std::nullopt;
        }
        bool duplicate = false;
        std::vector<std::set<std::string>> keys;
        const nlohmann::json::parser_callback_t callback =
            [&](int depth, nlohmann::json::parse_event_t event,
                nlohmann::json& parsed) {
                if (event == nlohmann::json::parse_event_t::object_start) {
                    const auto index = static_cast<std::size_t>(depth + 1);
                    if (keys.size() <= index) keys.resize(index + 1);
                    keys[index].clear();
                } else if (event == nlohmann::json::parse_event_t::key) {
                    const auto index = static_cast<std::size_t>(depth);
                    if (keys.size() <= index) keys.resize(index + 1);
                    if (!keys[index].insert(parsed.get<std::string>()).second) {
                        duplicate = true;
                    }
                }
                return true;
            };
        auto value = nlohmann::json::parse(bytes, callback, true, false);
        if (value.is_discarded() || duplicate) return std::nullopt;
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

bool exact_keys(const nlohmann::json& value,
                std::initializer_list<const char*> keys) {
    if (!value.is_object() || value.size() != keys.size()) return false;
    return std::all_of(keys.begin(), keys.end(), [&](const char* key) {
        return value.find(key) != value.end();
    });
}

bool lower_sha256(const std::string& value) {
    return value.size() == 64U &&
           std::all_of(value.begin(), value.end(), [](unsigned char byte) {
               return (byte >= '0' && byte <= '9') ||
                      (byte >= 'a' && byte <= 'f');
           });
}

bool safe_relative_path(const std::string& value) {
    if (value.empty() || value.size() > 512U || value.find('\\') != std::string::npos ||
        value.find(':') != std::string::npos || value.find('\0') != std::string::npos) {
        return false;
    }
    const auto path = std::filesystem::u8path(value);
    if (path.is_absolute() || path.generic_u8string() != value) return false;
    for (const auto& component : path) {
        if (component == "." || component == ".." || component.empty()) {
            return false;
        }
    }
    return true;
}

std::optional<std::vector<FileRecord>> parse_records(
    const nlohmann::json& value) {
    if (!value.is_array()) return std::nullopt;
    std::set<std::string> seen;
    std::vector<FileRecord> records;
    records.reserve(value.size());
    for (const auto& item : value) {
        if (!exact_keys(item, {"path", "bytes", "sha256"}) ||
            !item.at("path").is_string() ||
            !item.at("bytes").is_number_unsigned() ||
            !item.at("sha256").is_string()) {
            return std::nullopt;
        }
        FileRecord record{item.at("path").get<std::string>(),
                          item.at("bytes").get<std::uintmax_t>(),
                          item.at("sha256").get<std::string>()};
        if (!safe_relative_path(record.path) || !lower_sha256(record.sha256) ||
            !seen.insert(record.path).second) {
            return std::nullopt;
        }
        records.push_back(std::move(record));
    }
    return records;
}

std::optional<std::string> sha256_file(const std::filesystem::path& path) {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) return std::nullopt;
        StreamingSha256 digest;
        std::vector<char> buffer(1024U * 1024U);
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if (count > 0) {
                digest.update(reinterpret_cast<const std::uint8_t*>(buffer.data()),
                              static_cast<std::size_t>(count));
            }
        }
        if (!input.eof()) return std::nullopt;
        return digest.finish();
    } catch (...) {
        return std::nullopt;
    }
}

bool verify_record(const std::filesystem::path& root,
                   const FileRecord& record) {
    try {
        const auto path = root / std::filesystem::u8path(record.path);
        std::error_code error;
        if (!trusted_components(path) ||
            !std::filesystem::is_regular_file(path, error) || error ||
            path_is_link_or_reparse(path, error) || error ||
            std::filesystem::hard_link_count(path, error) != 1 || error ||
            std::filesystem::file_size(path, error) != record.bytes || error) {
            return false;
        }
        const auto digest = sha256_file(path);
        return digest.has_value() && *digest == record.sha256;
    } catch (...) {
        return false;
    }
}

bool hold_read_lock(const std::filesystem::path& path,
                    std::vector<std::uintptr_t>& handles) {
#if defined(_WIN32)
    const HANDLE handle = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
        info.nNumberOfLinks != 1) {
        CloseHandle(handle);
        return false;
    }
    handles.push_back(reinterpret_cast<std::uintptr_t>(handle));
#else
    (void)path;
    (void)handles;
#endif
    return true;
}

std::set<std::string> expected_directories(
    const std::vector<FileRecord>& records) {
    std::set<std::string> result;
    for (const auto& record : records) {
        auto parent = std::filesystem::u8path(record.path).parent_path();
        while (!parent.empty()) {
            result.insert(parent.generic_u8string());
            parent = parent.parent_path();
        }
    }
    return result;
}

bool inventory_tree(const std::filesystem::path& root,
                    const std::filesystem::path& subtree,
                    std::set<std::string>& files,
                    std::set<std::string>& directories) {
    try {
        const auto base = root / subtree;
        std::error_code error;
        if (!std::filesystem::is_directory(base, error) || error ||
            path_is_link_or_reparse(base, error) || error) {
            return false;
        }
        directories.insert(subtree.generic_u8string());
        std::filesystem::recursive_directory_iterator iterator(base, error);
        const std::filesystem::recursive_directory_iterator end;
        while (!error && iterator != end) {
            const auto path = iterator->path();
            if (path_is_link_or_reparse(path, error) || error) return false;
            const auto relative = path.lexically_relative(root).generic_u8string();
            if (iterator->is_directory(error) && !error) {
                directories.insert(relative);
            } else if (iterator->is_regular_file(error) && !error) {
                if (std::filesystem::hard_link_count(path, error) != 1 || error) {
                    return false;
                }
                files.insert(relative);
            } else {
                return false;
            }
            iterator.increment(error);
        }
        return !error;
    } catch (...) {
        return false;
    }
}

RuntimeError integrity_error() {
    return {ErrorCode::DependencyUnavailable,
            "rag executable payload integrity check failed", false};
}

}  // namespace

NativeRagPackVerifier::NativeRagPackVerifier(
    std::function<void(const std::string&)> progress_observer,
    std::string trusted_manifest_sha256)
    : progress_observer_(std::move(progress_observer)),
      trusted_manifest_sha256_(std::move(trusted_manifest_sha256)) {}

NativeRagPackVerifier::~NativeRagPackVerifier() { release_locks(); }

void NativeRagPackVerifier::release_locks() noexcept {
#if defined(_WIN32)
    for (const auto value : locked_handles_) {
        if (value != 0) CloseHandle(reinterpret_cast<HANDLE>(value));
    }
#endif
    locked_handles_.clear();
}

Result<void> NativeRagPackVerifier::verify_executable_payload(
    const std::filesystem::path& pack_root) {
    release_locks();
    std::vector<std::uintptr_t> pending_handles;
    std::size_t completed = 0;
    std::size_t total = 1;
    const auto started = std::chrono::steady_clock::now();
    auto last_emitted = started;
    auto emit = [&](const char* status, bool force) {
        if (!progress_observer_) return;
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - last_emitted < kProgressInterval) return;
        const auto elapsed =
            std::chrono::duration<double>(now - started).count();
        const auto throughput = elapsed > 0.0
                                    ? static_cast<double>(completed) / elapsed
                                    : 0.0;
        const auto percent = total != 0
                                 ? static_cast<double>(completed) * 100.0 /
                                       static_cast<double>(total)
                                 : 0.0;
        std::ostringstream message;
        message << std::fixed << std::setprecision(1)
                << "RAG bootstrap-integrity " << status << ": " << completed
                << '/' << total << " (" << percent << "%), elapsed "
                << elapsed << "s, throughput " << throughput << " files/s";
        if (throughput > 0.0 && completed < total &&
            std::string(status) != "failed") {
            message << ", ETA "
                    << static_cast<double>(total - completed) / throughput
                    << 's';
        }
        try {
            progress_observer_(message.str());
        } catch (...) {
            progress_observer_ = nullptr;
        }
        last_emitted = now;
    };
    auto close_pending = [&]() noexcept {
#if defined(_WIN32)
        for (const auto value : pending_handles) {
            if (value != 0) CloseHandle(reinterpret_cast<HANDLE>(value));
        }
#endif
        pending_handles.clear();
    };
    auto fail = [&]() {
        close_pending();
        emit("failed", true);
        return Result<void>::failure(integrity_error());
    };

    try {
        if (pack_root.empty() || !pack_root.is_absolute() ||
            !trusted_components(pack_root)) {
            return fail();
        }
        std::error_code error;
        const auto root = std::filesystem::canonical(pack_root, error);
        if (error || !std::filesystem::is_directory(root, error) || error ||
            !trusted_components(root)) {
            return fail();
        }
        const auto manifest_path = root / "pack.json";
        if (!lower_sha256(trusted_manifest_sha256_) ||
            !hold_read_lock(manifest_path, pending_handles)) {
            return fail();
        }
        const auto manifest_digest = sha256_file(manifest_path);
        const auto manifest =
            read_strict_json(manifest_path, kMaximumPackManifestBytes);
        if (!manifest_digest.has_value() ||
            *manifest_digest != trusted_manifest_sha256_ ||
            !manifest.has_value() ||
            !exact_keys(*manifest,
                        {"schema_version", "pack_id", "snapshot_date",
                         "document_count", "chunk_count", "embedding_model",
                         "embedding_revision", "embedding_dimensions",
                         "relevance_dense_min", "complete", "files"}) ||
            !manifest->at("schema_version").is_number_integer() ||
            manifest->at("schema_version").get<std::int64_t>() != 2 ||
            !manifest->at("complete").is_boolean() ||
            !manifest->at("complete").get<bool>()) {
            return fail();
        }
        const auto manifest_records = parse_records(manifest->at("files"));
        if (!manifest_records.has_value()) return fail();
        std::map<std::string, FileRecord> manifest_by_path;
        for (const auto& record : *manifest_records) {
            manifest_by_path.emplace(record.path, record);
        }
        const auto lock_record = manifest_by_path.find("runtime.lock.json");
        if (lock_record == manifest_by_path.end() ||
            !hold_read_lock(root / "runtime.lock.json", pending_handles) ||
            !verify_record(root, lock_record->second)) {
            return fail();
        }
        const auto lock = read_strict_json(
            root / "runtime.lock.json", kMaximumRuntimeLockBytes);
        if (!lock.has_value() ||
            !exact_keys(*lock, {"schema_version", "intent_sha256", "python",
                                "requirements_sha256", "wheels", "files"}) ||
            !lock->at("schema_version").is_number_integer() ||
            lock->at("schema_version").get<std::int64_t>() != 2 ||
            !lock->at("intent_sha256").is_string() ||
            !lower_sha256(lock->at("intent_sha256").get<std::string>()) ||
            !lock->at("requirements_sha256").is_string() ||
            !lower_sha256(lock->at("requirements_sha256").get<std::string>()) ||
            !lock->at("python").is_object() || !lock->at("wheels").is_array()) {
            return fail();
        }
        const auto runtime_records = parse_records(lock->at("files"));
        if (!runtime_records.has_value() || runtime_records->empty()) {
            return fail();
        }
        for (const auto& record : *runtime_records) {
            const auto found = manifest_by_path.find(record.path);
            if (record.path.rfind("runtime/", 0) != 0 ||
                found == manifest_by_path.end() ||
                found->second.bytes != record.bytes ||
                found->second.sha256 != record.sha256) {
                return fail();
            }
        }
        std::vector<FileRecord> sidecar_records;
        for (const auto& record : *manifest_records) {
            if (record.path.rfind("sidecar/", 0) == 0) {
                sidecar_records.push_back(record);
            }
        }
        if (sidecar_records.empty() ||
            manifest_by_path.find("runtime/python.exe") == manifest_by_path.end() ||
            manifest_by_path.find("sidecar/agent_rag_cli.py") ==
                manifest_by_path.end()) {
            return fail();
        }
        const auto intent_record = manifest_by_path.find("build.intent.json");
        if (intent_record == manifest_by_path.end() ||
            !hold_read_lock(root / "build.intent.json", pending_handles) ||
            !verify_record(root, intent_record->second)) {
            return fail();
        }

        std::vector<FileRecord> executable_records;
        executable_records.reserve(runtime_records->size() +
                                   sidecar_records.size());
        executable_records.insert(executable_records.end(),
                                  runtime_records->begin(),
                                  runtime_records->end());
        executable_records.insert(executable_records.end(),
                                  sidecar_records.begin(),
                                  sidecar_records.end());
        std::set<std::string> actual_files;
        std::set<std::string> actual_directories;
        if (!inventory_tree(root, "runtime", actual_files, actual_directories) ||
            !inventory_tree(root, "sidecar", actual_files, actual_directories)) {
            return fail();
        }
        std::set<std::string> expected_files;
        for (const auto& record : executable_records) {
            expected_files.insert(record.path);
        }
        if (actual_files != expected_files ||
            actual_directories != expected_directories(executable_records)) {
            return fail();
        }

        total = executable_records.size();
        emit("running", true);
        for (const auto& record : executable_records) {
            if (!hold_read_lock(root / std::filesystem::u8path(record.path),
                                pending_handles) ||
                !verify_record(root, record)) {
                return fail();
            }
            ++completed;
            emit(completed == total ? "completed" : "running",
                 completed == total);
        }
        locked_handles_ = std::move(pending_handles);
        return Result<void>::success();
    } catch (...) {
        return fail();
    }
}

}  // namespace agent

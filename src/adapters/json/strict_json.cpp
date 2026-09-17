#include "adapters/json/strict_json.h"

#include <algorithm>
#include <fstream>
#include <istream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace agent {

StrictJsonResult StrictJsonResult::success(nlohmann::json value) {
    return StrictJsonResult{std::move(value), std::nullopt};
}

StrictJsonResult StrictJsonResult::failure(StrictJsonError error) {
    return StrictJsonResult{std::nullopt, std::move(error)};
}

namespace {

// The duplicate-key detector. We only ever insert and erase here; nothing
// else touches the per-depth sets during the SAX pass, so the container is
// thread-safe for the single-writer streaming parser nlohmann drives.
class DuplicateKeyDetector final : public nlohmann::json::json_sax_t {
public:
    bool null() override { return true; }
    bool boolean(bool) override { return true; }
    bool number_integer(std::int64_t) override { return true; }
    bool number_unsigned(std::uint64_t) override { return true; }
    bool number_float(double, const std::string& raw) override {
        // Reject leading zeros in integers (e.g. ``01``); allow the rest.
        return reject_leading_zero(raw);
    }
    bool string(std::string&) override { return true; }
    bool binary(binary_t&) override { return true; }
    bool start_object(std::size_t) override {
        if (depth_ + 1 > kStrictJsonMaxDepth) {
            fail(StrictJsonError::Kind::DepthExceeded,
                 "JSON nesting exceeded the configured maximum");
            return false;
        }
        keys_.emplace_back();
        ++depth_;
        return true;
    }
    bool key(std::string& key) override {
        auto& slot = keys_.back();
        if (!slot.insert(key).second) {
            fail(StrictJsonError::Kind::DuplicateKey,
                 "duplicate JSON object key: " + key);
            return false;
        }
        return true;
    }
    bool end_object() override {
        if (keys_.empty()) {
            fail(StrictJsonError::Kind::InvalidSyntax,
                 "JSON end_object without matching start_object");
            return false;
        }
        keys_.pop_back();
        --depth_;
        return true;
    }
    bool start_array(std::size_t) override {
        if (depth_ + 1 > kStrictJsonMaxDepth) {
            fail(StrictJsonError::Kind::DepthExceeded,
                 "JSON nesting exceeded the configured maximum");
            return false;
        }
        // Arrays do not own keys; we still need to balance the depth counter.
        keys_.emplace_back();
        ++depth_;
        return true;
    }
    bool end_array() override {
        if (keys_.empty()) {
            fail(StrictJsonError::Kind::InvalidSyntax,
                 "JSON end_array without matching start_array");
            return false;
        }
        keys_.pop_back();
        --depth_;
        return true;
    }
    bool parse_error(std::size_t position,
                     const std::string& last_token,
                     const nlohmann::detail::exception& ex) override {
        StrictJsonError error{};
        error.position = position;
        if (ex.what() && std::string_view(ex.what()).find("trailing") != std::string_view::npos) {
            error.kind = StrictJsonError::Kind::TrailingContent;
            error.message = "trailing JSON content is not allowed";
        } else if (last_token.empty()) {
            error.kind = StrictJsonError::Kind::InvalidSyntax;
            error.message = ex.what() ? ex.what() : "JSON parse error";
        } else {
            error.kind = StrictJsonError::Kind::InvalidSyntax;
            error.message = "JSON parse error near '" + last_token + "'";
        }
        failure_ = std::move(error);
        return false;
    }
    const std::optional<StrictJsonError>& error() const { return failure_; }

private:
    void fail(StrictJsonError::Kind kind, std::string message) {
        if (failure_.has_value()) return;
        failure_ = StrictJsonError{kind, 0, std::move(message)};
    }
    bool reject_leading_zero(const std::string& raw) const {
        if (raw.size() <= 1) return true;
        if (raw.front() != '0' && raw.front() != '-') return true;
        if (raw.front() == '-' && raw.size() == 2) return true;
        // ``0.x`` style fractions are accepted; ``00``, ``01`` etc. are not.
        if (raw.front() == '-') {
            return raw[1] != '0';
        }
        return raw[1] == '.';
    }

    std::vector<std::set<std::string>> keys_;
    std::size_t depth_{0};
    std::optional<StrictJsonError> failure_;
};

}  // namespace

StrictJsonResult parse_strict_json(std::string_view bytes,
                                   std::size_t maximum_bytes) {
    if (bytes.empty()) {
        return StrictJsonResult::failure(
            {StrictJsonError::Kind::Empty, 0, "JSON input was empty"});
    }
    if (bytes.size() > maximum_bytes) {
        return StrictJsonResult::failure(
            {StrictJsonError::Kind::TooLarge, 0,
             "JSON input exceeds the configured maximum size"});
    }
    DuplicateKeyDetector detector;
    const auto sax_ok = nlohmann::json::sax_parse(
        bytes.begin(), bytes.end(), &detector,
        /*format=*/nlohmann::json::input_format_t::json,
        /*strict=*/true,
        /*ignore_comments=*/false);
    if (!sax_ok) {
        if (detector.error().has_value()) {
            return StrictJsonResult::failure(detector.error().value());
        }
        return StrictJsonResult::failure(
            {StrictJsonError::Kind::InvalidSyntax, 0,
             "JSON SAX duplicate-key check failed"});
    }
    try {
        // Second phase: plain DOM parse without a callback so the
        // quadratic discarded-element scan in ``end_object`` cannot fire.
        auto value = nlohmann::json::parse(
            bytes.begin(), bytes.end(),
            /*callback=*/nullptr,
            /*allow_exceptions=*/true,
            /*ignore_comments=*/false);
        return StrictJsonResult::success(std::move(value));
    } catch (const nlohmann::json::parse_error& ex) {
        return StrictJsonResult::failure(
            {StrictJsonError::Kind::InvalidSyntax,
             static_cast<std::size_t>(ex.byte),
             ex.what() ? std::string(ex.what()) : std::string("JSON parse failed")});
    } catch (const nlohmann::json::exception& ex) {
        return StrictJsonResult::failure(
            {StrictJsonError::Kind::InvalidEncoding, 0,
             ex.what() ? std::string(ex.what()) : std::string("JSON encoding invalid")});
    }
}

StrictJsonResult parse_strict_json_file(const std::string& path,
                                        std::size_t maximum_bytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return StrictJsonResult::failure(
            {StrictJsonError::Kind::InvalidSyntax, 0,
             "JSON file could not be opened: " + path});
    }
    std::string bytes{std::istreambuf_iterator<char>(input),
                      std::istreambuf_iterator<char>()};
    if (bytes.size() > maximum_bytes) {
        return StrictJsonResult::failure(
            {StrictJsonError::Kind::TooLarge, 0,
             "JSON file exceeds the configured maximum size"});
    }
    return parse_strict_json(bytes, maximum_bytes);
}

}  // namespace agent

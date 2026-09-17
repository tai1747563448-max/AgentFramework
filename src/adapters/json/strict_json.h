#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace agent {

// Two-phase strict JSON parser.
//
// The previous single-pass ``nlohmann::json::parse(bytes, callback, ...)``
// implementation scanned the parent container on every ``end_object`` event
// while it reconciled the callback-rejected ``discarded`` elements. With the
// long eCFR pack manifests (~11 MB / 64k array entries) the scan added up to
// O(n^2) work and dominated the parser budget.
//
// This module runs the parser in two independent phases:
//   1. A pure SAX phase using ``nlohmann::json::sax_parse`` that pushes a key
//      set when entering an object, pops it when leaving, and rejects the
//      whole document if any object declares the same key twice. Depth is
//      bounded and ``strict`` UTF-8 / numeric / trailing-content rules are
//      preserved.
//   2. A callback-free DOM build via plain ``nlohmann::json::parse`` so the
//      caller gets the same ``nlohmann::json`` value the original API did.
//
// The parser is intentionally minimal: it does not know about the filesystem,
// trusted-paths, hardlink counts, or the pack verifier; callers retain all
// of those responsibilities.
struct StrictJsonError {
    enum class Kind {
        Empty,
        DepthExceeded,
        DuplicateKey,
        TrailingContent,
        InvalidNumber,
        InvalidEncoding,
        InvalidSyntax,
        TooLarge,
    };
    Kind kind{Kind::InvalidSyntax};
    std::size_t position{0};
    std::string message;
};

struct StrictJsonResult {
    std::optional<nlohmann::json> value;
    std::optional<StrictJsonError> error;
    static StrictJsonResult success(nlohmann::json value);
    static StrictJsonResult failure(StrictJsonError error);
};

// Hard upper bound for the depth counter. The pack verifier and runtime
// config share this constant via the header so neither can drift.
inline constexpr std::size_t kStrictJsonMaxDepth = 128;

StrictJsonResult parse_strict_json(std::string_view bytes,
                                   std::size_t maximum_bytes);
StrictJsonResult parse_strict_json_file(const std::string& path,
                                        std::size_t maximum_bytes);

}  // namespace agent

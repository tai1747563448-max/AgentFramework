#pragma once

#include "domain/runtime_error.h"

#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>

namespace agent {

template <typename T>
class Result {
public:
    static Result success(T value) {
        return Result(std::move(value));
    }

    static Result failure(RuntimeError error) {
        return Result(std::move(error));
    }

    bool has_value() const noexcept {
        return std::holds_alternative<T>(storage_);
    }

    T& value() {
        if (!has_value()) {
            throw std::logic_error("result does not contain a value");
        }
        return std::get<T>(storage_);
    }

    const T& value() const {
        if (!has_value()) {
            throw std::logic_error("result does not contain a value");
        }
        return std::get<T>(storage_);
    }

    RuntimeError& error() {
        if (has_value()) {
            throw std::logic_error("result does not contain an error");
        }
        return std::get<RuntimeError>(storage_);
    }

    const RuntimeError& error() const {
        if (has_value()) {
            throw std::logic_error("result does not contain an error");
        }
        return std::get<RuntimeError>(storage_);
    }

private:
    explicit Result(T value) : storage_(std::move(value)) {}
    explicit Result(RuntimeError error) : storage_(std::move(error)) {}

    std::variant<T, RuntimeError> storage_;
};

template <>
class Result<void> {
public:
    static Result success() {
        return Result();
    }

    static Result failure(RuntimeError error) {
        return Result(std::move(error));
    }

    bool has_value() const noexcept {
        return std::holds_alternative<std::monostate>(storage_);
    }

    void value() const {
        if (!has_value()) {
            throw std::logic_error("result does not contain a value");
        }
    }

    RuntimeError& error() {
        if (has_value()) {
            throw std::logic_error("result does not contain an error");
        }
        return std::get<RuntimeError>(storage_);
    }

    const RuntimeError& error() const {
        if (has_value()) {
            throw std::logic_error("result does not contain an error");
        }
        return std::get<RuntimeError>(storage_);
    }

private:
    // Default state carries std::monostate to denote success; carrying
    // a RuntimeError denotes failure. Single-field storage mirrors
    // Result<T>'s std::variant<T, RuntimeError> so the two
    // specialisations share the same mental model.
    Result() : storage_(std::monostate{}) {}
    explicit Result(RuntimeError error) : storage_(std::move(error)) {}

    std::variant<std::monostate, RuntimeError> storage_;
};

}  // namespace agent

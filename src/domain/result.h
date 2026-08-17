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
        return Result(false, std::move(error));
    }

    bool has_value() const noexcept {
        return has_value_;
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
        return *error_;
    }

    const RuntimeError& error() const {
        if (has_value()) {
            throw std::logic_error("result does not contain an error");
        }
        return *error_;
    }

private:
    Result() = default;
    Result(bool has_value, RuntimeError error)
        : has_value_(has_value), error_(std::move(error)) {}

    bool has_value_ = true;
    std::optional<RuntimeError> error_;
};

}  // namespace agent

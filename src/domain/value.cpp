#include "domain/value.h"

#include <utility>

namespace agent {

Value::Value() : storage_(nullptr) {}

Value::Value(bool value) : storage_(value) {}

Value::Value(std::int64_t value) : storage_(value) {}

Value::Value(double value) : storage_(value) {}

Value::Value(std::string value) : storage_(std::move(value)) {}

Value::Value(const char* value) : storage_(std::string(value)) {}

Value::Value(Storage storage) : storage_(std::move(storage)) {}

Value Value::array(Array values) {
    return Value(std::make_shared<const ArrayNode>(ArrayNode{std::move(values)}));
}

Value Value::object(Object values) {
    return Value(std::make_shared<const ObjectNode>(ObjectNode{std::move(values)}));
}

bool Value::is_array() const noexcept {
    return std::holds_alternative<std::shared_ptr<const ArrayNode>>(storage_);
}

bool Value::is_object() const noexcept {
    return std::holds_alternative<std::shared_ptr<const ObjectNode>>(storage_);
}

bool Value::is_integer() const noexcept {
    return std::holds_alternative<std::int64_t>(storage_);
}

bool Value::is_double() const noexcept {
    return std::holds_alternative<double>(storage_);
}

const Value::Array& Value::as_array() const {
    return std::get<std::shared_ptr<const ArrayNode>>(storage_)->values;
}

const Value::Object& Value::as_object() const {
    return std::get<std::shared_ptr<const ObjectNode>>(storage_)->values;
}

const std::string& Value::as_string() const {
    return std::get<std::string>(storage_);
}

bool Value::as_bool() const {
    return std::get<bool>(storage_);
}

std::int64_t Value::as_integer() const {
    return std::get<std::int64_t>(storage_);
}

double Value::as_double() const {
    return std::get<double>(storage_);
}

const Value& Value::at(const std::string& key) const {
    return as_object().at(key);
}

const Value::Storage& Value::storage() const noexcept {
    return storage_;
}

bool operator==(const Value& left, const Value& right) {
    if (left.storage_.index() != right.storage_.index()) {
        return false;
    }

    if (left.is_array()) {
        return left.as_array() == right.as_array();
    }
    if (left.is_object()) {
        return left.as_object() == right.as_object();
    }
    return left.storage_ == right.storage_;
}

}  // namespace agent

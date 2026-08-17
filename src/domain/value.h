#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace agent {

class Value {
public:
    struct ArrayNode;
    struct ObjectNode;
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value>;
    using Storage = std::variant<std::nullptr_t, bool, std::int64_t, double,
                                 std::string,
                                 std::shared_ptr<const ArrayNode>,
                                 std::shared_ptr<const ObjectNode>>;

    Value();
    Value(bool value);
    Value(std::int64_t value);
    Value(double value);
    Value(std::string value);
    Value(const char* value);
    static Value array(Array values);
    static Value object(Object values);

    bool is_array() const noexcept;
    bool is_object() const noexcept;
    bool is_integer() const noexcept;
    bool is_double() const noexcept;
    const Array& as_array() const;
    const Object& as_object() const;
    const std::string& as_string() const;
    bool as_bool() const;
    std::int64_t as_integer() const;
    double as_double() const;
    const Value& at(const std::string& key) const;
    const Storage& storage() const noexcept;
    friend bool operator==(const Value& left, const Value& right);

private:
    explicit Value(Storage storage);

    Storage storage_;
};

struct Value::ArrayNode {
    Array values;
};

struct Value::ObjectNode {
    Object values;
};

}  // namespace agent

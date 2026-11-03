#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// Minimal, dependency-free JSON reader (RFC 8259) for the engine's asset importers. It parses a
// complete document into an immutable tree of Values; there is no serialization, no streaming,
// and no schema support. It is deliberately small: asset formats that embed JSON (glTF) only need
// to look values up by key/index and read numbers, strings, and booleans out of them.
namespace Frost::Json {

    class Value;

    using Array = std::vector<Value>;
    // Insertion-ordered key/value list rather than a map: glTF objects are small, and linear
    // lookup keeps the representation trivially standard-conforming with the recursive Value type.
    using Object = std::vector<std::pair<std::string, Value>>;

    class Value final {
    public:
        Value() = default;  // null

        explicit Value(bool boolean) : m_Data(boolean) {}
        explicit Value(double number) : m_Data(number) {}
        explicit Value(std::string string) : m_Data(std::move(string)) {}
        explicit Value(Array array) : m_Data(std::move(array)) {}
        explicit Value(Object object) : m_Data(std::move(object)) {}

        [[nodiscard]]
        bool IsNull() const noexcept;
        [[nodiscard]]
        bool IsBoolean() const noexcept;
        [[nodiscard]]
        bool IsNumber() const noexcept;
        [[nodiscard]]
        bool IsString() const noexcept;
        [[nodiscard]]
        bool IsArray() const noexcept;
        [[nodiscard]]
        bool IsObject() const noexcept;

        // Typed accessors. Each throws std::runtime_error if the value is not of that type.
        [[nodiscard]]
        bool AsBoolean() const;
        [[nodiscard]]
        double AsNumber() const;
        [[nodiscard]]
        const std::string& AsString() const;
        [[nodiscard]]
        const Array& AsArray() const;
        [[nodiscard]]
        const Object& AsObject() const;

        // Object member lookup. Returns nullptr if this value is not an object or has no such key
        // (so lookups can be chained without checking the type first).
        [[nodiscard]]
        const Value* Find(std::string_view key) const noexcept;

        // Array element lookup. Returns nullptr if this value is not an array or `index` is out
        // of range.
        [[nodiscard]]
        const Value* At(std::size_t index) const noexcept;

    private:
        std::variant<std::nullptr_t, bool, double, std::string, Array, Object> m_Data = nullptr;
    };

    // Parses a complete JSON document. Throws std::runtime_error (with a byte offset in the
    // message) on malformed input, including trailing non-whitespace after the root value.
    [[nodiscard]]
    Value Parse(std::string_view text);

}  // namespace Frost::Json

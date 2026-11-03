#include "Json.hpp"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace {

    using Frost::Json::Array;
    using Frost::Json::Object;
    using Frost::Json::Value;

    // Nesting depth past which a document is rejected. Valid asset files are a handful of levels
    // deep; the limit exists so that a malicious or corrupt file cannot overflow the stack through
    // the recursive descent below.
    constexpr std::size_t kMaxDepth = 256;

    class Parser final {
    public:
        explicit Parser(const std::string_view text) : m_Text(text) {}

        [[nodiscard]]
        Value ParseDocument() {
            SkipWhitespace();
            Value root = ParseValue(0);
            SkipWhitespace();

            if (!AtEnd()) { throw Error("unexpected trailing characters after the root value"); }

            return root;
        }

    private:
        [[nodiscard]]
        std::runtime_error Error(const std::string& what) const {
            std::ostringstream message;

            message << "JSON parse error at byte " << m_Position << ": " << what << '.';

            return std::runtime_error(message.str());
        }

        [[nodiscard]]
        bool AtEnd() const noexcept { return m_Position >= m_Text.size(); }

        [[nodiscard]]
        char Peek() const noexcept { return AtEnd() ? '\0' : m_Text[m_Position]; }

        void SkipWhitespace() noexcept {
            while (!AtEnd()) {
                const char c = m_Text[m_Position];
                if (c != ' ' && c != '\t' && c != '\n' && c != '\r') { break; }
                ++m_Position;
            }
        }

        void Expect(const char expected) {
            if (Peek() != expected) {
                std::string what = std::string("expected '") + expected + '\'';
                if (AtEnd()) { what += " but reached end of input"; }
                else { what += std::string(" but found '") + Peek() + '\''; }
                throw Error(what);
            }
            ++m_Position;
        }

        void ExpectLiteral(const std::string_view literal) {
            if (m_Text.substr(m_Position, literal.size()) != literal) {
                throw Error("invalid literal (expected '" + std::string(literal) + "')");
            }
            m_Position += literal.size();
        }

        [[nodiscard]]
        Value ParseValue(const std::size_t depth) {
            if (depth > kMaxDepth) { throw Error("nesting too deep"); }

            switch (Peek()) {
                case '{': return ParseObject(depth);
                case '[': return ParseArray(depth);
                case '"': return Value(ParseString());
                case 't': ExpectLiteral("true"); return Value(true);
                case 'f': ExpectLiteral("false"); return Value(false);
                case 'n': ExpectLiteral("null"); return Value();
                default:
                    if (Peek() == '-' || IsDigit(Peek())) { return Value(ParseNumber()); }
                    if (AtEnd()) { throw Error("unexpected end of input"); }
                    throw Error(std::string("unexpected character '") + Peek() + '\'');
            }
        }

        [[nodiscard]]
        Value ParseObject(const std::size_t depth) {
            Expect('{');
            Object members;

            SkipWhitespace();
            if (Peek() == '}') {
                ++m_Position;
                return Value(std::move(members));
            }

            while (true) {
                SkipWhitespace();
                if (Peek() != '"') { throw Error("expected a string key"); }
                std::string key = ParseString();

                SkipWhitespace();
                Expect(':');
                SkipWhitespace();

                Value value = ParseValue(depth + 1);
                members.emplace_back(std::move(key), std::move(value));

                SkipWhitespace();
                if (Peek() == ',') {
                    ++m_Position;
                    continue;
                }
                Expect('}');
                return Value(std::move(members));
            }
        }

        [[nodiscard]]
        Value ParseArray(const std::size_t depth) {
            Expect('[');
            Array elements;

            SkipWhitespace();
            if (Peek() == ']') {
                ++m_Position;
                return Value(std::move(elements));
            }

            while (true) {
                SkipWhitespace();
                elements.push_back(ParseValue(depth + 1));

                SkipWhitespace();
                if (Peek() == ',') {
                    ++m_Position;
                    continue;
                }
                Expect(']');
                return Value(std::move(elements));
            }
        }

        [[nodiscard]]
        std::string ParseString() {
            Expect('"');
            std::string result;

            while (true) {
                if (AtEnd()) { throw Error("unterminated string"); }

                const char c = m_Text[m_Position++];

                if (c == '"') { return result; }

                if (static_cast<unsigned char>(c) < 0x20) { throw Error("unescaped control character in string"); }

                if (c != '\\') {
                    result.push_back(c);
                    continue;
                }

                if (AtEnd()) { throw Error("unterminated escape sequence"); }

                const char escape = m_Text[m_Position++];
                switch (escape) {
                    case '"': result.push_back('"'); break;
                    case '\\': result.push_back('\\'); break;
                    case '/': result.push_back('/'); break;
                    case 'b': result.push_back('\b'); break;
                    case 'f': result.push_back('\f'); break;
                    case 'n': result.push_back('\n'); break;
                    case 'r': result.push_back('\r'); break;
                    case 't': result.push_back('\t'); break;
                    case 'u': AppendUtf8(ParseUnicodeEscape(), result); break;
                    default: throw Error(std::string("invalid escape sequence '\\") + escape + '\'');
                }
            }
        }

        // Parses the four hex digits following "\u" (already consumed) and, for a high surrogate,
        // the mandatory "\uXXXX" low surrogate that follows it. Returns the decoded code point.
        [[nodiscard]]
        std::uint32_t ParseUnicodeEscape() {
            const std::uint32_t first = ParseHex4();

            if (first >= 0xDC00 && first <= 0xDFFF) { throw Error("unexpected low surrogate in \\u escape"); }

            if (first < 0xD800 || first > 0xDBFF) { return first; }

            // High surrogate: must be immediately followed by an escaped low surrogate.
            if (m_Text.substr(m_Position, 2) != "\\u") { throw Error("high surrogate not followed by low surrogate"); }
            m_Position += 2;

            const std::uint32_t second = ParseHex4();
            if (second < 0xDC00 || second > 0xDFFF) { throw Error("high surrogate not followed by low surrogate"); }

            return 0x10000 + ((first - 0xD800) << 10) + (second - 0xDC00);
        }

        [[nodiscard]]
        std::uint32_t ParseHex4() {
            if (m_Position + 4 > m_Text.size()) { throw Error("truncated \\u escape"); }

            std::uint32_t value = 0;
            for (int i = 0; i < 4; ++i) {
                const char c = m_Text[m_Position++];
                value <<= 4;

                if (c >= '0' && c <= '9') { value |= static_cast<std::uint32_t>(c - '0'); }
                else if (c >= 'a' && c <= 'f') { value |= static_cast<std::uint32_t>(c - 'a' + 10); }
                else if (c >= 'A' && c <= 'F') { value |= static_cast<std::uint32_t>(c - 'A' + 10); }
                else { throw Error("invalid hex digit in \\u escape"); }
            }

            return value;
        }

        static void AppendUtf8(const std::uint32_t codePoint, std::string& out) {
            if (codePoint < 0x80) {
                out.push_back(static_cast<char>(codePoint));
            }
            else if (codePoint < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
                out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
            }
            else if (codePoint < 0x10000) {
                out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
                out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
            }
            else {
                out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
                out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
            }
        }

        [[nodiscard]]
        static bool IsDigit(const char c) noexcept { return c >= '0' && c <= '9'; }

        // Validates the JSON number grammar (-?int frac? exp?) before converting, since
        // std::from_chars is more permissive than JSON (it accepts e.g. leading '+', "1.", ".5").
        [[nodiscard]]
        double ParseNumber() {
            const std::size_t start = m_Position;

            if (Peek() == '-') { ++m_Position; }

            if (Peek() == '0') {
                ++m_Position;
            }
            else if (IsDigit(Peek())) {
                while (IsDigit(Peek())) { ++m_Position; }
            }
            else {
                throw Error("invalid number");
            }

            if (Peek() == '.') {
                ++m_Position;
                if (!IsDigit(Peek())) { throw Error("invalid number (expected digits after '.')"); }
                while (IsDigit(Peek())) { ++m_Position; }
            }

            if (Peek() == 'e' || Peek() == 'E') {
                ++m_Position;
                if (Peek() == '+' || Peek() == '-') { ++m_Position; }
                if (!IsDigit(Peek())) { throw Error("invalid number (expected digits in exponent)"); }
                while (IsDigit(Peek())) { ++m_Position; }
            }

            const char* const first = m_Text.data() + start;
            const char* const last = m_Text.data() + m_Position;

            // Out-of-range magnitudes (e.g. 1e999) are rejected rather than clamped: an asset
            // with such a value is corrupt and silently reading +-inf would only defer the failure.
            double value = 0.0;

#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
            const std::from_chars_result result = std::from_chars(first, last, value);
            if (result.ec != std::errc{} || result.ptr != last) { throw Error("invalid number"); }
#else
            // Standard libraries without floating-point std::from_chars (older libc++). strtod is
            // locale-sensitive, but the grammar was validated above and the engine never changes
            // the C locale from its default.
            const std::string token(first, last);
            char* end = nullptr;
            errno = 0;
            value = std::strtod(token.c_str(), &end);
            if (end != token.c_str() + token.size() || errno == ERANGE || !std::isfinite(value)) {
                throw Error("invalid number");
            }
#endif

            return value;
        }

        std::string_view m_Text;
        std::size_t m_Position = 0;
    };

}  // namespace

namespace Frost::Json {

    bool Value::IsNull() const noexcept { return std::holds_alternative<std::nullptr_t>(m_Data); }
    bool Value::IsBoolean() const noexcept { return std::holds_alternative<bool>(m_Data); }
    bool Value::IsNumber() const noexcept { return std::holds_alternative<double>(m_Data); }
    bool Value::IsString() const noexcept { return std::holds_alternative<std::string>(m_Data); }
    bool Value::IsArray() const noexcept { return std::holds_alternative<Array>(m_Data); }
    bool Value::IsObject() const noexcept { return std::holds_alternative<Object>(m_Data); }

    bool Value::AsBoolean() const {
        if (const bool* const value = std::get_if<bool>(&m_Data)) { return *value; }
        throw std::runtime_error("JSON value is not a boolean.");
    }

    double Value::AsNumber() const {
        if (const double* const value = std::get_if<double>(&m_Data)) { return *value; }
        throw std::runtime_error("JSON value is not a number.");
    }

    const std::string& Value::AsString() const {
        if (const std::string* const value = std::get_if<std::string>(&m_Data)) { return *value; }
        throw std::runtime_error("JSON value is not a string.");
    }

    const Array& Value::AsArray() const {
        if (const Array* const value = std::get_if<Array>(&m_Data)) { return *value; }
        throw std::runtime_error("JSON value is not an array.");
    }

    const Object& Value::AsObject() const {
        if (const Object* const value = std::get_if<Object>(&m_Data)) { return *value; }
        throw std::runtime_error("JSON value is not an object.");
    }

    const Value* Value::Find(const std::string_view key) const noexcept {
        const Object* const object = std::get_if<Object>(&m_Data);
        if (object == nullptr) { return nullptr; }

        for (const auto& [memberKey, memberValue] : *object) {
            if (memberKey == key) { return &memberValue; }
        }

        return nullptr;
    }

    const Value* Value::At(const std::size_t index) const noexcept {
        const Array* const array = std::get_if<Array>(&m_Data);
        if (array == nullptr || index >= array->size()) { return nullptr; }

        return &(*array)[index];
    }

    Value Parse(const std::string_view text) {
        Parser parser(text);
        return parser.ParseDocument();
    }

}  // namespace Frost::Json

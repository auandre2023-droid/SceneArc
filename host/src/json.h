// Minimal JSON reader/writer for the host <-> web UI bridge (UTF-8).
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <memory>
#include <string>
#include <vector>

namespace vp {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    static Json Parse(const std::string& text, bool* ok = nullptr) {
        Parser parser{text, 0, true};
        Json value = parser.Value();
        parser.SkipSpace();
        if (parser.pos != text.size()) parser.ok = false;
        if (ok) *ok = parser.ok;
        return parser.ok ? value : Json();
    }

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_object() const { return type_ == Type::Object; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_string() const { return type_ == Type::String; }

    const Json& operator[](const std::string& key) const {
        static const Json null_value;
        if (type_ != Type::Object) return null_value;
        for (const auto& member : object_)
            if (member.first == key) return member.second;
        return null_value;
    }
    const std::vector<Json>& items() const { return array_; }
    const std::vector<std::pair<std::string, Json>>& members() const { return object_; }

    std::string str(const std::string& fallback = {}) const { return type_ == Type::String ? string_ : fallback; }
    double num(double fallback = 0) const { return type_ == Type::Number ? number_ : fallback; }
    std::int64_t i64(std::int64_t fallback = 0) const { return type_ == Type::Number ? static_cast<std::int64_t>(number_) : fallback; }
    bool boolean(bool fallback = false) const { return type_ == Type::Bool ? bool_ : fallback; }

    static std::string Quote(const std::string& text) {
        std::string out = "\"";
        for (const unsigned char c : text) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char buffer[8]; std::snprintf(buffer, sizeof buffer, "\\u%04x", c); out += buffer; }
                else out += static_cast<char>(c);
            }
        }
        return out + "\"";
    }

private:
    struct Parser {
        const std::string& s;
        std::size_t pos;
        bool ok;

        void SkipSpace() { while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r')) ++pos; }
        bool Eat(char c) { SkipSpace(); if (pos < s.size() && s[pos] == c) { ++pos; return true; } return false; }
        bool Literal(const char* word) {
            std::size_t n = 0;
            while (word[n]) ++n;
            if (s.compare(pos, n, word) != 0) return false;
            pos += n;
            return true;
        }
        static void AppendUtf8(std::string& out, std::uint32_t cp) {
            if (cp < 0x80) out += static_cast<char>(cp);
            else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) {
                out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F));
            }
        }
        std::uint32_t Hex4() {
            if (pos + 4 > s.size()) { ok = false; return 0; }
            std::uint32_t value = 0;
            for (int i = 0; i < 4; ++i) {
                const char c = s[pos++];
                value <<= 4;
                if (c >= '0' && c <= '9') value |= static_cast<std::uint32_t>(c - '0');
                else if (c >= 'a' && c <= 'f') value |= static_cast<std::uint32_t>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') value |= static_cast<std::uint32_t>(c - 'A' + 10);
                else { ok = false; return 0; }
            }
            return value;
        }
        std::string String() {
            std::string out;
            ++pos; // opening quote
            while (pos < s.size()) {
                const char c = s[pos++];
                if (c == '"') return out;
                if (c != '\\') { out += c; continue; }
                if (pos >= s.size()) break;
                const char e = s[pos++];
                switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    std::uint32_t cp = Hex4();
                    if (cp >= 0xD800 && cp < 0xDC00 && pos + 6 <= s.size() && s[pos] == '\\' && s[pos + 1] == 'u') {
                        pos += 2;
                        const std::uint32_t low = Hex4();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: out += e; break;
                }
            }
            ok = false;
            return out;
        }
        Json Value() {
            Json value;
            SkipSpace();
            if (pos >= s.size()) { ok = false; return value; }
            const char c = s[pos];
            if (c == '{') {
                ++pos;
                value.type_ = Type::Object;
                if (Eat('}')) return value;
                do {
                    SkipSpace();
                    if (pos >= s.size() || s[pos] != '"') { ok = false; return value; }
                    std::string key = String();
                    if (!Eat(':')) { ok = false; return value; }
                    value.object_.emplace_back(std::move(key), Value());
                    if (!ok) return value;
                } while (Eat(','));
                if (!Eat('}')) ok = false;
            } else if (c == '[') {
                ++pos;
                value.type_ = Type::Array;
                if (Eat(']')) return value;
                do {
                    value.array_.push_back(Value());
                    if (!ok) return value;
                } while (Eat(','));
                if (!Eat(']')) ok = false;
            } else if (c == '"') {
                value.type_ = Type::String;
                value.string_ = String();
            } else if (Literal("true")) {
                value.type_ = Type::Bool; value.bool_ = true;
            } else if (Literal("false")) {
                value.type_ = Type::Bool; value.bool_ = false;
            } else if (Literal("null")) {
                value.type_ = Type::Null;
            } else {
                const std::size_t start = pos;
                while (pos < s.size() && (s[pos] == '-' || s[pos] == '+' || s[pos] == '.' || s[pos] == 'e' || s[pos] == 'E' || (s[pos] >= '0' && s[pos] <= '9'))) ++pos;
                if (start == pos) { ok = false; return value; }
                value.type_ = Type::Number;
                value.number_ = std::strtod(s.substr(start, pos - start).c_str(), nullptr);
            }
            return value;
        }
    };

    Type type_{Type::Null};
    bool bool_{};
    double number_{};
    std::string string_;
    std::vector<Json> array_;
    std::vector<std::pair<std::string, Json>> object_; // vector: valid with an incomplete element type
};

} // namespace vp

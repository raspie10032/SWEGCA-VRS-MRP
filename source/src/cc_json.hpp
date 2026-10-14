#pragma once
#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cc {
inline void utf8_append(std::string& out, uint32_t u) {
    if (u < 0x80) out += char(u);
    else if (u < 0x800) { out += char(0xc0 | (u >> 6)); out += char(0x80 | (u & 63)); }
    else if (u < 0x10000) {
        out += char(0xe0 | (u >> 12)); out += char(0x80 | ((u >> 6) & 63)); out += char(0x80 | (u & 63));
    } else {
        out += char(0xf0 | (u >> 18)); out += char(0x80 | ((u >> 12) & 63));
        out += char(0x80 | ((u >> 6) & 63)); out += char(0x80 | (u & 63));
    }
}
// Strict UTF-8, including overlong encodings and surrogate rejection.
inline uint32_t utf8_next(std::string_view s, size_t& i) {
    if (i == s.size()) throw std::runtime_error("UTF8_EOF");
    unsigned c = uint8_t(s[i++]);
    if (c < 128) return c;
    unsigned n = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 9;
    if (n == 9 || s.size() - i < n) throw std::runtime_error("UTF8_INVALID");
    uint32_t v = c & (n == 1 ? 31 : n == 2 ? 15 : 7);
    for (unsigned j = 0; j < n; ++j) {
        c = uint8_t(s[i++]); if ((c & 0xc0) != 0x80) throw std::runtime_error("UTF8_INVALID");
        v = (v << 6) | (c & 63);
    }
    if (v < (n == 1 ? 0x80u : n == 2 ? 0x800u : 0x10000u) || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff))
        throw std::runtime_error("UTF8_INVALID");
    return v;
}
inline size_t utf16_units(std::string_view s) {
    size_t n = 0; for (size_t i = 0; i < s.size();) n += utf8_next(s, i) > 0xffff ? 2 : 1; return n;
}
inline std::string quote(std::string_view s) {
    constexpr char hex[] = "0123456789abcdef";
    std::string o; o.reserve(s.size() + 2); o += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\b': o += "\\b"; break;
        case '\f': o += "\\f"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default: if (c < 32) { o += "\\u00"; o += hex[c >> 4]; o += hex[c & 15]; } else o += char(c);
        }
    }
    o += '"'; return o;
}

struct Json {
    enum Kind { null, boolean, number, string, array, object } kind = null;
    std::string scalar;
    std::vector<Json> values;
    std::vector<std::string> keys;
    static Json str(std::string s) { Json j; j.kind = string; j.scalar = std::move(s); return j; }
    static Json num(uint64_t n) { Json j; j.kind = number; j.scalar = std::to_string(n); return j; }
    static Json flag(bool b) { Json j; j.kind = boolean; j.scalar = b ? "true" : "false"; return j; }
    static Json arr() { Json j; j.kind = array; return j; }
    static Json obj() { Json j; j.kind = object; return j; }
    const Json* find(std::string_view k) const {
        if (kind == object) for (size_t i = 0; i < keys.size(); ++i) if (keys[i] == k) return &values[i];
        return nullptr;
    }
    const Json& at(std::string_view k) const {
        const auto* p = find(k); if (!p) throw std::runtime_error("JSON_MISSING_FIELD: " + std::string(k)); return *p;
    }
    std::string text() const { if (kind != string) throw std::runtime_error("JSON_EXPECT_STRING"); return scalar; }
    std::string get(std::string_view k, std::string def = {}) const {
        const auto* p = find(k); return p ? p->text() : def;
    }
    bool truth() const { return kind == boolean && scalar == "true"; }
    bool flag_at(std::string_view k) const { auto p = find(k); return p && p->truth(); }
    uint64_t uint() const {
        if (kind != number || scalar.empty()) throw std::runtime_error("JSON_EXPECT_UINT");
        uint64_t n = 0; auto r = std::from_chars(scalar.data(), scalar.data() + scalar.size(), n);
        if (r.ec != std::errc{} || r.ptr != scalar.data() + scalar.size()) throw std::runtime_error("JSON_EXPECT_UINT");
        return n;
    }
    Json& set(std::string k, Json v) {
        if (kind != object) throw std::runtime_error("JSON_EXPECT_OBJECT");
        for (size_t i = 0; i < keys.size(); ++i) if (keys[i] == k) { values[i] = std::move(v); return *this; }
        keys.push_back(std::move(k)); values.push_back(std::move(v)); return *this;
    }
    std::string dump() const {
        if (kind == null) return "null";
        if (kind == boolean || kind == number) return scalar;
        if (kind == string) return quote(scalar);
        std::string o(1, kind == array ? '[' : '{');
        for (size_t i = 0; i < values.size(); ++i) {
            if (i) o += ',';
            if (kind == object) o += quote(keys[i]) + ':';
            o += values[i].dump();
        }
        o += kind == array ? ']' : '}'; return o;
    }
};

class JsonParser {
    std::string_view s_; size_t i_ = 0; bool padded_;
    [[noreturn]] void bad() const { throw std::runtime_error("JSON_INVALID_AT_" + std::to_string(i_)); }
    void ws() { while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\t' || s_[i_] == '\r')) ++i_; }
    bool take(char c) { if (i_ < s_.size() && s_[i_] == c) { ++i_; return true; } return false; }
    uint32_t hex4() {
        uint32_t v = 0;
        for (unsigned k = 0; k < 4; ++k) {
            if (i_ >= s_.size()) bad(); char c = s_[i_++];
            int h = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (h < 0) bad(); v = (v << 4) | unsigned(h);
        } return v;
    }
    std::string str() {
        if (!take('"')) bad(); std::string o;
        while (i_ < s_.size()) {
            unsigned char c = s_[i_++]; if (c == '"') return o; if (c < 32) bad();
            if (c >= 128) { size_t a = --i_; utf8_next(s_, i_); o.append(s_.substr(a, i_ - a)); continue; }
            if (c != '\\') { o += char(c); continue; }
            if (i_ == s_.size()) bad(); c = s_[i_++];
            switch (c) {
            case '"': case '\\': case '/': o += char(c); break;
            case 'b': o += '\b'; break; case 'f': o += '\f'; break;
            case 'n': o += '\n'; break; case 'r': o += '\r'; break; case 't': o += '\t'; break;
            case 'u': {
                uint32_t u = hex4();
                // KX2b: an unpaired surrogate escape reads as U+FFFD; a following escape is read on its own.
                if (u >= 0xd800 && u <= 0xdbff) {
                    const size_t back = i_;
                    if (take('\\') && take('u')) {
                        uint32_t lo = hex4();
                        if (lo >= 0xdc00 && lo <= 0xdfff) u = 0x10000 + ((u - 0xd800) << 10) + lo - 0xdc00;
                        else { i_ = back; u = 0xfffd; }
                    } else { i_ = back; u = 0xfffd; }
                } else if (u >= 0xdc00 && u <= 0xdfff) u = 0xfffd;
                utf8_append(o, u); break;
            }
            default: bad();
            }
        } bad();
    }
    Json value(unsigned depth) {
        if (depth > 256) bad(); ws(); if (i_ == s_.size()) bad();
        if (s_[i_] == '"') return Json::str(str());
        if (take('{')) {
            Json j = Json::obj(); ws(); if (take('}')) return j;
            for (;;) {
                ws(); std::string k = str(); if (j.find(k)) bad(); ws(); if (!take(':')) bad();
                j.set(std::move(k), value(depth + 1)); ws(); if (take('}')) return j; if (!take(',')) bad();
            }
        }
        if (take('[')) {
            Json j = Json::arr(); ws(); if (take(']')) return j;
            for (;;) { j.values.push_back(value(depth + 1)); ws(); if (take(']')) return j; if (!take(',')) bad(); }
        }
        for (auto word : {std::string_view("true"), std::string_view("false"), std::string_view("null")}) {
            if (s_.substr(i_, word.size()) == word) { i_ += word.size(); return word == "null" ? Json{} : Json::flag(word == "true"); }
        }
        const size_t a = i_; take('-'); const size_t digits = i_;
        while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
        if (i_ == digits || (!padded_ && i_ > digits + 1 && s_[digits] == '0')) bad();
        if (take('.')) { size_t p = i_; while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_; if (i_ == p) bad(); }
        if (take('e') || take('E')) {
            if (!take('+')) take('-'); size_t p = i_;
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_; if (i_ == p) bad();
        }
        Json j; j.kind = Json::number; j.scalar = s_.substr(a, i_ - a); return j;
    }
public:
    explicit JsonParser(std::string_view s, bool padded = false) : s_(s), padded_(padded) {}
    Json parse() { Json j = value(0); ws(); if (i_ != s_.size()) bad(); return j; }
};
inline Json parse(std::string_view s, bool rollout = false) { return JsonParser(s, rollout).parse(); }
}

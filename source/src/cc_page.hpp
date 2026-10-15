#pragma once
// VRS_MCP_codexapp_v1.md 7.5: the pages of vrs_restore and the ranges of vrs_read, shared by the Claude Code mode and the Codex mode.
// Moved out of vrs_ccd.cpp unchanged; both modes render through these functions so their output has one form (X6, X12).
#include "cc_io.hpp"
#include "cc_conv.hpp"
#include <compare>
#include <cstdint>
#include <string>
#include <vector>

namespace cc {
inline size_t budget_of(const Json& j) {
    const auto* b = j.find("budget"); uint64_t n = b ? b->uint() : 48000;
    if (n < 1 || n > 95000) throw std::runtime_error("BUDGET_RANGE_1_TO_95000"); return size_t(n);
}
struct Key { uint64_t o = 0, k = 0, c = 0; auto operator<=>(const Key&) const = default; };
inline Key key_of(const std::string& s) {
    size_t c = s.find(':'); if (c == s.npos || c == 0 || c + 1 == s.size()) throw std::runtime_error("START_EXPECT_O_K_C");
    auto uint = [](std::string_view s) {
        uint64_t n = 0; auto r = std::from_chars(s.data(), s.data() + s.size(), n);
        if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) throw std::runtime_error("START_EXPECT_O_K_C"); return n;
    };
    size_t end = s.find(':', c + 1); const std::string_view v(s);
    return {uint(v.substr(0, c)), uint(v.substr(c + 1, end == s.npos ? s.size() - c - 1 : end - c - 1)),
            end == s.npos ? 0 : uint(v.substr(end + 1))};
}
inline std::string key_text(Key k) { return std::to_string(k.o) + ":" + std::to_string(k.k) + ":" + std::to_string(k.c); }
// Bodies have already passed strict UTF-8 parsing. Positions below are code
// point boundaries; a four-byte UTF-8 scalar occupies two UTF-16 units.
inline size_t point_bytes(unsigned char c) { return c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4; }
inline size_t body_byte_at(std::string_view body, uint64_t c) {
    size_t at = 0; uint64_t units = 0;
    while (at < body.size() && units < c) { size_t n = point_bytes(body[at]); units += n == 4 ? 2 : 1; at += n; }
    if (units != c || (at == body.size() && c != 0)) throw std::runtime_error("START_BODY_POSITION_INVALID");
    return at;
}
struct Restored { Key key; Json item; std::string header; size_t units; };

// The page of a restoration: items in key order, the start of the request, the budget in UTF-16 units of the whole text.
inline Json restore_page(std::vector<Restored>& items, const Json& request, size_t budget, const std::string& session, uint64_t boundary) {
        std::sort(items.begin(), items.end(), [](const Restored& x, const Restored& y) { return x.key < y.key; });
        for (size_t i = 1; i < items.size(); ++i) if (items[i - 1].key == items[i].key) throw std::runtime_error("DUPLICATE_ITEM_KEY");
        size_t begin = 0; uint64_t body_start = 0;
        if (const auto* start = request.find("start")) {
            Key key = key_of(start->text()); body_start = key.c; key.c = 0;
            auto it = std::lower_bound(items.begin(), items.end(), key, [](const Restored& a, Key k) { return a.key < k; });
            if (it == items.end() || it->key != key) throw std::runtime_error("START_NOT_IN_ASSEMBLY"); begin = size_t(it - items.begin());
        }
        Key start_key = items.empty() ? Key{} : items[begin].key; start_key.c = body_start;
        const std::string start = items.empty() ? "끝" : key_text(start_key);
        auto marker = [&](size_t count, const std::string& next) {
            return "⟦vrs-restore v1 · 세션 " + session + " · 경계 " + (boundary == UINT64_MAX ? "없음" : std::to_string(boundary)) +
                " · 항목 " + std::to_string(count) + "/" + std::to_string(items.size()) + " · start=" + start + " · next=" + next + "⟧";
        };
        size_t end = begin, used = 0; uint64_t c = body_start;
        std::string fragments; Json result_items = Json::arr();
        auto next_text = [&] { if (end == items.size()) return std::string("끝"); Key k = items[end].key; k.c = c; return key_text(k); };
        while (end < items.size()) {
            const Restored& item = items[end]; const std::string& body = item.item.at("text").scalar;
            const size_t byte_start = body_byte_at(body, c);
            const std::string continuation = "[이어짐 c=" + std::to_string(c) + " · " + item.header.substr(1);
            std::string head = c ? continuation : item.header;
            const std::string after = end + 1 == items.size() ? "끝" : key_text(items[end + 1].key);
            size_t take = body.size() - byte_start, take_units = item.units - size_t(c);
            bool whole = used + 2 + utf16_units(head) + take_units + utf16_units(marker(result_items.values.size() + 1, after)) <= budget;
            if (!whole) {
                head = continuation; take = 0; take_units = 0;
                const size_t overhead = used + 2 + utf16_units(head);
                size_t at = byte_start, u = 0;
                while (at < body.size()) {
                    const size_t n = point_bytes(body[at]); u += n == 4 ? 2 : 1; at += n;
                    if (at == body.size()) break; // The whole remainder was tested above.
                    Key next = item.key; next.c = c + u;
                    if (overhead + u + utf16_units(marker(result_items.values.size() + 1, key_text(next))) > budget) break;
                    take = at - byte_start; take_units = u;
                }
                if (!take) {
                    if (result_items.values.empty()) throw std::runtime_error("PAGE_BUDGET_TOO_SMALL_AT_" + next_text());
                    break;
                }
            }
            const std::string part = body.substr(byte_start, take);
            Json fragment = item.item; fragment.set("text", Json::str(part)).set("c", Json::num(c));
            result_items.values.push_back(std::move(fragment)); fragments += "\n\n" + head + part;
            used += 2 + utf16_units(head) + take_units;
            if (!whole) { c += take_units; break; }
            ++end; c = 0;
        }
        const std::string next = next_text();
        std::string text = marker(result_items.values.size(), next) + fragments;
        if (utf16_units(text) > budget) throw std::runtime_error("MARKER_EXCEEDS_BUDGET");
        Json out = Json::obj(); out.set("ok", Json::flag(true)).set("text", Json::str(text)).set("items", std::move(result_items))
            .set("total", Json::num(items.size())).set("boundary", boundary == UINT64_MAX ? Json{} : Json::num(boundary))
            .set("next", end == items.size() ? Json{} : Json::str(next)).set("utf16", Json::num(utf16_units(text)));
        return out;
}

// vrs_read after the caller has checked that `path` (canonical) is under an allowed root.
inline Json read_page(const std::string& path, const Json& request) {
        const size_t budget = budget_of(request);
        File file(path); uint64_t start = request.at("start").uint(), end = request.at("end").uint();
        if (start > end || end > file.size()) throw std::runtime_error("READ_RANGE_INVALID");
        // At most 4 UTF-8 bytes per UTF-16 unit, plus the envelope reserve.
        std::string bytes = file.range(start, std::min<uint64_t>(end - start, uint64_t(budget) * 4 + 4));
        auto header = [&](size_t n, const std::string& hash) {
            uint64_t e = start + n;
            return "⟦vrs-read v1 · " + path + "#" + std::to_string(start) + "+" + std::to_string(n) + " · sha " + hash +
                " · next=" + (e == end ? "끝" : std::to_string(e)) + "⟧\n";
        };
        // The text protocol supports UTF-8 ranges; invalid/split encodings are
        // explicit errors, never decoded with replacement characters.
        size_t units = 0, chosen = 0, at = 0;
        while (at < bytes.size()) {
            const size_t prior = at; uint32_t cp;
            try { cp = utf8_next(bytes, at); }
            catch (...) { if (start + bytes.size() < end && bytes.size() - prior < 4) break; throw; }
            units += cp > 0xffff ? 2 : 1;
            if (units + utf16_units(header(at, std::string(64, '0'))) > budget) break;
            if (cp == '\n' || start + at == end) chosen = at;
        }
        if (!chosen && start != end) throw std::runtime_error("READ_LINE_EXCEEDS_BUDGET");
        bytes.resize(chosen); const std::string digest = sha256(bytes); std::string text = header(chosen, digest) + bytes;
        if (utf16_units(text) > budget) throw std::runtime_error("READ_HEADER_EXCEEDS_BUDGET");
        Json out = Json::obj(); out.set("ok", Json::flag(true)).set("text", Json::str(text)).set("bytes", Json::str(bytes)).set("sha256", Json::str(digest))
            .set("start", Json::num(start)).set("end", Json::num(start + chosen)).set("next", start + chosen == end ? Json{} : Json::num(start + chosen))
            .set("utf16", Json::num(utf16_units(text))); return out;
}
}

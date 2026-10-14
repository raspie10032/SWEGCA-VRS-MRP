#include "cc_conv.hpp"
#include <fstream>
#include <random>
#include <iostream>

using namespace cc;
static std::string all(const std::string& p) { File f(p, false, true); return f.range(0, f.size()); }
static void save(const std::string& p, std::string_view s, bool append = false) {
    fs::create_directories(vp_path(p).parent_path());
    std::ofstream f(vp_path(p), std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    f.write(s.data(), std::streamsize(s.size())); if (!f) throw std::runtime_error("TEST_WRITE");
}
// Main test thread is slot 63; resident/concurrent workers use 0..7.
static std::string cover_for(const std::string& output) {
    const auto p = vp_path(output), parent = p.parent_path();
    for (auto root = parent; !root.empty() && root != root.root_path(); root = root.parent_path())
        if (root.filename() == "cc_rollout") return vp_u8(root.parent_path() / "cover" / (vp_u8(p.lexically_relative(root)) + ".cv"));
    return vp_u8(parent.parent_path() / "cover" / parent.filename() / (vp_u8(p.filename()) + ".cv"));
}
static Tail local_tail(const std::string& input, const std::string& output) { return prepare_tail(input, output, cover_for(output)); }
static TailResult convert(const std::string& input, const std::string& output, unsigned slot = 63) {
    return conv_tail(input, output, cover_for(output), slot);
}
static size_t check_count = 0;
static void check(bool b, const char* what) { ++check_count; if (!b) throw std::runtime_error(what); }
static Json result(const TailResult& r) {
    Json j = r.counts.json(); j.set("source_end", Json::num(r.source_end)).set("output_end", Json::num(r.output_end)).set("lines", Json::num(r.lines)); return j;
}
static void same(const std::string& a, const std::string& b) { check(all(a) == all(b), "BYTES_DIFFER"); }

int main(int argc, char** argv) {
    try {
        if (argc < 2) return 2;
        const std::string mode = argv[1];
        if (mode == "core") {
            const std::string special = "한국어😀\"\\\n\r\t\b\f" + std::string(1, '\0');
            check(parse(quote(special)).text() == special, "JSON_ROUNDTRIP");
            check(parse("\"\\ud83d\\ude00\"").text() == "😀", "JSON_SURROGATE");
            check(parse("\"\\ud800\"").text() == "�", "KX2B_HIGH_SURROGATE");
            check(parse("\"\\udc00\"").text() == "�", "KX2B_LOW_SURROGATE");
            check(parse("\"\\ud800\\u0061\"").text() == "�a", "KX2B_FOLLOWING_ESCAPE");
            check(utf16_units("한국어😀") == 5, "UTF16");
            check(sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA256");
            for (auto bad : {"01", "{\"a\":1,\"a\":2}", "[1,]", "{", "1e", "true false"}) {
                bool rejected = false; try { parse(bad); } catch (...) { rejected = true; } check(rejected, "JSON_ACCEPTED_INVALID");
            }
            std::string raw = R"({"type":"user","timestamp":"2026-10-13T00:00:00.000Z","uuid":"u","message":{"content":[{"type":"text","text":"가😀"},{"type":"image","source":{"media_type":"image/png","data":"huge"}},{"type":"document"},{"type":"tool_result","tool_use_id":"t","content":"secret"}]}})";
            Counts counts; auto c = conv(raw, 31, &counts); check(c.lines.size() == 4 && counts.unknown == 1, "C2_USER");
            check(c.lines[2].find("data:image/png;base64,A") != std::string::npos && c.lines[3].find("secret") == std::string::npos, "C2_PLACEHOLDERS");
            for (auto& line : c.lines) { auto j = parse(line, true); check(j.at("payload").keys.back() == "cc", "KF1"); check(j.at("payload").at("cc").at("n").uint() == raw.size(), "CC_N"); }
            auto user = [&](const std::string& s, const std::string& extra = "") {
                return conv("{\"type\":\"user\",\"timestamp\":\"now\",\"uuid\":\"u\",\"message\":{\"content\":" + quote(s) + "}" + extra + "}", 0);
            };
            auto skip = [&](const Converted& c) { return c.lines.empty(); };
            check(skip(user("summary", ",\"isCompactSummary\":true")), "COMPACT_SUMMARY_SKIP");
            check(user("Stop hook feedback: retry", ",\"isMeta\":true").lines.size() == 2, "HOOK_START");
            check(user("meta", ",\"isMeta\":true").lines.size() == 1, "META");
            check(user("[Request interrupted by user]").lines.size() == 2, "INTERRUPTION");
            check(user("<command-name>/clear</command-name>").lines.size() == 1, "COMMAND");
            check(skip(conv(R"({"type":"new-kind"})", 0, &counts)), "UNKNOWN_LINE_SKIP");
            check(conv(R"({"type":"attachment","timestamp":"now","attachment":{"type":"queued_command","origin":{"kind":"human"},"prompt":"hello"}})", 0).lines.size() == 1, "HUMAN_ATTACHMENT");
            check(skip(conv(R"({"type":"system","subtype":"compact_boundary","timestamp":"now"})", 0)), "BOUNDARY_SKIP");
            check(conv(R"({"type":"assistant","timestamp":"now","isApiErrorMessage":true,"message":{"content":[]}})", 0).lines.size() == 2, "API_ERROR");
            auto assistant = conv(R"({"type":"assistant","timestamp":"now","message":{"content":[{"type":"text","text":"answer"},{"type":"tool_use","name":"tool","id":"call","input":{"large":"secret"}},{"type":"thinking","thinking":"reason"},{"type":"thinking","thinking":""},{"type":"new-block"}],"stop_reason":"end_turn"}})", 0, &counts);
            check(assistant.lines.size() == 4 && assistant.lines[2].find("secret") == std::string::npos, "ASSISTANT_BLOCKS");
            check(counts.unknown == 3, "UNKNOWN_COUNT");
            for (const auto* raw : {R"({"type":"queue-operation"})", R"({"type":"attachment","attachment":{"type":"none"}})",
                 R"({"type":"assistant","message":{"content":[{"type":"thinking","thinking":""}]}})",
                 R"({"type":"user","message":{"content":[{"type":"document"}]}})", R"({"type":"user","message":{"content":[]}})"}) {
                Counts excluded; auto zero = conv(raw, 37, &excluded); check(skip(zero), "ZERO_KIND_SKIP");
                size_t excluded_lines = 0; for (const auto& [kind, v] : excluded.kinds) if (kind.starts_with("line:")) excluded_lines += v.out;
                check(excluded_lines == 1, "SKIP_STILL_EXCLUDED");
            }
            std::cout << "CORE PASS checks=" << check_count << "\n"; return 0;
        }
        if (argc != 4) return 2;
        const std::string input = canonical(argv[2]), output = argv[3];
        if (mode == "tail") {
            const Tail t = local_tail(input, output); Json j = Json::obj();
            j.set("source", Json::num(t.before.source)).set("ordinal", Json::num(t.before.ordinal)).set("output", Json::num(t.before.output)).set("tail_bytes", Json::num(t.bytes.size()));
            std::cout << j.dump() << '\n'; return 0;
        }
        if (mode == "conv_ready") { std::cout << "READY\n" << std::flush; std::cout << result(convert(input, output)).dump() << '\n'; return 0; }
        if (mode == "conv") { std::cout << result(convert(input, output)).dump() << '\n'; return 0; }
        if (mode == "parallel") {
            Resident pool(8); std::string error; TailResult r;
            conv_parallel(local_tail(input, output), pool, 63, nullptr, [&](TailResult v, std::string e) { r = std::move(v); error = std::move(e); });
            pool.drain(); check(error.empty(), error.c_str()); std::cout << result(r).dump() << '\n'; return 0;
        }
        if (mode != "determinism") return 2;
        const std::string name = vp_u8(vp_path(input).filename());
        const std::string a = output + "/a/" + name, b = output + "/b/" + name, c = output + "/c/" + name;
        const std::string source = output + "/source/" + name;
        const std::string raw = all(input); auto stats = convert(input, a); save(source, "");
        std::mt19937_64 random(20261013); std::vector<size_t> points{0, raw.size()};
        for (unsigned i = 0; i < 49; ++i) points.push_back(size_t(random() % (raw.size() + 1)));
        std::sort(points.begin(), points.end());
        for (size_t i = 1; i < points.size(); ++i) {
            save(source, std::string_view(raw).substr(points[i - 1], points[i] - points[i - 1]), true); convert(source, b);
        }
        same(a, b);
        std::array<std::string, 8> errors; std::vector<std::thread> threads; EntryFiles shared;
        for (size_t i = 0; i < 8; ++i) threads.emplace_back([&, i] { try { conv_tail(input, c, cover_for(c), unsigned(i), nullptr, &shared); } catch (const std::exception& e) { errors[i] = e.what(); } });
        for (auto& thread : threads) thread.join(); for (const auto& e : errors) check(e.empty(), e.c_str()); same(a, c);
        // U4: split the last emitting original line, leave the actual tail intact.
        size_t last = 0, last_end = 0;
        for (size_t at = 0; at < raw.size();) {
            size_t end = raw.find('\n', at); if (end == raw.npos) break;
            if (!conv(std::string_view(raw).substr(at, end - at), at).lines.empty()) { last = at; last_end = end; } at = end + 1;
        }
        const std::string d = output + "/d/" + name, partial = output + "/partial/" + name;
        save(partial, std::string_view(raw).substr(0, last)); convert(partial, d);
        const std::string before = all(d); size_t half = last + (last_end - last) / 2;
        save(partial, std::string_view(raw).substr(last, half - last), true); convert(partial, d); check(all(d) == before, "U4_HALF_VISIBLE");
        save(partial, std::string_view(raw).substr(half), true); convert(partial, d); same(a, d);
        // Recover C(T) cut in the middle of one original's emitted siblings.
        const std::string expected = all(a); size_t cut = expected.find('\n') + 1; cut = expected.find('\n', cut) + 1;
        const std::string torn = output + "/torn/" + name;
        save(torn, std::string_view(expected).substr(0, cut + 13)); convert(input, torn); same(a, torn);
        std::cout << "{\"U1\":\"PASS\",\"U4\":\"PASS\",\"threads\":8,\"tails\":50,\"original_bytes\":" << raw.size()
                  << ",\"rollout_bytes\":" << expected.size() << ",\"sha256\":" << quote(sha256(expected)) << ",\"counts\":" << stats.counts.json().dump() << "}\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}

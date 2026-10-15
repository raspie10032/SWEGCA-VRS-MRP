// VRS_MCP_codexapp_v1.md 7.3/7.5: the Codex mode daemon. lib7 opens $CODEX_HOME/sessions itself (no conversion, no watcher here);
// a request names a thread s, the daemon finds its rollout T(s) (D-x3), the last compaction c* and its replacement ids R (D-x4),
// and renders the lib7 items before c* outside R through the pages shared with the Claude Code mode (cc_page.hpp).
#include "cx_mode.hpp"
#include "cc_page.hpp"
#include "vrs.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <thread>
#include <unordered_map>

namespace cc {
#ifdef _WIN32
namespace {
constexpr uint64_t no_pos = UINT64_MAX;

uint64_t fnv64(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
    const auto* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
std::string slashed(std::string s) { std::replace(s.begin(), s.end(), '\\', '/'); return s; }
bool same_path(const std::string& a, const std::string& b) {
    const std::string x = slashed(a), y = slashed(b);
    return x.size() == y.size() && _stricmp(x.c_str(), y.c_str()) == 0;
}

// The first line of a rollout: session_meta (D-x3).
struct Meta { std::string id, parent; uint64_t cut = 0; };
Meta first_meta(const File& f) {
    std::string line; uint64_t at = 0; std::string buf(65536, '\0');
    for (;;) {
        const size_t got = f.read(buf.data(), buf.size(), at);
        if (!got) throw std::runtime_error("ROLLOUT_META_MISSING");
        const void* nl = std::memchr(buf.data(), '\n', got);
        if (nl) { line.append(buf.data(), size_t(static_cast<const char*>(nl) - buf.data())); break; }
        line.append(buf.data(), got); at += got;
        if (line.size() > 64 * 1024 * 1024) throw std::runtime_error("ROLLOUT_META_TOO_LARGE");
    }
    const Json j = parse(line, true);
    if (j.get("type") != "session_meta") throw std::runtime_error("ROLLOUT_META_MISSING");
    const Json& p = j.at("payload"); Meta m; m.id = p.get("id");
    if (const Json* fp = p.find("forked_from_id"); fp && fp->kind == Json::string) m.parent = fp->scalar;
    if (const Json* k = p.find("forked_from_ordinal_exclusive"); k && k->kind == Json::number) m.cut = k->uint();
    return m;
}
// The end of the last complete line: the bytes a call can take as written (X5).
uint64_t complete_end(const File& f) {
    uint64_t end = f.size(); std::string buf(65536, '\0');
    while (end) {
        const uint64_t a = end > buf.size() ? end - buf.size() : 0;
        const size_t got = f.read(buf.data(), size_t(end - a), a);
        if (got != end - a) throw std::runtime_error("SHORT_READ");
        for (size_t i = got; i > 0; --i) if (buf[i - 1] == '\n') return a + i;
        end = a;
    }
    return 0;
}

// A compacted line of a rollout: offset, length without the newline, ordinal.
struct Comp { uint64_t off, len, ord; };
// D-x4 by parts: the compacted lines of [from, end) of one file. Facts of bytes, so every scanner of the same bytes finds the same
// list; a node is published once by CAS and never changed, older nodes stay reachable through next.
struct CompNode {
    uint64_t end = 0, persisted = 0;
    std::vector<Comp> comps;           // the lines found in this node's range
    bool has_last = false; Comp last{}; // the last compacted line of [0, end)
    const CompNode* next = nullptr;
};
// The top-level type of a rollout line is in its first bytes ({"timestamp":…,"ordinal":…,"type":…,"payload":…}).
bool compacted_prefix(std::string_view head, uint64_t& ord) {
    const size_t pay = head.find("\"payload\"");
    const size_t ty = head.find("\"type\":\"compacted\"");
    if (ty == std::string_view::npos || (pay != std::string_view::npos && pay < ty)) return false;
    const size_t o = head.find("\"ordinal\":");
    ord = 0;
    if (o != std::string_view::npos && o < ty) for (size_t i = o + 10; i < head.size() && head[i] >= '0' && head[i] <= '9'; ++i) ord = ord * 10 + uint64_t(head[i] - '0');
    return true;
}
void scan_comps(const File& f, uint64_t from, uint64_t to, std::vector<Comp>& out) {
    std::string buf(4 * 1024 * 1024, '\0'), head; uint64_t at = from, line_at = from; bool in_line = false;
    while (at < to) {
        const size_t want = size_t(std::min<uint64_t>(buf.size(), to - at));
        const size_t got = f.read(buf.data(), want, at);
        if (got != want) throw std::runtime_error("SHORT_READ");
        size_t s = 0;
        while (s < got) {
            if (!in_line) { in_line = true; line_at = at + s; head.clear(); }
            const void* nl = std::memchr(buf.data() + s, '\n', got - s);
            const size_t e = nl ? size_t(static_cast<const char*>(nl) - buf.data()) : got;
            if (head.size() < 256) head.append(buf.data() + s, std::min<size_t>(e - s, 256 - head.size()));
            if (!nl) { s = got; break; }
            uint64_t ord = 0;
            if (compacted_prefix(head, ord)) out.push_back({line_at, at + e - line_at, ord});
            in_line = false; s = e + 1;
        }
        at += got;
    }
}

// The records of a file's scans kept in data/cstar/<thread>.cs, so a restart reads only the tail (E2-2).
// A record is 32 bytes: off, len, ord, check; an end record has off = no_pos and len = the scanned end. A torn record fails its check.
struct Record { uint64_t off, len, ord, check; };
uint64_t record_check(const Record& r) { return fnv64(&r, 24) ^ 0x7672732d63737472ull; }
void persist(const std::string& name, const std::vector<Comp>& comps, uint64_t end) {
    std::vector<Record> rs;
    for (const auto& c : comps) { Record r{c.off, c.len, c.ord, 0}; r.check = record_check(r); rs.push_back(r); }
    Record e{no_pos, end, 0, 0}; e.check = record_check(e); rs.push_back(e);
    HANDLE h = CreateFileW(vp_io_path(name).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;  // the records only save a rescan
    DWORD put = 0; WriteFile(h, rs.data(), DWORD(rs.size() * sizeof(Record)), &put, nullptr); CloseHandle(h);
}
CompNode* load_persisted(const std::string& name, uint64_t size) {
    File f(name, false, true); auto* n = new CompNode;
    if (!f.valid()) return n;
    const uint64_t bytes = f.size() / sizeof(Record) * sizeof(Record);
    if (!bytes || bytes > 64 * 1024 * 1024) return n;
    std::vector<Record> rs(size_t(bytes / sizeof(Record)));
    if (f.read(rs.data(), size_t(bytes), 0) != bytes) return n;
    std::vector<Comp> comps; uint64_t end = 0;
    for (const auto& r : rs) {
        if (record_check(r) != r.check) continue;
        if (r.off == no_pos) end = std::max(end, r.len); else comps.push_back({r.off, r.len, r.ord});
    }
    if (end > size) return n;  // not this file's bytes: scan again
    std::sort(comps.begin(), comps.end(), [](const Comp& a, const Comp& b) { return a.off < b.off; });
    comps.erase(std::unique(comps.begin(), comps.end(), [](const Comp& a, const Comp& b) { return a.off == b.off; }), comps.end());
    std::erase_if(comps, [&](const Comp& c) { return c.off + c.len + 1 > end; });
    n->end = end; n->persisted = end; n->comps = std::move(comps);
    if (!n->comps.empty()) { n->has_last = true; n->last = n->comps.back(); }
    return n;
}

// A thread s and the rollout it was found in (D-x3), immutable; a CAS table by s.
struct Located { std::string id, path; Meta meta; };
template <class T> class Table {
    std::array<std::atomic<T*>, 65536> slots_{};
public:
    std::atomic<T*>& slot(const std::string& key, const std::function<bool(T*)>& same) {
        const uint64_t h = fnv64(key.data(), key.size());
        for (size_t n = 0; n < slots_.size(); ++n) {
            auto& s = slots_[(h + n) % slots_.size()];
            T* p = s.load(std::memory_order_acquire);
            if (!p || same(p)) return s;
        }
        throw std::runtime_error("TABLE_FULL");
    }
};

struct Options {
    std::string home, data, endpoint; std::vector<std::string> read_roots;
    unsigned workers = 6, connections = 16; uint64_t timeout_ms = 5000;
    HANDLE ready_event = nullptr;
};

class CxDaemon {
    Options opts_;
    std::string sessions_, idx_, marks_, writers_, cstar_;
    vrs_h* lib_ = nullptr;
    std::function<vrs_h*()> lib_of_;  // VRS_onePC_v1.md B6: the one daemon's handle (attached share)
    Table<Located> threads_;
    struct PathSlot { std::string path; std::atomic<const CompNode*> head{nullptr}; };
    Table<PathSlot> paths_;
    HANDLE first_pipe_ = INVALID_HANDLE_VALUE;
    std::vector<std::thread> requests_;
    HANDLE stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    vrs_h* ready_lib() const {
        vrs_h* h = lib_of_ ? lib_of_() : lib_;
        if (!h) throw std::runtime_error("NOT_READY");
        const int rd = vrs_ready(h);
        if (rd < 0) throw std::runtime_error("VRS_READY_" + std::to_string(rd));
        if (rd != 1) throw std::runtime_error("NOT_READY");
        return h;
    }
    // D-x3: s is a UUIDv7; its first 48 bits are the milliseconds it was made. The rollout is named in the local date it was made,
    // which may differ from the UUID's date by a day either way, so the three folders d-1, d, d+1 are listed for the name ending -s.jsonl.
    std::vector<std::string> candidates(const std::string& s) const {
        if (s.size() != 36 || s[14] != '7') throw std::runtime_error("THREAD_ID_NOT_UUIDV7");
        uint64_t ms = 0;
        for (size_t i = 0; i < 13; ++i) {
            if (i == 8) continue; const char c = s[i];
            const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (v < 0) throw std::runtime_error("THREAD_ID_NOT_UUIDV7"); ms = ms * 16 + uint64_t(v);
        }
        std::vector<std::string> hits;
        for (int d = -1; d <= 1; ++d) {
            const std::time_t t = std::time_t(ms / 1000) + d * 86400; std::tm tm{};
            if (localtime_s(&tm, &t)) continue;
            char dir[32]; std::snprintf(dir, sizeof dir, "/%04d/%02d/%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
            const std::string folder = sessions_ + dir;
            WIN32_FIND_DATAW data{};
            HANDLE h = FindFirstFileExW(vp_io_path(folder + "/rollout-*-" + s + ".jsonl").c_str(), FindExInfoBasic, &data,
                                        FindExSearchNameMatch, nullptr, 0);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
                const std::string p = canonical(folder + "/" + vp_u8(fs::path(data.cFileName)));
                if (std::none_of(hits.begin(), hits.end(), [&](const std::string& q) { return same_path(p, q); })) hits.push_back(p);
            } while (FindNextFileW(h, &data));
            FindClose(h);
        }
        return hits;
    }
    const Located& locate(const std::string& s) {
        auto& slot = threads_.slot(s, [&](Located* p) { return p->id == s; });
        for (;;) {
            Located* known = slot.load(std::memory_order_acquire);
            if (known) {
                File f(known->path, false, true);
                if (f.valid()) { try { if (first_meta(f).id == s) return *known; } catch (const std::exception&) {} }
            }
            std::vector<std::unique_ptr<Located>> found;
            for (const auto& p : candidates(s)) {
                File f(p, false, true); if (!f.valid()) continue;
                Meta m; try { m = first_meta(f); } catch (const std::exception&) { continue; }
                if (m.id == s) found.emplace_back(new Located{s, p, m});
            }
            if (found.empty()) throw std::runtime_error("THREAD_ROLLOUT_NOT_FOUND");
            if (found.size() > 1) throw std::runtime_error("THREAD_ROLLOUT_AMBIGUOUS");
            Located* fresh = found[0].release();
            // A replaced entry is not freed: a peer may still read it. Replacement happens only when a rollout moved.
            if (slot.compare_exchange_strong(known, fresh, std::memory_order_acq_rel)) return *fresh;
            delete fresh;
        }
    }
    // The compacted lines of [0, e) of the file `path` (thread id names its records).
    const CompNode* comps_to(const std::string& path, const std::string& id, const File& f, uint64_t e) {
        auto& slot = paths_.slot(path, [&](PathSlot* p) { return same_path(p->path, path); });
        PathSlot* ps = slot.load(std::memory_order_acquire);
        if (!ps) {
            auto* fresh = new PathSlot; fresh->path = path; PathSlot* none = nullptr;
            if (slot.compare_exchange_strong(none, fresh, std::memory_order_acq_rel)) ps = fresh; else { delete fresh; ps = none; }
        }
        const std::string record = cstar_ + "/" + id + ".cs";
        for (;;) {
            const CompNode* h = ps->head.load(std::memory_order_acquire);
            if (h && h->end >= e) return h;
            std::unique_ptr<CompNode> node(h ? new CompNode : load_persisted(record, f.size()));
            if (h) { node->end = h->end; node->persisted = h->persisted; node->has_last = h->has_last; node->last = h->last; node->next = h; node->comps.clear(); }
            const uint64_t from = node->end;
            std::vector<Comp> found;
            if (from < e) scan_comps(f, from, e, found);
            if (!found.empty()) { node->has_last = true; node->last = found.back(); }
            node->comps.insert(node->comps.end(), found.begin(), found.end());
            node->end = std::max(from, e);
            const bool save = !found.empty() || node->end - node->persisted >= 4 * 1024 * 1024 || (!h && node->end > node->persisted);
            if (save) node->persisted = node->end;
            const CompNode* expected = h;
            if (ps->head.compare_exchange_strong(expected, node.get(), std::memory_order_acq_rel)) {
                if (save) persist(record, found, node->end);
                return node.release();
            }
        }
    }
    // The lineage of T: T, its parent, … (forked_from_id, forked_from_ordinal_exclusive). Index 0 is T.
    struct Link { std::string path, id; Meta meta; uint64_t cut = no_pos; /* lines of this file below this ordinal are inherited */ };
    std::vector<Link> lineage(const Located& t) {
        std::vector<Link> chain{{t.path, t.id, t.meta, no_pos}};
        while (!chain.back().meta.parent.empty()) {
            if (chain.size() > 64) throw std::runtime_error("LINEAGE_TOO_DEEP");
            const Located* p = nullptr;
            // A parent that is not under sessions (archived, deleted) is not in lib7's lineage either: the chain ends there.
            try { p = &locate(chain.back().meta.parent); } catch (const std::exception&) { break; }
            chain.push_back({p->path, p->id, p->meta, chain.back().meta.cut});
        }
        return chain;
    }
    // c* (D-x4): the last compacted line in lineage order. T's own last one, else the last one below the cut in the nearest ancestor.
    struct CStar { size_t link = 0; Comp comp{}; bool found = false; };
    CStar cstar(const std::vector<Link>& chain, uint64_t own_end) {
        for (size_t d = 0; d < chain.size(); ++d) {
            File f(chain[d].path);
            const uint64_t e = d == 0 ? own_end : complete_end(f);
            const CompNode* n = comps_to(chain[d].path, chain[d].id, f, e);
            if (d == 0) { if (n->has_last) return {0, n->last, true}; continue; }
            bool any = false; Comp best{};
            for (const CompNode* q = n; q; q = q->next)
                for (const auto& c : q->comps) if (c.ord < chain[d].cut && (!any || c.off > best.off)) { best = c; any = true; }
            if (any) return {d, best, true};
        }
        return {};
    }
    static uint64_t packed(size_t link, size_t links, uint64_t off) { return (uint64_t(links - 1 - link) << 40) | off; }
    size_t link_of(const std::vector<Link>& chain, const std::string& f) const {
        for (size_t d = 0; d < chain.size(); ++d) if (same_path(chain[d].path, f)) return d;
        throw std::runtime_error("SOURCE_OUTSIDE_LINEAGE");
    }
    Json assembly(const std::string& rollout, uint64_t end) {
        vrs_h* lib = ready_lib();
        if (int rc = vrs_refresh(lib, rollout.c_str())) throw std::runtime_error("LIB_REFRESH_FAILED_" + std::to_string(rc));
        int rc = vrs_wait(lib, rollout.c_str(), opts_.timeout_ms);
        if (rc) throw std::runtime_error("VRS_WAIT_" + std::to_string(rc));
        rc = vrs_wait_off(lib, rollout.c_str(), end, opts_.timeout_ms);
        if (rc == -2) {
            // A file the library has not met yet: its first assembly discovers it through the normal API (as the Claude mode does).
            char* raw = nullptr; size_t n = 0;
            int discovered = vrs_assemble_loc(lib, rollout.c_str(), 258400, &raw, &n); vrs_free(raw);
            if (discovered < 0 || discovered == 2) throw std::runtime_error("VRS_ASSEMBLE_" + std::to_string(discovered));
            rc = vrs_wait_off(lib, rollout.c_str(), end, opts_.timeout_ms);
        }
        if (rc) throw std::runtime_error("VRS_WAIT_OFF_" + std::to_string(rc));
        char* raw = nullptr; size_t n = 0;
        rc = vrs_assemble_loc(lib, rollout.c_str(), 258400, &raw, &n);
        std::unique_ptr<char, decltype(&vrs_free)> bytes(raw, vrs_free);
        if (rc) throw std::runtime_error("VRS_ASSEMBLE_" + std::to_string(rc));
        return parse(std::string_view(raw, n));
    }
    std::vector<std::string> replacement_ids(const Link& link, const Comp& c) {
        File f(link.path); const Json j = parse(f.range(c.off, c.len), true);
        if (j.get("type") != "compacted") throw std::runtime_error("CSTAR_NOT_COMPACTED");
        std::vector<std::string> ids;
        if (const Json* r = j.at("payload").find("replacement_history"); r && r->kind == Json::array)
            for (const auto& it : r->values) if (const Json* id = it.find("id"); id && id->kind == Json::string) ids.push_back(id->scalar);
        std::sort(ids.begin(), ids.end());
        return ids;
    }
    struct Selection { std::vector<Link> chain; CStar star; std::vector<std::string> R; Json items; };
    Selection select(const std::string& s) {
        ready_lib();  // KL1: before lib7 is ready, a request is answered at once, not held
        const Located& t = locate(s);
        uint64_t end = 0; { File f(t.path); end = complete_end(f); }
        Selection out; out.items = assembly(t.path, end);
        out.chain = lineage(t);
        out.star = cstar(out.chain, end);
        if (out.star.found) out.R = replacement_ids(out.chain[out.star.link], out.star.comp);
        return out;
    }
    Json restore(const Json& request) {
        const size_t budget = budget_of(request);
        const std::string s = request.at("thread").text();
        Selection sel = select(s);
        const size_t links = sel.chain.size();
        const uint64_t boundary = sel.star.found ? sel.star.comp.off : UINT64_MAX;
        std::vector<Restored> items;
        if (sel.star.found) {
            const uint64_t limit = packed(sel.star.link, links, sel.star.comp.off);
            std::unordered_map<std::string, std::unique_ptr<File>> open;
            for (const auto& a : sel.items.at("items").values) {
                const Json& p = a.at("p");
                if (p.get("type") != "message") continue;
                const std::string role = p.get("role");
                if (role != "user" && role != "assistant") continue;
                const std::string f = a.at("f").text(); const uint64_t o = a.at("o").uint(), n = a.at("n").uint();
                const size_t link = link_of(sel.chain, f);
                const uint64_t at = packed(link, links, o);
                if (at >= limit) continue;
                const Json* pid = p.find("id"); const std::string id = pid && pid->kind == Json::string ? pid->scalar : "";
                if (!id.empty() && std::binary_search(sel.R.begin(), sel.R.end(), id)) continue;
                auto& file = open[slashed(f)]; if (!file) file.reset(new File(f));
                const std::string raw = file->range(o, n), hash = sha256(raw);
                const Json j = parse(raw, true);
                if (j.get("type") != "response_item") throw std::runtime_error("SOURCE_TYPE_MISMATCH_AT_" + std::to_string(o));
                const Json& q = j.at("payload");
                if (q.get("type") != "message" || q.get("role") != role) throw std::runtime_error("SOURCE_ROLE_MISMATCH_AT_" + std::to_string(o));
                const Json* qid = q.find("id");
                if ((qid && qid->kind == Json::string ? qid->scalar : "") != id) throw std::runtime_error("SOURCE_ID_MISMATCH_AT_" + std::to_string(o));
                const std::string ts = j.get("timestamp"), filename = vp_u8(vp_path(f).filename());
                const Json& content = q.at("content");
                if (content.kind != Json::array) continue;
                for (size_t k = 0; k < content.values.size(); ++k) {
                    const Json& b = content.values[k]; const std::string bt = b.get("type");
                    std::string body;
                    if (bt == "input_text" || bt == "output_text") body = b.at("text").text();
                    else if (bt == "input_image" && role == "user") body = "[image · " + filename + "#" + std::to_string(o) + "+" + std::to_string(n) + " k=" + std::to_string(k) + "]";
                    else continue;
                    const std::string header = "[" + ts + " · " + (role == "user" ? "사용자" : "답") + " · " + filename + "#" + std::to_string(o) + "+" +
                        std::to_string(n) + " k=" + std::to_string(k) + " · sha " + hash.substr(0, 16) + "]\n";
                    Json loc = Json::obj(); loc.set("f", Json::str(f)).set("o", Json::num(o)).set("n", Json::num(n)).set("k", Json::num(k)).set("h", Json::str(hash));
                    Json item = Json::obj(); item.set("loc", std::move(loc)).set("role", Json::str(role)).set("timestamp", Json::str(ts)).set("text", Json::str(body));
                    const size_t units = utf16_units(body);
                    items.push_back({Key{at, k, 0}, std::move(item), header, units});
                }
            }
        }
        return restore_page(items, request, budget, s, boundary);
    }
    // Test only: the assembly with its places, c* and R, so a checker filters it on its own (C2).
    Json diag(const Json& request) {
        Selection sel = select(request.at("thread").text());
        Json out = Json::obj(); out.set("ok", Json::flag(true));
        Json items = Json::arr();
        for (const auto& a : sel.items.at("items").values) {
            const Json& p = a.at("p"); Json i = Json::obj();
            i.set("f", a.at("f")).set("o", a.at("o")).set("n", a.at("n")).set("type", Json::str(p.get("type")));
            if (const Json* r = p.find("role"); r && r->kind == Json::string) i.set("role", *r);
            if (const Json* d = p.find("id"); d && d->kind == Json::string) i.set("id", *d);
            items.values.push_back(std::move(i));
        }
        out.set("items", std::move(items));
        if (sel.star.found) {
            Json c = Json::obj(); c.set("f", Json::str(sel.chain[sel.star.link].path)).set("o", Json::num(sel.star.comp.off)).set("ord", Json::num(sel.star.comp.ord));
            out.set("cstar", std::move(c));
        } else out.set("cstar", Json{});
        Json r = Json::arr(); for (const auto& id : sel.R) r.values.push_back(Json::str(id)); out.set("R", std::move(r));
        return out;
    }
    Json read(const Json& request) {
        const size_t budget = budget_of(request); const std::string path = canonical(request.at("path").text());
        bool permitted = under(path, sessions_);
        for (const auto& root : opts_.read_roots) permitted = permitted || under(path, root);
        if (!permitted) throw std::runtime_error("READ_OUTSIDE_ROOT");
        (void)budget; return read_page(path, request);
    }
public:
    explicit CxDaemon(Options o) : opts_(std::move(o)) {}
    // VRS_onePC_v1.md B6: the share inside the one daemon (no pipe, no library of its own)
    void attach(std::function<vrs_h*()> lib) {
        sessions_ = canonical(opts_.home + "/sessions");
        fs::create_directories(vp_io_path(opts_.data)); opts_.data = canonical(opts_.data);
        if (opts_.data == sessions_ || under(opts_.data, sessions_)) throw std::runtime_error("DATA_MUST_BE_OUTSIDE_SESSIONS");
        cstar_ = opts_.data + "/cstar"; fs::create_directories(vp_io_path(cstar_));
        for (auto& root : opts_.read_roots) root = canonical(root);
        lib_of_ = std::move(lib);
    }
    const std::string& sessions() const { return sessions_; }
    std::string respond(std::string_view bytes) {
        try {
            const Json j = parse(bytes); const std::string op = j.at("op").text();
            if (op == "restore") return restore(j).dump() + '\n';
            if (op == "read") return read(j).dump() + '\n';
            if (op == "diag") return diag(j).dump() + '\n';
            throw std::runtime_error("UNKNOWN_OPERATION");
        } catch (const std::exception& e) {
            std::string message(e.what());
            if (message.size() > 1024) { size_t n = 1024; while (n && (uint8_t(message[n]) & 0xc0) == 0x80) --n; message.resize(n); }
            Json out = Json::obj(); out.set("ok", Json::flag(false)).set("error", Json::str(message)); return out.dump() + '\n';
        }
    }
    void start() {
        fs::create_directories(vp_io_path(opts_.home + "/sessions"));
        sessions_ = canonical(opts_.home + "/sessions");
        fs::create_directories(vp_io_path(opts_.data)); opts_.data = canonical(opts_.data);
        if (opts_.data == sessions_ || under(opts_.data, sessions_)) throw std::runtime_error("DATA_MUST_BE_OUTSIDE_SESSIONS");
        idx_ = opts_.data + "/idx"; marks_ = opts_.data + "/marks"; writers_ = opts_.data + "/writers"; cstar_ = opts_.data + "/cstar";
        for (const auto* d : {&idx_, &marks_, &writers_, &cstar_}) fs::create_directories(vp_io_path(*d));
        for (auto& root : opts_.read_roots) root = canonical(root);
        first_pipe_ = CreateNamedPipeW(vp_io_path(opts_.endpoint).c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, nullptr);
        // The bridge that started this process waits for this one fact: the name is served (by this process or an earlier one).
        if (opts_.ready_event) { SetEvent(opts_.ready_event); CloseHandle(opts_.ready_event); opts_.ready_event = nullptr; }
        if (first_pipe_ == INVALID_HANDLE_VALUE) throw std::runtime_error("PIPE_ALREADY_RUNNING_OR_UNAVAILABLE");
        vrs_opts vo{}; vo.sessions_dir = sessions_.c_str(); vo.index_dir = idx_.c_str(); vo.workers = opts_.workers;
        vo.change_markers = marks_.c_str(); vo.writer_markers = writers_.c_str();
        if (int rc = vrs_open(&vo, &lib_)) throw std::runtime_error("VRS_OPEN_" + std::to_string(rc));
        for (unsigned k = 0; k < opts_.connections; ++k) requests_.emplace_back([this, k] {
            HANDLE pipe = k == 0 ? first_pipe_ : INVALID_HANDLE_VALUE;
            auto create = [&] { return CreateNamedPipeW(vp_io_path(opts_.endpoint).c_str(), PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, nullptr); };
            for (;;) {
                if (pipe == INVALID_HANDLE_VALUE) pipe = create();
                if (pipe == INVALID_HANDLE_VALUE) break;
                bool connected = ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
                std::string line; bool complete = false;
                if (connected) while (line.size() < 65536) {
                    char buf[4096]; DWORD got = 0;
                    if (!ReadFile(pipe, buf, sizeof buf, &got, nullptr) || !got) break;
                    line.append(buf, got); if (line.find('\n') != line.npos) { complete = true; break; }
                }
                if (complete) {
                    std::string reply = respond(std::string_view(line).substr(0, line.find('\n')));
                    for (size_t at = 0; at < reply.size();) { DWORD put = 0; if (!WriteFile(pipe, reply.data() + at, DWORD(std::min<size_t>(reply.size() - at, 65536)), &put, nullptr) || !put) break; at += put; }
                    FlushFileBuffers(pipe);
                }
                // Keep an instance alive across replacement, so a second process cannot take FIRST_PIPE_INSTANCE in the gap.
                HANDLE next = create();
                DisconnectNamedPipe(pipe); CloseHandle(pipe); pipe = next;
            }
        });
    }
    void serve() { WaitForSingleObject(stop_, INFINITE); }
};
}  // namespace

int cx_daemon_main(const std::vector<std::string>& args) {
    Options o;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i]; if (i + 1 >= args.size()) throw std::runtime_error("OPTION_VALUE_MISSING"); const std::string v = args[++i];
        if (a == "--codex-home") o.home = v; else if (a == "--data") o.data = v; else if (a == "--endpoint") o.endpoint = v;
        else if (a == "--read-root") o.read_roots.push_back(v);
        else if (a == "--workers") o.workers = unsigned(std::stoul(v));
        else if (a == "--connections") o.connections = unsigned(std::stoul(v));
        else if (a == "--timeout-ms") o.timeout_ms = std::stoull(v);
        else if (a == "--ready-event") o.ready_event = reinterpret_cast<HANDLE>(uintptr_t(std::stoull(v)));
        else throw std::runtime_error("UNKNOWN_OPTION");
    }
    if (o.home.empty() || o.data.empty() || o.endpoint.empty() || !o.workers || o.workers > 64 || !o.connections || o.connections > 64)
        throw std::runtime_error("usage: vrs_ccd --codex-daemon --codex-home DIR --data DIR --endpoint NAME [--workers 6] [--connections 16] [--read-root ROOT]");
    CxDaemon d(std::move(o)); d.start(); d.serve(); return 0;
}
class CxCore { public: explicit CxCore(Options o) : d(std::move(o)) {} CxDaemon d; };
std::shared_ptr<CxCore> cx_core(const std::string& codex_home, const std::string& data, const std::vector<std::string>& read_roots,
                                uint64_t timeout_ms, std::function<vrs_h*()> lib) {
    Options o; o.home = codex_home; o.data = data; o.read_roots = read_roots; o.timeout_ms = timeout_ms;
    auto core = std::make_shared<CxCore>(std::move(o));
    core->d.attach(std::move(lib));
    return core;
}
std::string cx_core_respond(CxCore& core, std::string_view request) { return core.d.respond(request); }
const std::string& cx_core_sessions(const CxCore& core) { return core.d.sessions(); }
#else
int cx_daemon_main(const std::vector<std::string>&) { throw std::runtime_error("CODEX_MODE_WINDOWS_ONLY"); }
class CxCore {};
std::shared_ptr<CxCore> cx_core(const std::string&, const std::string&, const std::vector<std::string>&, uint64_t, std::function<vrs_h*()>) {
    throw std::runtime_error("CODEX_MODE_WINDOWS_ONLY");
}
std::string cx_core_respond(CxCore&, std::string_view) { throw std::runtime_error("CODEX_MODE_WINDOWS_ONLY"); }
const std::string& cx_core_sessions(const CxCore&) { throw std::runtime_error("CODEX_MODE_WINDOWS_ONLY"); }
#endif
}

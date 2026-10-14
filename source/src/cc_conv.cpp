#include "cc_conv.hpp"
#include "swegca_architecture/sha256.hpp"
#include "swegca_architecture/strong_types.hpp"
#include <cstdio>
#include <exception>

namespace cc {
std::string sha256(std::string_view bytes) {
    swegca::architecture::Sha256 h; h.update(bytes); return swegca::architecture::Digest256(h.finish()).hex();
}
void Counts::add(const Counts& other) {
    unknown += other.unknown;
    for (const auto& [k, v] : other.kinds) { auto& d = kinds[k]; d.in += v.in; d.out += v.out; d.emitted += v.emitted; }
}
Json Counts::json() const {
    Json out = Json::obj();
    for (const auto& [k, v] : kinds) {
        Json row = Json::obj(); row.set("included", Json::num(v.in)).set("excluded", Json::num(v.out)).set("emitted", Json::num(v.emitted));
        out.set(k, std::move(row));
    }
    Json r = Json::obj(); r.set("kinds", std::move(out)).set("unknown", Json::num(unknown)); return r;
}
static std::string first_text(const Json& content) {
    if (content.kind == Json::string) return content.scalar;
    if (content.kind == Json::array) for (const auto& b : content.values)
        if (b.get("type") == "text") return b.at("text").text();
    return {};
}
Converted conv(std::string_view original, uint64_t offset, Counts* counts) {
    const Json j = parse(original); const std::string type = j.get("type");
    Converted out; out.timestamp = j.get("timestamp");
    std::string digest;
    auto emit = [&](std::string payload, const std::string& k, const char* event = "response_item") {
        if (out.timestamp.empty()) throw std::runtime_error("TIMESTAMP_MISSING_AT_" + std::to_string(offset));
        if (digest.empty()) digest = sha256(original);
        payload.pop_back();
        payload += ",\"cc\":{\"o\":" + std::to_string(offset) + ",\"n\":" + std::to_string(original.size()) +
            ",\"k\":" + quote(k) + ",\"h\":" + quote(digest) + "}}";
        out.lines.push_back("{\"timestamp\":" + quote(out.timestamp) + ",\"ordinal\":0000000000,\"type\":" + quote(event) + ",\"payload\":" + payload + "}\n");
    };
    auto message = [&](const char* role, const char* block, const std::string& text, const std::string& k) {
        emit("{\"type\":\"message\",\"role\":" + quote(role) + ",\"content\":[{\"type\":" + quote(block) +
             ",\"text\":" + quote(text) + "}]}", k);
    };
    auto dev = [&](const std::string& k) { message("developer", "input_text", "⟨cc⟩", k); };
    auto start = [&] { emit("{\"type\":\"task_started\",\"turn_id\":" + quote(j.at("uuid").text()) + "}", "t", "event_msg"); };
    auto complete = [&](const std::string& text) { emit("{\"type\":\"task_complete\",\"last_agent_message\":" + text + "}", "c", "event_msg"); };
    auto unknown = [&] { if (counts) ++counts->unknown; };
    std::string count_kind = type;
    if (type == "user" || type == "assistant") {
        const Json& msg = j.at("message"); const Json& content = msg.at("content");
        if (type == "user" && j.flag_at("isCompactSummary")) { count_kind += "/compact_summary"; }
        else if (type == "user" && j.flag_at("isMeta")) {
            count_kind += "/meta";
            if (first_text(content).starts_with("Stop hook feedback:")) start();
            dev("0");
        } else if (type == "assistant" && j.flag_at("isApiErrorMessage")) { count_kind += "/api_error"; dev("0"); complete("null"); }
        else {
            Json blocks = content;
            if (content.kind == Json::string) {
                blocks = Json::arr(); Json b = Json::obj(); b.set("type", Json::str("text")).set("text", content); blocks.values.push_back(std::move(b));
            }
            if (blocks.kind != Json::array) throw std::runtime_error("CONTENT_EXPECT_ARRAY_OR_STRING");
            bool started = false;
            for (size_t i = 0; i < blocks.values.size(); ++i) {
                const Json& b = blocks.values[i]; const std::string k = std::to_string(i), bt = b.get("type");
                const size_t prior = out.lines.size();
                if (type == "user") {
                    if (bt == "tool_result") {
                        emit("{\"type\":\"function_call_output\",\"call_id\":" + quote(b.at("tool_use_id").text()) + ",\"output\":\"\"}", k);
                    } else if (bt == "text" || bt == "image") {
                        const std::string text = bt == "text" ? b.at("text").text() : std::string();
                        if (bt == "text" && (text == "[Request interrupted by user]" || text == "[Request interrupted by user for tool use]")) {
                            dev(k); complete("null");
                        } else if (bt == "text" && (text.starts_with("<command-name>") || text.starts_with("<command-message>") ||
                                   text.starts_with("<local-command-stdout>") || text.starts_with("<local-command-caveat>"))) dev(k);
                        else {
                            if (!started) { start(); started = true; }
                            if (bt == "text") message("user", "input_text", text, k);
                            else {
                                std::string media = b.at("source").at("media_type").text();
                                emit("{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":\"input_image\",\"image_url\":" + quote("data:" + media + ";base64,A") + "}]}", k);
                            }
                        }
                    } else unknown();
                } else {
                    if (bt == "text") {
                        const std::string text = b.at("text").text(); message("assistant", "output_text", text, k);
                        const auto* reason = msg.find("stop_reason");
                        if (reason && reason->kind == Json::string && reason->scalar == "end_turn") complete(quote(text));
                    } else if (bt == "tool_use") {
                        emit("{\"type\":\"function_call\",\"name\":" + quote(b.at("name").text()) + ",\"arguments\":\"{}\",\"call_id\":" + quote(b.at("id").text()) + "}", k);
                    } else if (bt == "thinking") {
                        if (!b.get("thinking").empty()) emit("{\"type\":\"reasoning\",\"summary\":[{\"type\":\"summary_text\",\"text\":\"\"}]}", k);
                    } else unknown();
                }
                if (counts) { auto& c = counts->kinds[type + "/" + bt]; (out.lines.size() > prior ? c.in : c.out)++; c.emitted += out.lines.size() - prior; }
            }
        }
    } else if (type == "attachment") {
        const Json& a = j.at("attachment"); count_kind += "/" + a.get("type");
        const auto* origin = a.find("origin"); const auto* rendered = j.find("rendered");
        if (a.get("type") == "queued_command" && origin && origin->get("kind") == "human") {
            // KX2a: the prompt is a string (one input_text, k=0) or a content block list (each block as a user block, k=i).
            const Json& prompt = a.at("prompt");
            if (prompt.kind == Json::string) message("user", "input_text", prompt.scalar, "0");
            else if (prompt.kind == Json::array) for (size_t i = 0; i < prompt.values.size(); ++i) {
                const Json& b = prompt.values[i]; const std::string bt = b.get("type"), k = std::to_string(i);
                if (bt == "text") message("user", "input_text", b.at("text").text(), k);
                else if (bt == "image") {
                    std::string media = b.at("source").at("media_type").text();
                    emit("{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":\"input_image\",\"image_url\":" + quote("data:" + media + ";base64,A") + "}]}", k);
                } else unknown();
            } else throw std::runtime_error("PROMPT_EXPECT_ARRAY_OR_STRING");
        }
        else if (rendered && rendered->kind == Json::array && !rendered->values.empty()) dev("0");
    } else if (type == "system") {
        count_kind += "/" + j.get("subtype"); if (j.get("subtype") == "local_command") dev("0");
    } else unknown();
    if (counts) { auto& c = counts->kinds["line:" + count_kind]; (out.lines.empty() ? c.out : c.in)++; c.emitted += out.lines.size(); }
    return out;
}
void ordinal(std::string& line, uint64_t n) {
    if (n > 9999999999ull || n >= (1u << 24)) throw std::runtime_error("LIB_ORDINAL_LIMIT");
    const size_t at = line.find("\"ordinal\":"); if (at == std::string::npos) throw std::runtime_error("ORDINAL_MISSING");
    char b[11]; std::snprintf(b, sizeof b, "%010llu", (unsigned long long)n); line.replace(at + 10, 10, b, 10);
}

static uint64_t decode_cover(std::string_view bytes) {
    uint64_t value = 0;
    for (size_t i = 0; i < bytes.size(); ++i) value |= uint64_t(uint8_t(bytes[i])) << (8 * i);
    return value;
}
uint64_t cover_max(const std::string& path, EntryFiles* files) {
    std::unique_ptr<File> local;
    auto lease = files ? files->cover.get(path, true, true) : std::shared_ptr<File>{};
    File* file = files ? lease.get() : (local.reset(new File(path, false, true)), local.get());
    if (!file || !file->valid()) return 0;
    // Fixed extent plus one validation byte: no separate size syscall.
    char bytes[cover_slots * 8 + 1]; size_t size = 0;
    while (size < sizeof bytes) { size_t n = file->read(bytes + size, sizeof bytes - size, size); if (!n) break; size += n; }
    if (size > cover_slots * 8) throw std::runtime_error("COVER_SIZE_INVALID");
    uint64_t value = 0;
    for (size_t i = 0; i + 8 <= size; i += 8)
        value = std::max(value, decode_cover(std::string_view(bytes + i, 8)));
    return value;
}
static void cover_written(const std::string& path, unsigned slot, uint64_t end, EntryFiles* files) {
    if (slot >= cover_slots) throw std::runtime_error("COVER_SLOT_RANGE");
    std::unique_ptr<File> local;
    if (!files) { fs::create_directories(vp_io_path(path).parent_path()); local.reset(new File(path, true)); }
    auto lease = files ? files->cover.get(path, true) : std::shared_ptr<File>{};
    File& file = files ? *lease : *local;
    file.cover_extent();
    // Only this resident thread reads/updates this slot. An older parallel group
    // can finish after a newer group on this worker; preserve the larger fact.
    end = std::max(end, decode_cover(file.range(uint64_t(slot) * 8, 8)));
    char bytes[8]; for (unsigned i = 0; i < 8; ++i) bytes[i] = char((end >> (8 * i)) & 255);
    file.write_once(std::string_view(bytes, 8), uint64_t(slot) * 8);
}

Tail prepare_tail(const std::string& transcript, const std::string& rollout, const std::string& cover, EntryFiles* files) {
    Tail tail; tail.transcript = transcript; tail.rollout = rollout; tail.cover = cover; tail.files = files;
    // C3′ / T-cv1: cover must be observed BEFORE the rollout end.
    const uint64_t covered = cover_max(cover, files);
    std::unique_ptr<File> local_source, local_output;
    if (!files) { local_source.reset(new File(transcript)); local_output.reset(new File(rollout, false, true)); }
    auto source_lease = files ? files->source.get(transcript, false) : std::shared_ptr<File>{};
    File& original = files ? *source_lease : *local_source;
    if (files) original.check_source(transcript);
    auto output_lease = files ? files->output.get(rollout, true, true) : std::shared_ptr<File>{};
    File* output = files ? output_lease.get() : local_output.get();
    if (output && output->valid()) {
        ReverseLines reverse(*output, output->size()); std::string line; uint64_t off;
        if (reverse.previous(line, off)) {
            const Json last = parse(line, true); const uint64_t last_ord = last.at("ordinal").uint();
            tail.before = {0, last_ord, off + line.size() + 1};
            if (const auto* c = last.at("payload").find("cc")) {
                const uint64_t o = c->at("o").uint(), n = c->at("n").uint();
                const uint64_t source_size = original.size();
                if (o > source_size || n >= source_size - o) throw std::runtime_error("SOURCE_TRUNCATED");
                if (covered >= o + n + 1) {
                    // The cover proves that this entire source line was written.
                    tail.before.source = o + n + 1;
                } else {
                    const std::string raw = original.range(o, n);
                    if (sha256(raw) != c->at("h").text() || original.range(o + n, 1) != "\n") throw std::runtime_error("SOURCE_SHA_MISMATCH");
                    const auto group = conv(raw, o);
                    // Count only the siblings of this original line. This closes a
                    // crash/concurrent read inside its multi-line image of conv.
                    std::vector<std::string> visible{line + "\n"}; uint64_t begin = off;
                    std::string prev; uint64_t prev_off;
                    while (reverse.previous(prev, prev_off)) {
                        const Json p = parse(prev, true); const auto* pc = p.at("payload").find("cc");
                        if (!pc || pc->at("o").uint() != o) break;
                        visible.push_back(prev + "\n"); begin = prev_off;
                    }
                    if (visible.size() > group.lines.size() || last_ord + 1 < visible.size()) throw std::runtime_error("ROLLOUT_GROUP_INVALID");
                    const uint64_t first_ord = last_ord + 1 - visible.size();
                    for (size_t k = 0; k < visible.size(); ++k) {
                        std::string expected = group.lines[k]; ordinal(expected, first_ord + k);
                        if (expected != visible[visible.size() - k - 1]) throw std::runtime_error("ROLLOUT_BYTES_MISMATCH");
                    }
                    if (visible.size() == group.lines.size()) tail.before.source = o + n + 1;
                    else tail.before = {o, first_ord - 1, begin};
                }
            } else if (last_ord != 0 || last.get("type") != "session_meta") throw std::runtime_error("ROLLOUT_CC_MISSING");
        }
    }
    tail.before.source = std::max(tail.before.source, covered);
    const uint64_t source_size = original.size();
    if (tail.before.source > source_size) throw std::runtime_error("SOURCE_TRUNCATED");
    tail.bytes = original.range(tail.before.source, source_size - tail.before.source);
    const size_t nl = tail.bytes.rfind('\n'); tail.bytes.resize(nl == std::string::npos ? 0 : nl + 1);
    tail.observed_end = tail.before.source + tail.bytes.size(); return tail;
}
struct Batch {
    std::vector<std::string> lines; std::string timestamp; size_t bytes = 0; Counts counts;
};
static Batch build(std::string_view bytes, uint64_t source) {
    Batch b;
    for (size_t a = 0; a < bytes.size();) {
        size_t e = bytes.find('\n', a); if (e == std::string_view::npos) break;
        Converted c = conv(bytes.substr(a, e - a), source + a, &b.counts);
        if (!c.lines.empty() && b.lines.empty()) b.timestamp = c.timestamp;
        for (auto& line : c.lines) { b.bytes += line.size(); b.lines.push_back(std::move(line)); }
        a = e + 1;
    } return b;
}
static void hand(vrs_h* lib, const std::string& path, std::string_view bytes, uint64_t at) {
    if (!lib) return;
    for (size_t a = 0; a < bytes.size();) {
        size_t e = bytes.find('\n', a); if (e == std::string_view::npos) throw std::runtime_error("OUTPUT_INCOMPLETE");
        const int rc = vrs_insert(lib, path.c_str(), at + a, bytes.data() + a, e - a);
        if (rc) throw std::runtime_error("VRS_INSERT_" + std::to_string(rc));
        const int wr = vrs_written(lib, path.c_str(), at + a);
        if (wr && wr != -3) throw std::runtime_error("VRS_WRITTEN_" + std::to_string(wr));
        a = e + 1;
    }
}
static TailResult publish(Tail& tail, std::vector<Batch>& batches, unsigned slot, vrs_h* lib) {
    TailResult result; result.source_end = tail.observed_end; result.output_end = tail.before.output;
    uint64_t count = 0, bytes = 0; std::string ts; bool have_ts = false;
    std::vector<std::pair<uint64_t, uint64_t>> prefixes;
    for (auto& b : batches) {
        prefixes.push_back({count, bytes}); count += b.lines.size(); bytes += b.bytes;
        if (!have_ts && !b.lines.empty()) { ts = b.timestamp; have_ts = true; } result.counts.add(b.counts);
    }
    if (!count) { cover_written(tail.cover, slot, tail.observed_end, tail.files); return result; }
    std::string out; out.reserve(size_t(bytes) + 256);
    if (!tail.before.output) {
        out = "{\"timestamp\":" + quote(ts) + ",\"ordinal\":0000000000,\"type\":\"session_meta\",\"payload\":{\"id\":" +
            quote(vp_u8(vp_path(tail.transcript).stem())) + "}}\n";
    }
    const size_t meta_bytes = out.size();
    for (size_t j = 0; j < batches.size(); ++j) {
        if (out.size() != meta_bytes + prefixes[j].second) throw std::runtime_error("PREFIX_BYTES_INVALID");
        uint64_t ord = tail.before.ordinal + prefixes[j].first;
        for (auto& line : batches[j].lines) { ordinal(line, ++ord); out += line; }
    }
    std::unique_ptr<File> local;
    if (!tail.files) { fs::create_directories(vp_io_path(tail.rollout).parent_path()); local.reset(new File(tail.rollout, true)); }
    auto lease = tail.files ? tail.files->output.get(tail.rollout, true) : std::shared_ptr<File>{};
    File& file = tail.files ? *lease : *local;
    file.write_once(out, tail.before.output);
    // C3′: publish the fact only AFTER the positioned output write completes.
    cover_written(tail.cover, slot, tail.observed_end, tail.files);
    hand(lib, tail.rollout, out, tail.before.output);
    result.output_end += out.size(); result.lines = count; result.bytes = out.size(); return result;
}
TailResult conv_prepared(Tail t, unsigned slot, vrs_h* lib) {
    if (slot >= cover_slots) throw std::runtime_error("COVER_SLOT_RANGE");
    std::vector<Batch> b; b.push_back(build(t.bytes, t.before.source)); return publish(t, b, slot, lib);
}
TailResult conv_tail(const std::string& transcript, const std::string& rollout, const std::string& cover, unsigned slot, vrs_h* lib, EntryFiles* files) {
    return conv_prepared(prepare_tail(transcript, rollout, cover, files), slot, lib);
}

Resident::Resident(unsigned workers, std::function<void(size_t, unsigned)> file_fn) : file_fn_(std::move(file_fn)) {
    if (!workers || workers > 64) throw std::runtime_error("WORKERS_RANGE");
    for (unsigned i = 0; i < workers; ++i) workers_.emplace_back([this, i] {
        for (;;) {
            if (one(i)) continue;
            const auto seen = signal_.load(std::memory_order_seq_cst);
            if (one(i)) continue;
            if (stop_.load(std::memory_order_seq_cst) && !active_.load(std::memory_order_seq_cst)) break;
            sleep_word(signal_, seen);
        }
    });
}
bool Resident::one(unsigned wi) {
    size_t id;
    if (ready_work_.take(id, wi)) {
        ReadyJob* job = ready_jobs_[id].exchange(nullptr, std::memory_order_acq_rel);
        if (!job) return true;
        // The copy keeps predicate data alive after another worker takes the
        // republished pointer. The reservation remains held during dispatch.
        auto ready = job->ready;
        if (!ready()) {
            ready_jobs_[id].store(job, std::memory_order_release);
            // Cover an input notification consumed while the pointer was absent.
            if (ready()) notify_ready(id);
            return true;
        }
        try { job->fn(wi); } catch (const std::exception& e) { std::fprintf(stderr, "resident readiness: %.512s\n", e.what()); }
        delete job;
        ready_reserved_[id].store(false, std::memory_order_release);
        active_.fetch_sub(1, std::memory_order_seq_cst); wake_word(signal_);
        return true;
    }
    if (work_.take(id, wi)) {
        Job* job = jobs_[id].exchange(nullptr, std::memory_order_acq_rel);
        if (job) {
            try { job->fn(wi); } catch (const std::exception& e) { std::fprintf(stderr, "resident: %.512s\n", e.what()); }
            delete job; active_.fetch_sub(1, std::memory_order_seq_cst); wake_word(signal_);
        } return true;
    }
    if (file_fn_ && files_.take(id, wi)) { file_fn_(id, wi); return true; }
    return false;
}
size_t Resident::prepare_ready(std::function<bool()> ready, std::function<void(unsigned)> fn) {
    auto job = std::make_unique<ReadyJob>(ReadyJob{std::move(ready), std::move(fn)});
    const size_t start = (uintptr_t(job.get()) >> 4) % ready_jobs_.size();
    for (size_t i = 0; i < ready_jobs_.size(); ++i) {
        const size_t at = (start + i) % ready_jobs_.size(); bool empty = false;
        if (!ready_reserved_[at].compare_exchange_strong(empty, true, std::memory_order_acq_rel)) continue;
        active_.fetch_add(1, std::memory_order_seq_cst);
        ready_jobs_[at].store(job.release(), std::memory_order_release);
        return at;
    }
    return no_ready_slot;
}
void Resident::notify_ready(size_t id) { ready_work_.add(id); wake_word(signal_); }
void Resident::submit(std::function<void(unsigned)> fn, unsigned submitter_slot) {
    auto* job = new Job{std::move(fn)}; active_.fetch_add(1, std::memory_order_seq_cst);
    const size_t start = (uintptr_t(job) >> 4) % jobs_.size();
    for (size_t i = 0; i < jobs_.size(); ++i) {
        const size_t at = (start + i) % jobs_.size(); Job* empty = nullptr;
        if (jobs_[at].compare_exchange_strong(empty, job, std::memory_order_acq_rel)) {
            work_.add(at); wake_word(signal_); return;
        }
    }
    // Finite capacity: the submitter performs this independent piece itself.
    try { job->fn(submitter_slot); } catch (...) { delete job; active_.fetch_sub(1); wake_word(signal_); throw; }
    delete job; active_.fetch_sub(1); wake_word(signal_);
}
void Resident::mark(size_t id) { if (id >= 65536) throw std::runtime_error("FILE_ID_RANGE"); files_.add(id); wake_word(signal_); }
void Resident::drain() {
    for (;;) { auto seen = signal_.load(); if (!active_.load()) return; sleep_word(signal_, seen); }
}
void Resident::stop_and_join() {
    // Producers have ended. A running file callback can still publish pieces;
    // its own resident thread continues until that continuation has completed.
    stop_.store(true); wake_word(signal_); for (auto& t : workers_) if (t.joinable()) t.join();
}
Resident::~Resident() { stop_and_join(); }
void conv_parallel(Tail tail, Resident& resident, unsigned submitter_slot, vrs_h* lib,
                   std::function<void(TailResult, std::string)> finished, size_t chunk_bytes) {
    if (!chunk_bytes) throw std::runtime_error("CHUNK_SIZE_ZERO");
    struct Group {
        Tail tail; std::vector<std::pair<size_t, size_t>> spans; std::vector<Batch> batches;
        std::vector<std::string> errors; std::atomic<size_t> left{0};
        std::atomic<bool> published{false};
        std::function<void(TailResult, std::string)> done;
    };
    auto g = std::make_shared<Group>(); g->tail = std::move(tail); g->done = std::move(finished);
    for (size_t a = 0; a < g->tail.bytes.size();) {
        size_t e = std::min(g->tail.bytes.size(), a + chunk_bytes);
        if (e < g->tail.bytes.size()) { size_t nl = g->tail.bytes.find('\n', e - 1); e = nl == std::string::npos ? g->tail.bytes.size() : nl + 1; }
        g->spans.push_back({a, e}); a = e;
    }
    if (g->spans.empty()) g->spans.push_back({0, 0});
    g->batches.resize(g->spans.size()); g->errors.resize(g->spans.size()); g->left.store(g->spans.size());
    const size_t check = resident.prepare_ready(
        [g] { return g->left.load(std::memory_order_acquire) == 0; },
        [g, lib](unsigned writer_slot) {
            bool no = false;
            if (!g->published.compare_exchange_strong(no, true, std::memory_order_acq_rel)) return;
            for (const auto& error : g->errors) if (!error.empty()) { g->done({}, error); return; }
            try { g->done(publish(g->tail, g->batches, writer_slot, lib), {}); }
            catch (const std::exception& e) { g->done({}, e.what()); }
        });
    if (check == Resident::no_ready_slot) {
        // Admission fallback precedes all piece work; no completer runs a successor.
        try { g->done(conv_prepared(std::move(g->tail), submitter_slot, lib), {}); }
        catch (const std::exception& e) { g->done({}, e.what()); }
        return;
    }
    for (size_t i = 0; i < g->spans.size(); ++i) resident.submit([g, i, check, &resident](unsigned) {
        try {
            auto [a, e] = g->spans[i];
            g->batches[i] = build(std::string_view(g->tail.bytes).substr(a, e - a), g->tail.before.source + a);
        } catch (const std::exception& e) { g->errors[i] = e.what(); }
        g->left.fetch_sub(1, std::memory_order_acq_rel);
        resident.notify_ready(check);
    }, submitter_slot);
}
}

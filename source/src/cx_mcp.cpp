// VRS_MCP_codexapp_v1.md 7.4: the stdio MCP server of the Codex mode. One app-server starts one of these and shares it between its
// threads, so requests are handed to resident workers through free slots (one atomic exchange each), and every answer is one WriteFile
// of one line (P-X5). A call names its thread by params._meta.threadId; nothing is guessed when it is missing.
#include "cx_mode.hpp"
#include "cc_io.hpp"
#include <array>
#include <atomic>
#include <set>
#include <thread>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace cc {
#ifdef _WIN32
namespace {
struct BridgeError : std::runtime_error { using std::runtime_error::runtime_error; };
std::string env(const char* k) { const char* v = std::getenv(k); return v ? v : ""; }
std::string codex_home() {
    auto h = env("CODEX_HOME"); if (!h.empty()) return h;
    h = env("USERPROFILE"); if (h.empty()) throw BridgeError("HOME_UNAVAILABLE");
    return h + "/.codex";
}
std::string cut(std::string s, size_t n = 2000) {
    if (s.size() <= n) return s;
    while (n && (static_cast<unsigned char>(s[n]) & 0xc0) == 0x80) --n;
    s.resize(n); return s;
}
std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n); return w;
}
std::string narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0'); WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr); return s;
}
std::wstring quote_arg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring o = L"\""; size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'"') { o.append(slashes * 2 + 1, L'\\'); o += c; slashes = 0; continue; }
        o.append(slashes, L'\\'); slashes = 0; o += c;
    }
    o.append(slashes * 2, L'\\'); o += L'"'; return o;
}

class Client {
    std::string home_, data_, endpoint_;
    std::vector<std::string> daemon_args_;  // VRS_onePC_v1.md B7: the one daemon of this PC
    std::atomic<bool> greeted_{false};
public:
    Client() {
        home_ = codex_home();
        PcDaemon d = pc_daemon("VRS_CX_DATA", "VRS_CX_ENDPOINT");
        data_ = d.data; endpoint_ = d.endpoint; daemon_args_ = std::move(d.args);
    }
    // The daemon is started detached. It inherits one event and nothing else (not this process's stdio), and sets the event once
    // the pipe name is served; with wait the caller waits for that fact or for the daemon to end, at most 5 s.
    void spawn(bool wait) {
        std::string exe = env("VRS_CX_DAEMON");
        if (exe.empty()) {
            std::wstring self(32768, L'\0'); DWORD n = GetModuleFileNameW(nullptr, self.data(), DWORD(self.size()));
            if (!n || n >= self.size()) throw BridgeError("DAEMON_BINARY_NOT_FOUND");
            self.resize(n); exe = narrow(self);
        }
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE ready = CreateEventW(&sa, TRUE, FALSE, nullptr);
        if (!ready) throw BridgeError("DAEMON_EVENT_FAILED");
        struct Close { HANDLE h; ~Close() { if (h) CloseHandle(h); } } ready_guard{ready};
        std::vector<std::string> args{exe};
        args.insert(args.end(), daemon_args_.begin(), daemon_args_.end());
        args.push_back("--ready-event"); args.push_back(std::to_string(uintptr_t(ready)));
        std::wstring line;
        for (const auto& a : args) { if (!line.empty()) line += L' '; line += quote_arg(wide(a)); }
        SIZE_T bytes = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        std::vector<char> list(bytes);
        auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list.data());
        if (!InitializeProcThreadAttributeList(attrs, 1, 0, &bytes)) throw BridgeError("DAEMON_ATTRIBUTES_FAILED");
        struct Del { LPPROC_THREAD_ATTRIBUTE_LIST a; ~Del() { DeleteProcThreadAttributeList(a); } } del{attrs};
        if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &ready, sizeof ready, nullptr, nullptr))
            throw BridgeError("DAEMON_ATTRIBUTES_FAILED");
        STARTUPINFOEXW si{}; si.StartupInfo.cb = sizeof si; si.lpAttributeList = attrs; PROCESS_INFORMATION pi{};
        if (!CreateProcessW(wide(exe).c_str(), line.data(), nullptr, nullptr, TRUE,
                            DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &si.StartupInfo, &pi))
            throw BridgeError("DAEMON_SPAWN_FAILED_" + std::to_string(GetLastError()));
        CloseHandle(pi.hThread);
        if (wait) { HANDLE hs[2] = {ready, pi.hProcess}; WaitForMultipleObjects(2, hs, FALSE, 5000); }
        CloseHandle(pi.hProcess);
    }
    HANDLE open() { return CreateFileW(wide(endpoint_).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr); }
    HANDLE connect() {
        HANDLE h = open();
        if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY && WaitNamedPipeW(wide(endpoint_).c_str(), 5000)) h = open();
        return h;
    }
    void start_early() {
        HANDLE h = open();
        if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return; }
        if (GetLastError() == ERROR_PIPE_BUSY) return;
        try { spawn(false); } catch (const std::exception&) {}  // a tool call reports the same failure by name
    }
    // VRS_onePC_v1.md B7: before the first request, the daemon on the pipe must be the one daemon (proto 2) with the app root
    Json call(const Json& request) {
        if (!greeted_.load(std::memory_order_acquire)) {
            Json hello = Json::obj(); hello.set("op", Json::str("hello"));
            Json r;
            try { r = exchange(hello); }
            catch (const BridgeError& e) { if (std::string(e.what()) == "UNKNOWN_OPERATION") throw BridgeError("OLD_DAEMON_ON_PIPE"); throw; }
            const Json* p = r.find("proto");
            if (!p || p->kind != Json::number || p->scalar != "2") throw BridgeError("DAEMON_PROTO_MISMATCH");
            bool app = false;
            if (const Json* roots = r.find("roots"); roots && roots->kind == Json::array)
                for (const auto& t : roots->values) app = app || (t.kind == Json::string && t.scalar == "_cx/");
            if (!app) throw BridgeError("CODEX_ROOT_ABSENT");
            greeted_.store(true, std::memory_order_release);
        }
        return exchange(request);
    }
    Json exchange(const Json& request) {
        HANDLE h = connect();
        if (h == INVALID_HANDLE_VALUE) { spawn(true); h = connect(); }
        if (h == INVALID_HANDLE_VALUE) throw BridgeError("DAEMON_CONNECT_FAILED_" + std::to_string(GetLastError()));
        struct Close { HANDLE h; ~Close() { CloseHandle(h); } } guard{h};
        const std::string data = request.dump() + "\n";
        for (size_t at = 0; at < data.size();) {
            DWORD put = 0;
            if (!WriteFile(h, data.data() + at, DWORD(data.size() - at), &put, nullptr) || !put) throw BridgeError("DAEMON_WRITE_FAILED");
            at += put;
        }
        std::string got; std::string buf(65536, '\0');
        while (got.size() <= 8 * 1024 * 1024) {
            DWORD n = 0;
            if (!ReadFile(h, buf.data(), DWORD(buf.size()), &n, nullptr) || !n) throw BridgeError("DAEMON_INCOMPLETE_REPLY");
            got.append(buf.data(), n);
            const size_t nl = got.find('\n');
            if (nl != std::string::npos) {
                Json reply = parse(std::string_view(got).substr(0, nl));
                if (!reply.flag_at("ok")) throw BridgeError(reply.find("error") ? reply.at("error").text() : "DAEMON_ERROR");
                return reply;
            }
        }
        throw BridgeError("DAEMON_REPLY_TOO_LARGE");
    }
};

Json text_result(const std::string& text, bool error) {
    Json item = Json::obj(); item.set("type", Json::str("text")); item.set("text", Json::str(text));
    Json content = Json::arr(); content.values.push_back(std::move(item));
    Json r = Json::obj(); r.set("content", std::move(content)); r.set("isError", Json::flag(error)); return r;
}
Json invoke(Client& client, const Json& params) {
    try {
        const Json* name = params.find("name"); const Json* given = params.find("arguments");
        Json args = given && given->kind != Json::null ? *given : Json::obj();
        if (args.kind != Json::object) throw BridgeError("ARGUMENTS_EXPECT_OBJECT");
        const std::string tool = name && name->kind == Json::string ? name->scalar : "";
        std::set<std::string> keys(args.keys.begin(), args.keys.end());
        Json request = args;
        if (tool == "vrs_restore") {
            for (const auto& k : keys) if (k != "budget" && k != "start") throw BridgeError("UNKNOWN_ARGUMENT");
            const Json* meta = params.find("_meta");
            const Json* thread = meta && meta->kind == Json::object ? meta->find("threadId") : nullptr;
            if (!thread || thread->kind != Json::string || thread->scalar.empty()) throw BridgeError("THREAD_UNKNOWN");
            request.set("op", Json::str("restore")); request.set("thread", *thread);
        } else if (tool == "vrs_read") {
            for (const auto& k : keys) if (k != "path" && k != "start" && k != "end" && k != "budget") throw BridgeError("READ_ARGUMENTS_INVALID");
            if (!keys.count("path") || !keys.count("start") || !keys.count("end")) throw BridgeError("READ_ARGUMENTS_INVALID");
            request.set("op", Json::str("read"));
        } else throw BridgeError("UNKNOWN_TOOL");
        return text_result(client.call(request).at("text").text(), false);
    } catch (const std::exception& e) {
        const std::string m = e.what();
        return text_result(m == "NOT_READY" ? mcp_not_ready_text : cut(m), true);
    }
}
std::string response_line(const Json* id, Json result, const std::exception* failure) {
    Json response = Json::obj(); response.set("jsonrpc", Json::str("2.0")); response.set("id", id ? *id : Json{});
    if (failure) {
        const bool bridge = dynamic_cast<const BridgeError*>(failure) != nullptr;
        Json err = Json::obj(); err.set("code", Json{}); err.values.back().kind = Json::number;
        err.values.back().scalar = bridge ? "-32601" : "-32700"; err.set("message", Json::str(cut(failure->what())));
        response.set("error", std::move(err));
    } else response.set("result", std::move(result));
    return response.dump() + "\n";
}
// One line, one WriteFile: a synchronous handle completes one request at a time per file object, so lines do not mix (P-X5, C7).
void write_line(const std::string& s) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    for (size_t at = 0; at < s.size();) {
        DWORD put = 0; if (!WriteFile(out, s.data() + at, DWORD(s.size() - at), &put, nullptr) || !put) return; at += put;
    }
}

struct Slot { std::atomic<std::string*> request{nullptr}; std::atomic<uint32_t> word{0}; };
std::array<Slot, 64> slots;
std::atomic<uint32_t> barrier_arrived{0};

void worker(Client& client, Slot& slot, unsigned barrier, const std::string& hold) {
    for (;;) {
        const uint32_t seen = slot.word.load(std::memory_order_seq_cst);
        std::string* line = slot.request.load(std::memory_order_seq_cst);
        if (!line) { sleep_word(slot.word, seen); continue; }
        if (!hold.empty()) {  // C7 test hook: the worker holds its slot until the test sets the named event
            HANDLE e = OpenEventW(SYNCHRONIZE, FALSE, wide(hold).c_str());
            if (e) { WaitForSingleObject(e, INFINITE); CloseHandle(e); }
        }
        std::string out;
        try {
            const Json request = parse(*line); const Json* pp = request.find("params"); const Json params = pp ? *pp : Json::obj();
            out = response_line(request.find("id"), invoke(client, params), nullptr);
        } catch (const std::exception& e) { out = response_line(nullptr, Json{}, &e); }
        if (barrier) {  // C7 test hook: every worker reaches its write at once
            barrier_arrived.fetch_add(1, std::memory_order_seq_cst); wake_word(barrier_arrived);
            for (uint32_t n; (n = barrier_arrived.load(std::memory_order_seq_cst)) < barrier;) sleep_word(barrier_arrived, n);
        }
        write_line(out);
        delete line; slot.request.store(nullptr, std::memory_order_seq_cst);
    }
}
}  // namespace

int cx_mcp_main() {
    _setmode(_fileno(stdin), _O_BINARY); _setmode(_fileno(stdout), _O_BINARY);
    Client client;
    const Json tools = parse(mcp_tools_json);
    unsigned w = 8;
    if (auto v = env("VRS_CX_WORKERS"); !v.empty()) w = unsigned(std::stoul(v));
    if (!w || w > slots.size()) throw std::runtime_error("VRS_CX_WORKERS_1_TO_64");
    const unsigned barrier = env("VRS_CX_TEST_BARRIER").empty() ? 0 : unsigned(std::stoul(env("VRS_CX_TEST_BARRIER")));
    const std::string hold = env("VRS_CX_TEST_HOLD_EVENT");
    for (unsigned i = 0; i < w; ++i) std::thread(worker, std::ref(client), std::ref(slots[i]), barrier, hold).detach();
    auto handle = [&](const std::string& line) {
        Json request; bool have = false;
        try {
            request = parse(line); have = true;
            if (request.kind != Json::object) throw std::runtime_error("JSONRPC_EXPECT_OBJECT");
            if (!request.find("id")) return;  // a notification
            const std::string method = request.find("method") && request.at("method").kind == Json::string ? request.at("method").scalar : "";
            const Json* pp = request.find("params"); const Json params = pp ? *pp : Json::obj();
            Json result = Json::obj();
            if (method == "tools/call") {
                auto* owned = new std::string(line);
                for (unsigned i = 0; i < w; ++i) {
                    std::string* none = nullptr;
                    if (slots[i].request.compare_exchange_strong(none, owned, std::memory_order_seq_cst)) { wake_word(slots[i].word); return; }
                }
                delete owned;
                write_line(response_line(request.find("id"), text_result("BUSY: every vrs-lib worker is answering another call. Call again.", true), nullptr));
                return;
            }
            if (method == "initialize") {
                const Json* v = params.find("protocolVersion");
                result.set("protocolVersion", v ? *v : Json::str("2025-03-26"));
                Json caps = Json::obj(); caps.set("tools", Json::obj()); result.set("capabilities", std::move(caps));
                Json info = Json::obj(); info.set("name", Json::str("vrs-lib")); info.set("version", Json::str("1.1.0"));
                result.set("serverInfo", std::move(info));
                client.start_early();
            } else if (method == "ping") {
            } else if (method == "tools/list") result = tools;
            else throw BridgeError("METHOD_NOT_FOUND");
            write_line(response_line(request.find("id"), std::move(result), nullptr));
        } catch (const std::exception& e) {
            write_line(response_line(have && request.kind == Json::object ? request.find("id") : nullptr, Json{}, &e));
        }
    };
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    std::string pending; std::string buf(65536, '\0'); DWORD n = 0;
    while (ReadFile(in, buf.data(), DWORD(buf.size()), &n, nullptr) && n) {
        pending.append(buf.data(), n);
        for (size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
            handle(pending.substr(0, nl)); pending.erase(0, nl + 1);
        }
    }
    if (!pending.empty()) handle(pending);
    return 0;
}
#else
int cx_mcp_main() { throw std::runtime_error("CODEX_MODE_WINDOWS_ONLY"); }
#endif
}

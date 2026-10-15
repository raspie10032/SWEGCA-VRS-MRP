#include "cc_mcp.hpp"
#include "cx_mode.hpp"
#include "cc_io.hpp"
#include "vrs_witness.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <set>
#include <thread>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace cc {
namespace {
struct BridgeError : std::runtime_error { using std::runtime_error::runtime_error; };

std::string env(const char* k) { const char* v = std::getenv(k); return v ? v : ""; }

// The writer hook's home: CLAUDE_CONFIG_DIR, else <profile>/.claude.
std::string claude_home() {
    auto h = env("CLAUDE_CONFIG_DIR"); if (!h.empty()) return h;
#ifdef _WIN32
    h = env("USERPROFILE");
#else
    h = env("HOME");
#endif
    if (h.empty()) throw BridgeError("HOME_UNAVAILABLE");
    return h + "/.claude";
}
#ifdef _WIN32
// VRS_onePC_v1.md B7: the home of this PC's one daemon, the same for hooks and both bridges: VRS_HOME, else %LOCALAPPDATA%/vrs-lib.
std::string pc_home() {
    auto h = env("VRS_HOME"); if (!h.empty()) return h;
    h = env("LOCALAPPDATA"); if (h.empty()) throw BridgeError("LOCALAPPDATA_UNAVAILABLE");
    return h + "/vrs-lib";
}
std::string data_dir() {
    auto d = env("VRS_CC_DATA"); if (!d.empty()) return d;
    return pc_home();
}
#else
// Hooks and the MCP server of one plugin both receive CLAUDE_PLUGIN_DATA, so they agree on one data directory.
std::string data_dir() {
    for (const char* k : {"VRS_CC_DATA", "CLAUDE_PLUGIN_DATA"}) { auto d = env(k); if (!d.empty()) return d; }
    return claude_home() + "/vrs-lib";
}
#endif

std::string read_all_stdin() {
    std::string input; char buf[4096];
    while (size_t n = std::fread(buf, 1, sizeof buf, stdin)) {
        input.append(buf, n); if (input.size() > 1024 * 1024) throw std::runtime_error("HOOK_INPUT_TOO_LARGE");
    }
    return input;
}

// str(exc)[:2000] of the Python bridge, cut on a UTF-8 boundary.
std::string cut(std::string s, size_t n = 2000) {
    if (s.size() <= n) return s;
    while (n && (static_cast<unsigned char>(s[n]) & 0xc0) == 0x80) --n;
    s.resize(n); return s;
}

const char* const GUIDE =
    "이 문맥은 압축되었다. 위의 압축 요약은 원문이 아니며 빠지거나 바뀐 내용이 있을 수 있다. "
    "압축 전 대화의 사실·결정·수치·사용자 원문에 기대어 답하거나 행동하기 전에는, 먼저 vrs-lib 의 vrs_restore 도구(인자 없음)로 "
    "압축으로 빠진 원문(사용자 발화·답)을 받아 확인하라. 결과가 여러 쪽이면 next 값을 start 로 넘겨 이어 받는다. "
    "항목에 붙은 원본 위치의 바이트 범위는 vrs_read 로 읽을 수 있다.";

const char* const TOOLS = R"({"tools":[{"name":"vrs_restore","description":"Restore selected original user utterances and assistant answers before this session's last compaction. Pass next as start for another page.","inputSchema":{"type":"object","properties":{"budget":{"type":"integer","minimum":1,"maximum":95000,"default":48000,"description":"Maximum UTF-16 units of the full rendered result."},"start":{"type":"string","pattern":"^[0-9]+:[0-9]+(?::[0-9]+)?$","description":"Original o:k:c position; c is a UTF-16 body offset. Legacy o:k means c=0."}},"additionalProperties":false},"_meta":{"anthropic/maxResultSizeChars":100000}},{"name":"vrs_read","description":"Read an exact UTF-8 byte range under the allowed roots, with SHA-256. Pages end at line boundaries.","inputSchema":{"type":"object","properties":{"path":{"type":"string"},"start":{"type":"integer","minimum":0},"end":{"type":"integer","minimum":0},"budget":{"type":"integer","minimum":1,"maximum":95000,"default":48000,"description":"Maximum UTF-16 units of the full rendered result."}},"required":["path","start","end"],"additionalProperties":false},"_meta":{"anthropic/maxResultSizeChars":100000}}]})";

const char* const NOT_READY_TEXT =
    "NOT_READY: the vrs-lib index is still being prepared (its daemon has just started). "
    "This does not mean the originals are missing. Call the same tool again with the same arguments in a few seconds.";

#ifdef _WIN32
std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n); return w;
}
std::string narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0'); WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr); return s;
}
// subprocess.list2cmdline quoting.
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

// [0-9a-fA-F]{8}(-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}, without std::regex (its locale guard would add a lock symbol).
bool is_uuid(const std::string& s) {
    if (s.size() != 36) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return false; }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}
// Python start_matches: the native FILETIME ticks, the .NET DateTime ticks, or the one-second UTC asctime.
bool start_matches(const Json* recorded, uint64_t birth) {
    if (!recorded || (recorded->kind != Json::string && recorded->kind != Json::number)) return false;
    const std::string& text = recorded->scalar;
    if (text == std::to_string(birth) || text == std::to_string(birth + 504911232000000000ull)) return true;
    const std::time_t epoch = std::time_t(birth / 10000000ull) - std::time_t(11644473600ll);
    std::tm t{}; if (gmtime_s(&t, &epoch)) return false;
    static const char* const days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s %s %2d %02d:%02d:%02d %d", days[t.tm_wday], months[t.tm_mon], t.tm_mday,
                  t.tm_hour, t.tm_min, t.tm_sec, t.tm_year + 1900);
    return text == buf;
}

class Client {
    std::string projects_, data_, endpoint_;
    std::vector<std::string> daemon_args_;  // VRS_onePC_v1.md B7
    std::map<std::string, std::string> transcripts_;  // session id -> transcript, as the Python Identity cache
public:
    Client() {
        projects_ = env("VRS_CC_PROJECTS"); if (projects_.empty()) projects_ = claude_home() + "/projects";
        PcDaemon d = pc_daemon("VRS_CC_DATA", "VRS_CC_ENDPOINT");
        data_ = d.data; endpoint_ = d.endpoint; daemon_args_ = std::move(d.args);
    }
    std::string transcript() {
        const std::string sessions = claude_home() + "/sessions";
        uint64_t pid = GetCurrentProcessId();
        Json record; bool found = false;
        for (int k = 0; k < 4; ++k) {
            pid = vw::process(pid).parent;
            if (!pid) break;
            File f(sessions + "/" + std::to_string(pid) + ".json", false, true);
            if (f.valid()) {
                if (f.size() > 1024 * 1024) throw BridgeError("SESSION_RECORD_TOO_LARGE");
                record = parse(f.range(0, f.size())); found = true; break;
            }
        }
        if (!found) throw BridgeError("CALLER_SESSION_NOT_FOUND_IN_4_PARENTS");
        const Json* rp = record.find("pid");
        if (!rp || rp->kind != Json::number || rp->scalar != std::to_string(pid)) throw BridgeError("SESSION_PID_MISMATCH");
        if (!start_matches(record.find("procStart"), vw::process(pid).birth)) throw BridgeError("SESSION_PROC_START_MISMATCH");
        const Json* sp = record.find("sessionId");
        if (!sp || sp->kind != Json::string || !is_uuid(sp->scalar)) throw BridgeError("SESSION_ID_INVALID");
        const std::string sid = sp->scalar, root = canonical(projects_);
        auto it = transcripts_.find(sid);
        if (it == transcripts_.end()) {
            std::vector<std::string> hits; std::error_code ec;
            for (fs::directory_iterator d(vp_io_path(root), ec), end; !ec && d != end; d.increment(ec)) {
                std::error_code e2;
                if (d->is_directory(e2) && fs::is_regular_file(d->path() / (sid + ".jsonl"), e2))
                    hits.push_back(vp_u8(d->path() / (sid + ".jsonl")));
            }
            if (hits.size() != 1) throw BridgeError("SESSION_TRANSCRIPT_NOT_UNIQUE");
            it = transcripts_.emplace(sid, canonical(hits[0])).first;
        }
        const std::string path = canonical(it->second);
        if (!under(path, root)) throw BridgeError("SESSION_TRANSCRIPT_OUTSIDE_ROOT");
        return path;
    }
    void spawn() {
        std::string exe = env("VRS_CC_DAEMON");
        if (exe.empty()) {
            std::wstring self(32768, L'\0'); DWORD n = GetModuleFileNameW(nullptr, self.data(), DWORD(self.size()));
            if (!n || n >= self.size()) throw BridgeError("DAEMON_BINARY_NOT_FOUND");
            self.resize(n); exe = narrow(self);
        }
        if (GetFileAttributesW(wide(exe).c_str()) == INVALID_FILE_ATTRIBUTES) throw BridgeError("DAEMON_BINARY_NOT_FOUND");
        std::vector<std::string> args{exe};  // VRS_onePC_v1.md B7: the one daemon, started the same way by either bridge
        args.insert(args.end(), daemon_args_.begin(), daemon_args_.end());
        std::wstring line;
        for (const auto& a : args) { if (!line.empty()) line += L' '; line += quote_arg(wide(a)); }
        STARTUPINFOW si{}; si.cb = sizeof si; PROCESS_INFORMATION pi{};
        if (!CreateProcessW(wide(exe).c_str(), line.data(), nullptr, nullptr, FALSE,
                            DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi))
            throw BridgeError("DAEMON_SPAWN_FAILED_" + std::to_string(GetLastError()));
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    HANDLE connect() {
        return CreateFileW(wide(endpoint_).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    // D-KM6a: probe once at initialize, spawn if absent, never wait.
    void start_early() {
        HANDLE h = connect();
        if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return; }
        try { spawn(); } catch (const std::exception&) {}  // a tool call reports the same failure by name
    }
    Json call(const Json& request) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        bool spawned = false; HANDLE h;
        while ((h = connect()) == INVALID_HANDLE_VALUE) {
            if (!spawned) { spawn(); spawned = true; }
            if (std::chrono::steady_clock::now() >= deadline) throw BridgeError("DAEMON_CONNECT_TIMEOUT_5S");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        struct Close { HANDLE h; ~Close() { CloseHandle(h); } } guard{h};
        const std::string data = request.dump() + "\n";
        for (size_t at = 0; at < data.size();) {
            DWORD put = 0;
            if (!WriteFile(h, data.data() + at, DWORD(data.size() - at), &put, nullptr) || !put) throw BridgeError("DAEMON_WRITE_FAILED");
            at += put;
        }
        std::string got; char buf[65536];
        while (got.size() <= 8 * 1024 * 1024) {
            DWORD n = 0;
            if (!ReadFile(h, buf, sizeof buf, &n, nullptr) || !n) throw BridgeError("DAEMON_INCOMPLETE_REPLY");
            got.append(buf, n);
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
        Json args = given ? *given : Json::obj();
        if (args.kind != Json::object) throw BridgeError("ARGUMENTS_EXPECT_OBJECT");
        const std::string tool = name && name->kind == Json::string ? name->scalar : "";
        std::set<std::string> keys(args.keys.begin(), args.keys.end());
        Json request = args;
        if (tool == "vrs_restore") {
            for (const auto& k : keys) if (k != "budget" && k != "start") throw BridgeError("UNKNOWN_ARGUMENT");
            request.set("op", Json::str("restore")); request.set("transcript", Json::str(client.transcript()));
        } else if (tool == "vrs_read") {
            for (const auto& k : keys) if (k != "path" && k != "start" && k != "end" && k != "budget") throw BridgeError("READ_ARGUMENTS_INVALID");
            if (!keys.count("path") || !keys.count("start") || !keys.count("end")) throw BridgeError("READ_ARGUMENTS_INVALID");
            request.set("op", Json::str("read"));
        } else throw BridgeError("UNKNOWN_TOOL");
        return text_result(client.call(request).at("text").text(), false);
    } catch (const std::exception& e) {
        const std::string m = e.what();
        return text_result(m == "NOT_READY" ? NOT_READY_TEXT : cut(m), true);
    }
}
#endif
}  // namespace

const char* const mcp_tools_json = TOOLS;
const char* const mcp_not_ready_text = NOT_READY_TEXT;

#ifdef _WIN32
// VRS_onePC_v1.md B7. The old data is named only when the home is the default one: a test (or any other home) never moves anything.
PcDaemon pc_daemon(const char* legacy_data_env, const char* legacy_endpoint_env) {
    PcDaemon d;
    const bool given_home = !env(legacy_data_env).empty() || !env("VRS_HOME").empty() ||
                            !env(legacy_endpoint_env).empty() || !env("VRS_ENDPOINT").empty();  // a home or a pipe of its own: never migrate
    d.data = env(legacy_data_env); if (d.data.empty()) d.data = pc_home();
    d.endpoint = env(legacy_endpoint_env); if (d.endpoint.empty()) d.endpoint = env("VRS_ENDPOINT");
    if (d.endpoint.empty()) {
        std::string user;
        for (const char* k : {"LOGNAME", "USER", "LNAME", "USERNAME"}) if (!(user = env(k)).empty()) break;
        d.endpoint = "\\\\.\\pipe\\vrs-lib-" + user;
    }
    auto is_dir = [](const std::string& p) { std::error_code ec; return fs::is_directory(vp_io_path(p), ec); };
    std::string projects = env("VRS_CC_PROJECTS"); if (projects.empty()) projects = claude_home() + "/projects";
    std::string codex = env("CODEX_HOME");
    if (codex.empty()) { const auto p = env("USERPROFILE"); if (!p.empty()) codex = p + "/.codex"; }
    if (is_dir(projects)) { d.args.push_back("--projects"); d.args.push_back(projects); }
    if (!codex.empty() && is_dir(codex + "/sessions")) { d.args.push_back("--codex-home"); d.args.push_back(codex); }
    d.args.insert(d.args.end(), {"--data", d.data, "--endpoint", d.endpoint});
    for (const char* k : {"VRS_CC_READ_ROOTS", "VRS_CX_READ_ROOTS"}) {
        const std::string roots = env(k);
        for (size_t a = 0; a <= roots.size();) {
            size_t b = roots.find(';', a); if (b == std::string::npos) b = roots.size();
            if (b > a) { d.args.push_back("--read-root"); d.args.push_back(roots.substr(a, b - a)); }
            a = b + 1;
        }
    }
    if (!given_home) {
        std::string old_claude = env("CLAUDE_PLUGIN_DATA"); if (old_claude.empty()) old_claude = claude_home() + "/plugins/data/vrs-lib-vrs-lib";
        if (is_dir(old_claude)) { d.args.push_back("--migrate-claude"); d.args.push_back(old_claude); }
        if (!codex.empty() && is_dir(codex + "/vrs-lib")) { d.args.push_back("--migrate-codex"); d.args.push_back(codex + "/vrs-lib"); }
    }
    return d;
}
#else
PcDaemon pc_daemon(const char*, const char*) { throw std::runtime_error("ONE_PC_DAEMON_WINDOWS_ONLY"); }
#endif

int writer_hook(const std::string& dir, const std::string& host) {
    const auto j = parse(read_all_stdin());
    const auto source = j.at("transcript_path").text();
    std::string home;
    if (host == "claude") home = claude_home();
    const auto w = vw::hook_owner(source, j.at("session_id").text(), host, home);
    fs::create_directories(vp_io_path(dir)); vw::publish(canonical(dir), w); return 0;
}

int hook_main(const std::string& which) {
    if (which == "writer") return writer_hook(data_dir() + "/writer-markers", "claude");
    if (which == "compact") {
        Json inner = Json::obj();
        inner.set("hookEventName", Json::str("SessionStart")); inner.set("additionalContext", Json::str(GUIDE));
        Json out = Json::obj(); out.set("hookSpecificOutput", std::move(inner));
        const std::string s = out.dump() + "\n";
        std::fwrite(s.data(), 1, s.size(), stdout); std::fflush(stdout); return 0;
    }
    throw std::runtime_error("usage: vrs_ccd --hook writer|compact");
}

int mcp_main() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY); _setmode(_fileno(stdout), _O_BINARY);
    Client client;
    const Json tools = parse(TOOLS);
    auto handle = [&](const std::string& line) {
        Json request; bool have = false; Json response = Json::obj();
        try {
            request = parse(line); have = true;
            if (request.kind != Json::object) throw std::runtime_error("JSONRPC_EXPECT_OBJECT");
            if (!request.find("id")) return;
            const std::string method = request.find("method") && request.at("method").kind == Json::string ? request.at("method").scalar : "";
            const Json* pp = request.find("params"); const Json params = pp ? *pp : Json::obj();
            Json result = Json::obj();
            if (method == "initialize") {
                const Json* v = params.find("protocolVersion");
                result.set("protocolVersion", v ? *v : Json::str("2025-03-26"));
                Json caps = Json::obj(); caps.set("tools", Json::obj()); result.set("capabilities", std::move(caps));
                Json info = Json::obj(); info.set("name", Json::str("vrs-lib")); info.set("version", Json::str("1.1.0"));
                result.set("serverInfo", std::move(info));
                client.start_early();
            } else if (method == "ping") {
            } else if (method == "tools/list") result = tools;
            else if (method == "tools/call") result = invoke(client, params);
            else throw BridgeError("METHOD_NOT_FOUND");
            response.set("jsonrpc", Json::str("2.0")); response.set("id", request.at("id")); response.set("result", std::move(result));
        } catch (const std::exception& e) {
            const bool bridge = dynamic_cast<const BridgeError*>(&e) != nullptr;
            Json err = Json::obj(); err.set("code", Json{}); err.values.back().kind = Json::number;
            err.values.back().scalar = bridge ? "-32601" : "-32700"; err.set("message", Json::str(cut(e.what())));
            response = Json::obj(); response.set("jsonrpc", Json::str("2.0"));
            const Json* id = have && request.kind == Json::object ? request.find("id") : nullptr;
            response.set("id", id ? *id : Json{}); response.set("error", std::move(err));
        }
        const std::string s = response.dump() + "\n";
        std::fwrite(s.data(), 1, s.size(), stdout); std::fflush(stdout);
    };
    // ReadFile returns what the pipe holds now; fread would wait to fill the whole buffer (initialize would never answer).
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    std::string pending; char buf[65536]; DWORD n = 0;
    while (ReadFile(in, buf, sizeof buf, &n, nullptr) && n) {
        pending.append(buf, n);
        for (size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
            handle(pending.substr(0, nl)); pending.erase(0, nl + 1);
        }
    }
    if (!pending.empty()) handle(pending);
    return 0;
#else
    throw std::runtime_error("MCP_BRIDGE_WINDOWS_ONLY: use src/bridge/vrs_lib_bridge.py");
#endif
}
}

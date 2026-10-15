#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct vrs_h;

namespace cc {
// VRS_MCP_codexapp_v1.md 7.3: the daemon of the ChatGPT desktop app (Codex) mode. lib7 reads $CODEX_HOME/sessions itself.
int cx_daemon_main(const std::vector<std::string>& args);
// VRS_MCP_codexapp_v1.md 7.4: the stdio MCP server one app-server shares between its threads; a call names its thread by _meta.threadId.
int cx_mcp_main();
// The tool list and the not-ready text of the Claude Code bridge (cc_mcp.cpp); the Codex bridge answers with the same text (X6).
extern const char* const mcp_tools_json;
extern const char* const mcp_not_ready_text;

// VRS_onePC_v1.md B6: the app share of the one daemon. It answers the requests that name a thread, over a library handle the daemon
// opened (lib() is null until then: NOT_READY); its c* records are in data/cstar.
class CxCore;
std::shared_ptr<CxCore> cx_core(const std::string& codex_home, const std::string& data, const std::vector<std::string>& read_roots,
                                uint64_t timeout_ms, std::function<vrs_h*()> lib);
std::string cx_core_respond(CxCore& core, std::string_view request);
const std::string& cx_core_sessions(const CxCore& core);  // the canonical $CODEX_HOME/sessions

// VRS_onePC_v1.md B7: the one daemon of this PC as either bridge starts it. legacy_* name the environment variables of the separate
// modes (tests); without them the home is VRS_HOME, else %LOCALAPPDATA%/vrs-lib, and the pipe VRS_ENDPOINT, else vrs-lib-<user>.
struct PcDaemon { std::string data, endpoint; std::vector<std::string> args; };  // args: everything after the executable
PcDaemon pc_daemon(const char* legacy_data_env, const char* legacy_endpoint_env);
}

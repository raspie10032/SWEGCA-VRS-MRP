#pragma once
#include <string>

namespace cc {
// D-P1 (doc 14.21): the stdio MCP bridge, the same function as src/bridge/vrs_lib_bridge.py, inside vrs_ccd.exe.
int mcp_main();
// D-P2 (doc 14.21): plugin hooks with default paths. "writer" = --writer-hook <data>/writer-markers --host claude;
// "compact" = the compaction guidance of hook_compact.py.
int hook_main(const std::string& which);
// The writer-marker hook body shared by --writer-hook and --hook writer.
int writer_hook(const std::string& dir, const std::string& host);
}

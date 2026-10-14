#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace vw {
struct Process { uint64_t pid = 0, birth = 0, parent = 0; bool alive = false; };
struct Witness {
    std::string source, boot, name;
    uint64_t pid = 0, birth = 0;
};
Process process(uint64_t pid); // Missing/exited is alive=false; inaccessible throws.
std::string boot();
bool alive(const Witness& w);
std::string path(const std::string& p); // Canonical existing ancestor, unborn leaf allowed.
std::string encode(const Witness& w);
Witness read(const std::string& name);
void publish(const std::string& root, const Witness& w);
uint64_t scan(const std::string& root, bool writers, const std::function<void(const Witness&)>& visit);
uint64_t sources(const Witness& w, const std::function<void(const std::string&)>& visit);
Witness owner(const std::string& source, uint64_t pid);
Witness hook_owner(const std::string& source, const std::string& session,
                   const std::string& host, const std::string& claude_home);
}

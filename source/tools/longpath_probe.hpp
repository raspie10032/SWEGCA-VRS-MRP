#pragma once
// Invoked only by the Windows longpath relay test. All files are under its root.
#ifdef _WIN32
#include "../src/cc_io.hpp"
#include <fstream>

inline int longpath_probe(const std::string& root) {
    namespace fs = std::filesystem;
    cc::Json result = cc::Json::obj(); bool all = true;
    auto check = [&](const char* name, auto fn, bool required = true) {
        cc::Json row = cc::Json::obj();
        try { fn(); row.set("pass", cc::Json::flag(true)); }
        catch (const std::exception& e) { if (required) all = false; row.set("pass", cc::Json::flag(false)).set("error", cc::Json::str(e.what())); }
        row.set("production", cc::Json::flag(required));
        result.set(name, std::move(row));
    };
    auto require = [](bool b) { if (!b) throw std::runtime_error("PROBE_MISMATCH"); };
    check("normalization", [&] {
        require(vp_io_path("C:/a/./b/../c").native() == L"\\\\?\\C:\\a\\c");
        require(vp_io_path("\\\\server\\share\\a\\..\\b").native() == L"\\\\?\\UNC\\server\\share\\b");
        require(vp_io_path("\\\\?\\C:\\a").native() == L"\\\\?\\C:\\a");
        require(vp_io_path("\\\\.\\pipe\\vrs-kl3").native() == L"\\\\.\\pipe\\vrs-kl3");
        require(vp_u8(vp_io_path("\\\\server\\share\\b")) == "//server/share/b");
        require(vp_u8(vp_io_path("\\\\?\\uNc\\server\\share\\b")) == "//server/share/b");
        require(vp_io_path("//?/C:/a").native() == L"\\\\?\\C:\\a");
        require(vp_u8(vp_io_path("C:/a/./b/../c")) == "C:/a/c");
        require(vp_io_path("").empty());
        require(vp_io_path("./a/../b") == vp_io_path("b"));
    });
    const auto directory = vp_io_path(root + "/created/한글");
    check("create_directories", [&] { require(fs::create_directories(directory)); require(fs::is_directory(directory)); });
    // If create_directories fails, bootstrap with Win32 so later observations
    // still measure their own function rather than a missing fixture.
    const auto native = directory.native();
    for (size_t at = native.find(L'\\', 7); ; at = native.find(L'\\', at + 1)) {
        const auto partial = native.substr(0, at);
        if (!CreateDirectoryW(partial.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            throw std::runtime_error("PROBE_BOOTSTRAP_FAILED");
        if (at == std::wstring::npos) break;
    }
    const auto file = directory / L"probe.txt";
    { cc::File f(vp_u8(file), true); f.write_once("KL3", 0); }
    check("directory_iterator", [&] { size_t n = 0; for (const auto& e : fs::directory_iterator(directory)) { require(e.is_regular_file()); ++n; } require(n == 1); }, false);
    check("recursive_directory_iterator", [&] { size_t n = 0; for (const auto& e : fs::recursive_directory_iterator(vp_io_path(root + "/created"))) if (e.is_regular_file()) ++n; require(n == 1); }, false);
    check("canonical", [&] { require(vp_u8(fs::canonical(file)) == cc::canonical(vp_u8(file))); }, false);
    check("weakly_canonical", [&] { require(vp_u8(fs::weakly_canonical(file / L".." / L"missing")) == vp_u8(directory / L"missing")); }, false);
    check("native_directory_iterator", [&] { size_t n = 0; for (const auto& e : vp_directory_iterator(directory)) { require(e.is_regular_file()); require(vp_u8(e.path()) == vp_u8(file)); ++n; } require(n == 1); });
    check("native_recursive_directory_iterator", [&] { size_t n = 0; for (const auto& e : vp_recursive_directory_iterator(vp_io_path(root + "/created"))) if (e.is_regular_file()) { require(vp_u8(e.path()) == vp_u8(file)); ++n; } require(n == 1); });
    check("native_canonical", [&] { require(cc::canonical(vp_u8(file)) == vp_u8(file)); });
    check("native_weakly_canonical", [&] { require(vp_weakly_canonical(vp_u8(file / L".." / L"missing")) == vp_u8(directory / L"missing")); });
    check("file_size", [&] { require(fs::file_size(file) == 3); });
    check("streams", [&] { std::ifstream in(file, std::ios::binary); std::string bytes((std::istreambuf_iterator<char>(in)), {}); require(bytes == "KL3"); });
    check("identity", [&] { cc::File f(vp_u8(file)); f.check_source(vp_u8(file)); });
    result.set("path", cc::Json::str(vp_u8(file))).set("pass", cc::Json::flag(all));
    std::printf("%s\n", result.dump().c_str()); return all ? 0 : 1;
}
#endif

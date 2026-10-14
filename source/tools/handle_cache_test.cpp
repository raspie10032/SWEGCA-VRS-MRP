// N-I: eviction while borrowed, identity persistence, and thread-independent I/O.
#include "cc_io.hpp"
#include <fstream>
#include <iostream>
#include <thread>

static void require(bool value, const char* label) {
  if (!value) throw std::runtime_error(label);
}
static void put(const std::string& path, const std::string& bytes) {
  std::ofstream out(vp_path(path), std::ios::binary); out << bytes;
  require(bool(out), "fixture write");
}
int main(int argc, char** argv) {
  try {
    require(argc == 2, "one fresh work directory");
    const std::string root = argv[1];
    require(!std::filesystem::exists(vp_path(root)), "fresh work directory");
    std::filesystem::create_directories(vp_path(root));
    const auto first = std::make_shared<VrsHandleKey>();
    std::shared_ptr<VrsHandleKey> collision;
    std::vector<std::shared_ptr<VrsHandleKey>> keys;
    for (size_t i = 0; i < 4096 && !collision; ++i) {
      auto key = std::make_shared<VrsHandleKey>();
      if (VrsHandleCache<VrsFd>::slot(key.get()) == VrsHandleCache<VrsFd>::slot(first.get())) collision = key;
      keys.push_back(std::move(key));
    }
    require(bool(collision), "collision fixture");
    put(root + "/a", "alpha"); put(root + "/b", "bravo");
    VrsHandleCache<VrsFd> cache;
    unsigned opened = 0;
    auto get = [&](const auto& key, const char* name) {
      return cache.get(key, [&] { ++opened; return std::make_shared<VrsFd>(vp_open_ro((root + name).c_str())); });
    };
    auto a = get(first, "/a");
    require(get(first, "/a") == a && opened == 1, "hit reuses handle");
    auto b = get(collision, "/b");
    char bytes[5];
    require(vp_pread(a->fd, bytes, 5, 0) == 5 && std::string(bytes, 5) == "alpha", "eviction closed borrowed file");
    require(vp_pread(b->fd, bytes, 5, 0) == 5 && std::string(bytes, 5) == "bravo", "wrong collision file");
    require(get(first, "/a") != a && opened == 3, "miss did not reopen");

    cc::FileSlot persistent;
    auto original = persistent.get(root + "/a", false);
    std::vector<std::unique_ptr<cc::FileSlot>> slots;
    for (unsigned i = 0; i < 512; ++i) {
      slots.push_back(std::make_unique<cc::FileSlot>());
      slots.back()->get(root + "/b", false);
    }
    require(original.use_count() == 1, "source cache eviction fixture");
    std::filesystem::rename(vp_path(root + "/a"), vp_path(root + "/old-a"));
    put(root + "/a", "changed");
    bool rejected = false;
    try { auto x = persistent.get(root + "/a", false); x->check_source(root + "/a"); }
    catch (const std::runtime_error& e) { rejected = std::string(e.what()) == "SOURCE_REPLACED"; }
    require(rejected, "identity forgotten on eviction");
    require(original->range(0, 5) == "alpha", "original lease changed object");

    cc::FileSlot same_file;
    std::array<std::string, 8> errors;
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < 8; ++t) workers.emplace_back([&, t] {
      try {
        for (unsigned n = 0; n < 1000; ++n) {
          auto file = same_file.get(root + "/parallel", true);
          file->write_once(std::string(16, char('A' + t)), 16 * t);
          cc::FileSlot other;
          other.get(root + "/b", false);
          require(file->range(16 * t, 16) == std::string(16, char('A' + t)), "parallel positioned I/O");
        }
      } catch (const std::exception& e) { errors[t] = e.what(); }
    });
    for (auto& worker : workers) worker.join();
    for (const auto& error : errors) require(error.empty(), error.c_str());
    std::cout << "PASS reuse, forced collision with live lease, replacement after eviction, 8000 concurrent positioned writes\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}

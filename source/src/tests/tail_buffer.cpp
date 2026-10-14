// Storage equivalence for N-D: chunk cuts, embedded NUL, retained suffix and leases.
#include "vrs_tail_buffer.h"
#include <cassert>
#include <string>
#include <thread>
#include <vector>

static void check() {
  VrsTailLease outer;
  auto& b = outer.get();
  std::string input = "one\n";
  input.append(37 * 1024 * 1024, 'x');  // a line larger than the 16 MiB read chunk
  input += "\nend";
  input.insert(12345, 3, '\0');
  std::string reference;
  size_t off = 0, step = 7;
  while (off < input.size()) {
    const size_t count = std::min(step, input.size() - off);
    b.reserve_more(count);
    std::memcpy(&b[b.size()], input.data() + off, count);
    b.appended(count);
    reference.append(input, off, count);
    assert(b.size() == reference.size());
    assert(std::memcmp(&b[0], reference.data(), b.size()) == 0);
    assert(b.find('\n', 0) == reference.find('\n'));
    assert(b.find('\0', 0) == reference.find('\0'));
    assert(b.find('x', b.size()) == VrsTailBuffer::npos);
    const size_t nl = b.find('\n', 0);
    if (nl != VrsTailBuffer::npos) { b.remove_prefix(nl + 1); reference.erase(0, nl + 1); }
    { VrsTailLease nested; auto& n = nested.get();
      assert(&n != &b); n.reserve_more(1); n[0] = '!'; n.appended(1); }
    assert(b.size() == reference.size());
    if (!reference.empty()) assert(std::memcmp(&b[0], reference.data(), b.size()) == 0);
    off += count;
    step = std::min<size_t>(step * 2 + 1, 16 * 1024 * 1024);
  }
  assert(reference == "end");
}

int main() {
  check();
  { VrsTailLease again; assert(again.get().size() == 0); }
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) threads.emplace_back(check);
  for (auto& t : threads) t.join();
}

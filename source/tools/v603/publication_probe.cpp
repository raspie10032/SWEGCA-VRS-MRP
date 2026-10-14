// Deterministic publication schedules. No sleeps, no paused worker, no changed W6 oracle.
// Compile this translation unit with the architecture sources (it includes the library).
#include "../../native/libvrs/vrs_lib.cpp"
#define main original_v603_main
#include "v603_portable.cpp"
#undef main

static bool contains(const std::string& got, const std::vector<std::string>& expected) {
  size_t pos = 0;
  for (const auto& t : expected) {
    const auto p = got.find("\"text\":\"" + t + "\"", pos);
    if (p == std::string::npos) return false;
    pos = p;
  }
  return true;
}
int main(int argc, char** argv) {
  if (argc != 4) return 2; // ops, isolated work directory, expected precompletion rc (0 before fix, 1 after)
  std::vector<Op> ops;
  std::ifstream input(argv[1]); std::string line;
  while (std::getline(input, line)) {
    Op op{line[0], std::atoi(line.c_str() + 2), {}};
    if (op.k == 'W') op.line = line.substr(line.find(' ', 2) + 1);
    ops.push_back(op);
  }
  std::vector<std::pair<int, int64_t>> cuts(3, {-1, 0});
  for (const auto& op : ops) if (op.k == 'W') {
    const auto p = op.line.find("\"forked_from_ordinal_exclusive\":");
    if (p != std::string::npos) cuts[op.f] = {op.f - 1, std::strtoll(op.line.c_str() + p + 32, nullptr, 10)};
  }
  fs::create_directories(argv[2]);
  int bad = 0;
  for (int target : {11, 25}) for (int stage = 0; stage < 4; ++stage) {
    // A Store without resident threads is an explicit schedule of the same operations.
    // Pending work is selected from the production set only when this driver chooses it.
    auto sp = std::make_unique<Store>(); auto& S = *sp;
    S.sdir = fs::absolute(argv[2]).string(); S.segwin = true; S.budget = 258400;
    S.B.st = &S; S.B.yields = &S.insert_waits;
    S.ready.store(1); S.loaded.store(1); S.restore_left.store(0); S.fin_left.store(0);
    vrs_h handle{&S}; auto* h = &handle;
    std::vector<std::string> paths;
    std::ifstream names(fs::path(argv[1]).parent_path() / "names.txt");
    while (std::getline(names, line)) paths.push_back(S.sdir + "/" + line);
    std::vector<uint64_t> offsets(paths.size());
    auto drain = [&] { uint64_t v; uint32_t hint = 0; while (next_work(S, v, hint)) run_task(S, v); };
    for (int q = 0; q <= target; ++q) {
      const auto& op = ops[q];
      if (op.k != 'W') continue;
      const auto off = offsets[op.f];
      if (q == target) {
        auto* ss = get_sess(S, paths[op.f], true);
        drain();
        std::vector<uint32_t> run;
        if (stage == 0) S.lmap.get(lk(ss->sg, off))->val.store(CLAIMING);
        else {
          insert_line(S, *ss, off, op.line.data(), op.line.size(), false);
          run.push_back(S.lmap.find(lk(ss->sg, off))->val.load());
          if (stage >= 2) ss->front.store(off + op.line.size() + 1);
          if (stage == 3) {
            std::string wb; process_line(S, *ss, run[0], wb); wflush(S, *ss, wb);
            S.lr[run[0]].done.store(1); S.pend_total.fetch_sub(1);
          }
        }
        const int inserted = vrs_insert(h, paths[op.f].c_str(), off, op.line.data(), op.line.size());
        char* json = nullptr; size_t n = 0;
        const int wait_rc = vrs_wait(h, paths[op.f].c_str(), 0);
        const int off_rc = vrs_wait_off(h, paths[op.f].c_str(), off + op.line.size() + 1, 0);
        const int rc = vrs_assemble(h, paths[op.f].c_str(), &json, &n);
        const bool held = json && contains(std::string(json, n), expect(ops, q + 1, op.f, cuts));
        if (json) vrs_free(json);
        const auto before = ss->pub.load();
        if (stage == 0) { S.lmap.get(lk(ss->sg, off))->val.store(0); insert_line(S, *ss, off, op.line.data(), op.line.size(), true); }
        if (stage == 1) claim_and_run(S, *ss);
        if (stage == 2) process_run(S, *ss, run);
        if (stage == 3) advance_pub(S, *ss);
        drain();
        const int completed_wait = vrs_wait(h, paths[op.f].c_str(), 0);
        json = nullptr;
        const int completed_rc = vrs_assemble(h, paths[op.f].c_str(), &json, &n);
        const bool complete = json && contains(std::string(json, n), expect(ops, q + 1, op.f, cuts));
        if (json) vrs_free(json);
        const int expected_rc = std::atoi(argv[3]);
        const bool ok = inserted == 0 && rc == expected_rc && !held && off_rc == 1 &&
                        wait_rc == expected_rc && completed_wait == 0 && completed_rc == 0 && complete;
        bad += !ok;
        std::printf("target=%d stage=%d insert=%d pub=%llu end=%llu wait=%d wait_off=%d assemble=%d contains=%d complete=%d/%d pass=%d\n",
            target, stage, inserted, (unsigned long long)before, (unsigned long long)(off + op.line.size() + 1),
            wait_rc, off_rc, rc, held, completed_rc, complete, ok);
      } else { vrs_insert(h, paths[op.f].c_str(), off, op.line.data(), op.line.size()); drain(); }
      offsets[op.f] += op.line.size() + 1;
    }
  }
  std::printf("publication_schedules=8 bad=%d\n", bad);
  return bad ? 1 : 0;
}

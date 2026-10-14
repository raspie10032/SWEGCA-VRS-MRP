// N-H regression: saturated work dispatch must not run a successor inline.
// Harness rendezvous are test-only; the product adds no waits.
#include "cc_conv.hpp"
#include <atomic>
#include <iostream>
#include <stdexcept>

using cc::Resident;
static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

int main(int argc, char** argv) {
    {
        Resident pool(1);
        std::atomic<bool> producer{false}, ready{false};
        std::atomic<unsigned> calls{0}, inline_calls{0}, pieces{0};
        const size_t check = pool.prepare_ready([&] { return ready.load(); }, [&](unsigned) {
            if (producer.load()) ++inline_calls;
            ++calls;
        });
        require(check != Resident::no_ready_slot, "reserve readiness");
        pool.submit([&](unsigned slot) {
            producer.store(true);
            // One resident cannot consume these until this callback returns.
            // The final independent piece takes submit's finite-capacity fallback.
            for (unsigned i = 0; i < 4097; ++i) pool.submit([&](unsigned) { ++pieces; }, slot);
            ready.store(true);
            pool.notify_ready(check);
            require(calls.load() == 0, "successor ran in producer callback");
            producer.store(false);
        }, 63);
        pool.drain();
        require(calls == 1 && inline_calls == 0 && pieces == 4097, "saturated dispatch");
        std::cout << "saturated dispatch: pieces=4097 successors=1 inline=0\n";
    }
    {
        Resident pool(8);
        // A readiness notification racing with an unready dispatch must remain
        // observable. Reuse slots repeatedly to exercise late, harmless marks.
        for (unsigned round = 0; round < 100; ++round) {
            constexpr unsigned n = 256;
            // An unready checker can still hold its predicate after a peer has
            // completed the registered job. Match the product's shared lifetime.
            auto flags = std::make_shared<std::array<std::atomic<bool>, n>>();
            std::array<size_t, n> ids{};
            std::atomic<unsigned> completed{0};
            for (unsigned i = 0; i < n; ++i) {
                ids[i] = pool.prepare_ready([flags, i] { return (*flags)[i].load(); }, [&](unsigned) { ++completed; });
                require(ids[i] != Resident::no_ready_slot, "race reserve");
                pool.notify_ready(ids[i]);
                pool.submit([&, flags, i](unsigned) { (*flags)[i].store(true); pool.notify_ready(ids[i]); }, 63);
            }
            pool.drain();
            require(completed == n, "lost readiness");
        }
        std::cout << "readiness races: 25600/25600\n";
    }
    {
        Resident pool(1);
        std::atomic<bool> ready{false};
        std::atomic<unsigned> calls{0};
        std::array<size_t, 4096> ids{};
        for (auto& id : ids) {
            id = pool.prepare_ready([&] { return ready.load(); }, [&](unsigned) { ++calls; });
            require(id != Resident::no_ready_slot, "capacity reserve");
        }
        require(pool.prepare_ready([] { return true; }, [](unsigned) {}) == Resident::no_ready_slot, "capacity bound");
        // Actual group admission at saturation must preserve the complete output.
        const std::string root = argc > 1 ? argv[1] : ".h-artifacts/readiness-files";
        cc::fs::create_directories(vp_path(root));
        cc::Tail tail;
        tail.transcript = root + "/source/session.jsonl";
        tail.rollout = root + "/reference/session.jsonl";
        tail.cover = root + "/reference.cv";
        for (unsigned i = 0; i < 5000; ++i)
            tail.bytes += "{\"type\":\"user\",\"timestamp\":\"2026-10-13T00:00:00.000Z\",\"uuid\":\"u" +
                std::to_string(i) + "\",\"message\":{\"content\":\"hello\"}}\n";
        tail.observed_end = tail.bytes.size();
        const auto expected = cc::conv_prepared(tail, 63);
        tail.rollout = root + "/fallback/session.jsonl"; tail.cover = root + "/fallback.cv";
        bool finished = false;
        cc::conv_parallel(tail, pool, 63, nullptr, [&](cc::TailResult got, std::string error) {
            require(error.empty() && got.lines == expected.lines && got.source_end == expected.source_end, "group admission result");
            finished = true;
        }, 64);
        require(finished, "group admission did not return its independent result");
        cc::File ref(root + "/reference/session.jsonl", false), fallback(tail.rollout, false);
        require(ref.range(0, ref.size()) == fallback.range(0, fallback.size()), "group admission bytes");
        ready.store(true);
        for (auto id : ids) pool.notify_ready(id);
        pool.drain();
        require(calls == 4096, "capacity completion");
        std::cout << "readiness capacity: 4096 completed, overflow returned before execution\n";
        std::cout << "group admission: 5000 input lines, output bytes equal\n";
    }
}

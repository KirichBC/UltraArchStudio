#include "core/RamCandidate.hpp"
#include "core/SettingsJson.hpp"
#include <atomic>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>
using namespace UltraArch;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)
template<class F> static void rejected(F action) {
    bool caught = false;
    try { action(); } catch (const std::exception &) { caught = true; }
    CHECK(caught);
}
static void settingsAndThresholds() {
    OptimizerSettings s;
    s.enableRamCandidates = true;
    s.ramGroupLimitMB = 40;
    s.ramTemporaryBudgetMB = 256;
    auto restored = settingsFromJson(settingsJson(s));
    CHECK(restored.enableRamCandidates && restored.ramGroupLimitMB == 40 &&
          restored.ramTemporaryBudgetMB == 256);
    for (const auto *name : {"ramGroupLimitMB", "ramTemporaryBudgetMB"}) {
        auto j = settingsJson(s); j[name] = -1;
        rejected([&] { (void)settingsFromJson(j); });
    }
    for (auto limit : {0ULL, 257ULL}) {
        auto j = settingsJson(s); j["ramGroupLimitMB"] = limit;
        rejected([&] { (void)settingsFromJson(j); });
    }
    for (auto limit : {31ULL, 4097ULL}) {
        auto j = settingsJson(s); j["ramTemporaryBudgetMB"] = limit;
        rejected([&] { (void)settingsFromJson(j); });
    }
    CHECK(RamPolicy::eligible(s, 40 * RamPolicy::MiB, 1));
    CHECK(!RamPolicy::eligible(s, 40 * RamPolicy::MiB + 1, 1));
    CHECK(!RamPolicy::eligible(s, 1, 65537));
    s.enableRamCandidates = false;
    CHECK(!RamPolicy::eligible(s, 1, 1));
    s.enableRamCandidates = true; s.ramTemporaryBudgetMB = 32;
    CHECK(!RamPolicy::eligible(s, 40 * RamPolicy::MiB, 1));
}
static void budgetsAndEstimates() {
    RamBudget budget(4096);
    CHECK(budget.acquire(4096)); CHECK(!budget.acquire(1));
    budget.release(4096);
    CHECK(!budget.acquire(std::numeric_limits<uint64_t>::max()));
    std::atomic<bool> failed{false};
    std::vector<std::thread> threads;
    for (unsigned n = 0; n < 8; ++n) threads.emplace_back([&] {
        for (unsigned i = 0; i < 10000; ++i) {
            if (budget.acquire(128)) {
                if (budget.used() > 4096) failed = true;
                budget.release(128);
            }
        }
    });
    for (auto &thread : threads) thread.join();
    CHECK(!failed && budget.used() == 0);
    OptimizerSettings s; s.enableRamCandidates = true; s.workerThreads = 4;
    s.cpuThreads = 8; s.ramLimitMB = 8192; s.ramTemporaryBudgetMB = 256;
    auto e = RamPolicy::estimate(s, 40 * RamPolicy::MiB);
    CHECK(e.perTaskMB >= e.temporaryMB + e.codecMB);
    CHECK(e.slots > 0 && e.slots <= s.workerThreads);
    CHECK(e.concurrentMB == e.perTaskMB * e.slots && e.concurrentMB <= s.ramLimitMB);
    CHECK(e.temporaryMB * e.slots <= s.ramTemporaryBudgetMB);
    s.ramLimitMB = 64;
    CHECK(!RamPolicy::estimate(s, 40 * RamPolicy::MiB).fits);
    for (uint64_t bytes : {0ULL, 1ULL, 40ULL * RamPolicy::MiB, 256ULL * RamPolicy::MiB}) {
        CHECK(RamPolicy::outputCapacity(bytes, 65536) >= bytes);
        CHECK(RamPolicy::temporaryMB(bytes, 65536) * RamPolicy::MiB >=
              bytes + RamPolicy::outputCapacity(bytes, 65536));
    }
    // Either reject an impossible request or conservatively saturate; never wrap small.
    try { CHECK(RamPolicy::outputCapacity(UINT64_MAX, 1) == UINT64_MAX); }
    catch (const std::overflow_error &) {}
    try { CHECK(RamPolicy::temporaryMB(UINT64_MAX, 1) >= UINT64_MAX / RamPolicy::MiB); }
    catch (const std::overflow_error &) {}
    try { CHECK(RamPolicy::temporaryMB(UINT64_MAX - 16 * RamPolicy::MiB - 1024, 1) >=
                UINT64_MAX / RamPolicy::MiB); }
    catch (const std::overflow_error &) {}
}
int main() {
    try { settingsAndThresholds(); budgetsAndEstimates();
        std::cout << "RAM policy contracts passed\n"; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}

#pragma once
#include "SettingsJson.hpp"
#include <chrono>
#include <map>
#include <mutex>

namespace UltraArch {
// Bounded aggregates: overlapping operations are effort, never total run wall time.
class StageMetrics {
    struct Entry { double seconds = 0; uint64_t calls = 0; };
    mutable std::mutex mutex;
    std::map<std::string, Entry> entries;
  public:
    void clear() { std::lock_guard lock(mutex); entries.clear(); }
    void add(const std::string &stage, double seconds) {
        std::lock_guard lock(mutex);
        auto &entry = entries[stage];
        entry.seconds += std::max(0.0, seconds);
        ++entry.calls;
    }
    Json report() const {
        std::lock_guard lock(mutex);
        Json stages = Json::object();
        std::string largest;
        double maximum = -1;
        for (const auto &[name, entry] : entries) {
            stages[name] = {{"seconds", entry.seconds}, {"calls", entry.calls}};
            if (entry.seconds > maximum) { maximum = entry.seconds; largest = name; }
        }
        return {{"stages", stages}, {"largestAccumulatedStage", largest},
            {"scope", "Local engine operation elapsed time, including failed attempts. Concurrent/nested operations overlap; do not sum as run wall time. Child engines are reported separately in groups. Phase wall intervals remain in phaseSeconds."}};
    }
    class Scope {
        StageMetrics *owner;
        std::string stage;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
      public:
        Scope(StageMetrics *metrics, std::string name) : owner(metrics), stage(std::move(name)) {}
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;
        ~Scope() { finish(); }
        void finish() noexcept {
            if (!owner) return;
            try { owner->add(stage, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()); }
            catch (...) {} // Diagnostics cannot change publication or recovery outcomes.
            owner = nullptr;
        }
    };
};
}

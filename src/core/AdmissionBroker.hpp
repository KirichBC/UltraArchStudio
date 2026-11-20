#pragma once
#include "ResourceScheduler.hpp"
#include <deque>
#include <atomic>
#include <memory>
#include <mutex>
#include <algorithm>
#include <functional>
#include <set>
#include <vector>

namespace UltraArch {
// One broker per run, shared by child engines. Orchestration never takes a lease.
class AdmissionBroker : public std::enable_shared_from_this<AdmissionBroker> {
  public:
    enum class RestoreState { Unchanged, WaitingReservations, WaitingCommit, Restored, UpdateFailed };
    struct Snapshot {
        uint64_t ramLimit, ramReserved;
        uint32_t cpuLimit, cpuReserved, active, diskLimit;
        bool admissionsBlocked = false, draining = false;
        RestoreState restoreState = RestoreState::Unchanged;
        std::string blockReason;
        uint32_t diskActive = 0, executorLimit = 0;
        size_t waiting = 0;
        uint64_t peakTotalRam = 0;
        uint32_t peakActive = 0, peakDisk = 0;
        uint64_t persistent = 0;
    };
    struct Lease {
        std::shared_ptr<AdmissionBroker> owner;
        uint64_t ram = 0;
        uint32_t cpu = 0;
        bool disk = true, executes = true;
        uint32_t workerIndex = 0;
        std::string key;
        ~Lease() { if (owner) owner->release(key, ram, cpu, disk, executes, workerIndex); }
        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;
        Lease(std::shared_ptr<AdmissionBroker> value, uint64_t r, uint32_t c, std::string k,
              bool needsDisk = true, bool runs = true)
            : owner(std::move(value)), ram(r), cpu(c), disk(needsDisk), key(std::move(k)), executes(runs) {}
    };
    AdmissionBroker(uint64_t ram, uint32_t cpu, uint32_t disk, uint64_t persistent = 0, uint32_t executors = 0)
        : resources{ram, 0, cpu, 0}, diskLimit(std::max(1u, disk)), executorLimit(std::max(1u, executors ? executors : disk)), baseRam(ram), persistentRam(persistent), executionSlots(executorLimit, false) {}
    uint64_t baselineRam() const { return baseRam.load(); }
    uint64_t persistentMB() const { return persistentRam.load(); }
    bool setPersistent(uint64_t value) {
        std::lock_guard lock(mutex);
        if (active || resources.ramReserved || baseRam > UINT64_MAX - persistentRam ||
            value >= baseRam + persistentRam) return false;
        const auto total = baseRam + persistentRam;
        persistentRam = value;
        baseRam = resources.ramLimit = total - value;
        return true;
    }
    void setAdmissionCheck(std::function<std::string(uint64_t, uint32_t)> check) {
        std::lock_guard lock(mutex);
        admissionCheck = std::move(check);
    }
    std::shared_ptr<Lease> tryAcquire(const std::string &key, uint64_t ram, uint32_t cpu,
                                    bool disk = true, bool executes = true) {
        std::lock_guard lock(mutex);
        if (admissionsBlocked || draining || activeKeys.contains(key) || (disk && !executes)) return {};
        auto found = std::find_if(waiters.begin(), waiters.end(), [&](const auto &w) { return w.key == key; });
        if (found == waiters.end()) { waiters.push_back({key, ram, cpu}); found = std::prev(waiters.end()); }
        else { found->ram = ram; found->cpu = cpu; }
        if (!resources.fits(ram, cpu) || (executes && active >= executorLimit) || (disk && (diskActive >= diskLimit || !diskUpgrades.empty()))) return {};
        if (admissionCheck) {
            pressureReason = admissionCheck(resources.ramReserved + ram, cpu);
            if (!pressureReason.empty()) return {};
        }
        // An individually impossible request is retained for a later override,
        // but must not drain the entire run behind a limit it cannot currently fit.
        const auto head = std::find_if(waiters.begin(), waiters.end(), [&](const auto &w) {
            return w.ram <= resources.ramLimit && w.cpu <= resources.cpuLimit;
        });
        const auto headKey = head == waiters.end() ? std::string{} : head->key;
        if (headKey != fairnessHead) { fairnessHead = headKey; bypasses = 0; }
        if (found != head && bypasses >= 32) return {};
        if (found == head) bypasses = 0; else ++bypasses;
        auto owner = shared_from_this();
        auto lease = std::make_shared<Lease>(nullptr, ram, cpu, key, disk, executes);
        activeKeys.insert(key);
        resources.reserve(ram, cpu);
        if (executes) {
            lease->workerIndex = static_cast<uint32_t>(std::find(executionSlots.begin(), executionSlots.end(), false) - executionSlots.begin());
            executionSlots[lease->workerIndex] = true;
            ++active;
        }
        if (disk) ++diskActive;
        recordPeaks();
        waiters.erase(found);
        lease->owner = std::move(owner);
        return lease;
    }
    // A RAM attempt must obtain disk capacity before materialization or CLI fallback.
    // Upgrades keep their existing memory/CPU reservation and precede new disk admissions.
    bool tryEnableDisk(const std::shared_ptr<Lease> &lease) {
        std::lock_guard lock(mutex);
        if (!lease || lease->owner.get() != this || !lease->executes || !activeKeys.contains(lease->key))
            return false;
        if (lease->disk) return true;
        diskUpgrades.insert(lease->key);
        if (diskActive >= diskLimit) return false;
        ++diskActive;
        lease->disk = true;
        recordPeaks();
        diskUpgrades.erase(lease->key);
        return true;
    }
    void withdraw(const std::string &key) {
        std::lock_guard lock(mutex);
        if (fairnessHead == key) { fairnessHead.clear(); bypasses = 0; }
        std::erase_if(waiters, [&](const auto &w) { return w.key == key; });
    }
    Snapshot snapshot() const {
        std::lock_guard lock(mutex);
        return {resources.ramLimit, resources.ramReserved, resources.cpuLimit, resources.cpuReserved,
                active, diskLimit, admissionsBlocked, draining, restoreState,
                admissionsBlocked ? blockReason : draining ? "Final admission drain" : pressureReason, diskActive, executorLimit, waiters.size(), peakTotalRam, peakActive, peakDisk, persistentRam.load()};
    }
    bool raiseLimit(uint64_t target, const std::function<bool()> &update) {
        std::lock_guard lock(mutex);
        if (admissionsBlocked || target <= resources.ramLimit || target <= baseRam ||
            target - baseRam > 1024 || !update()) return false;
        resources.ramLimit = target;
        return true;
    }
    // Job update and candidate reservation form one transaction under the broker
    // mutex. A queued override already owns its resources before launch is signalled.
    std::shared_ptr<Lease> raiseAndAcquire(const std::string &key, uint64_t ram, uint32_t cpu,
                                           uint64_t target, const std::function<bool()> &update) {
        std::lock_guard lock(mutex);
        if (admissionsBlocked || draining || activeKeys.contains(key) || (active >= executorLimit || diskActive >= diskLimit || !diskUpgrades.empty()) ||
            (target > baseRam && target - baseRam > 1024) || target > UINT64_MAX - persistentRam ||
            ram > UINT64_MAX - resources.ramReserved || resources.cpuReserved > resources.cpuLimit ||
            cpu > resources.cpuLimit - resources.cpuReserved)
            return {};
        const auto effective = std::max(target, resources.ramLimit);
        if (resources.ramReserved + ram > effective)
            return {};
        auto owner = shared_from_this();
        auto lease = std::make_shared<Lease>(nullptr, ram, cpu, key);
        // All potentially allocating bookkeeping precedes the external Job change.
        activeKeys.insert(key);
        if (target > resources.ramLimit) {
            bool updated = false;
            try { updated = update(); } catch (...) { updated = false; }
            if (!updated) { activeKeys.erase(key); return {}; }
            resources.ramLimit = target;
        }
        resources.reserve(ram, cpu);
        lease->workerIndex = static_cast<uint32_t>(std::find(executionSlots.begin(), executionSlots.end(), false) - executionSlots.begin());
        executionSlots[lease->workerIndex] = true;
        ++active;
        ++diskActive;
        recordPeaks();
        std::erase_if(waiters, [&](const auto &w) { return w.key == key; });
        if (fairnessHead == key) { fairnessHead.clear(); bypasses = 0; }
        lease->owner = std::move(owner);
        return lease;
    }
    // currentCommittedMB is the total Job commit, including persistent RAM input.
    bool restoreLimit(uint64_t target, const std::function<bool()> &update,
                      uint64_t currentCommittedMB = 0) {
        std::lock_guard lock(mutex);
        if (target >= resources.ramLimit) return false;
        if (resources.ramReserved > target) {
            restoreState = RestoreState::WaitingReservations;
            return false;
        }
        if (target > UINT64_MAX - persistentRam || currentCommittedMB > target + persistentRam) {
            restoreState = RestoreState::WaitingCommit;
            return false;
        }
        bool updated = false;
        try { updated = update(); } catch (...) { updated = false; }
        if (!updated) {
            restoreState = RestoreState::UpdateFailed;
            admissionsBlocked = true;
            blockReason = "Windows Job memory limit restoration failed; new admissions blocked";
            return false;
        }
        resources.ramLimit = target;
        admissionsBlocked = false;
        blockReason.clear();
        restoreState = RestoreState::Restored;
        return true;
    }
    void beginDrain() {
        std::lock_guard lock(mutex);
        draining = true;
        waiters.clear();
        fairnessHead.clear();
        bypasses = 0;
    }
    void endDrain() {
        std::lock_guard lock(mutex);
        draining = false;
    }
  private:
    void recordPeaks() {
        const auto total = resources.ramReserved > UINT64_MAX - persistentRam ? UINT64_MAX : resources.ramReserved + persistentRam;
        peakTotalRam = std::max(peakTotalRam, total);
        peakActive = std::max(peakActive, active);
        peakDisk = std::max(peakDisk, diskActive);
    }
    uint64_t peakTotalRam = 0;
    uint32_t peakActive = 0, peakDisk = 0;
    struct Waiter { std::string key; uint64_t ram; uint32_t cpu; };
    void release(const std::string &key, uint64_t ram, uint32_t cpu, bool disk, bool executes, uint32_t workerIndex) {
        std::lock_guard lock(mutex);
        resources.release(ram, cpu);
        if (executes) { executionSlots[workerIndex] = false; --active; }
        if (disk) --diskActive;
        diskUpgrades.erase(key);
        activeKeys.erase(key);
    }
    mutable std::mutex mutex;
    ResourceScheduler resources;
    uint32_t diskLimit, executorLimit, active = 0, diskActive = 0, bypasses = 0;
    bool admissionsBlocked = false, draining = false;
    RestoreState restoreState = RestoreState::Unchanged;
    std::string blockReason, pressureReason;
    std::function<std::string(uint64_t, uint32_t)> admissionCheck;
    std::string fairnessHead;
    std::atomic<uint64_t> baseRam, persistentRam;
    std::deque<Waiter> waiters;
    std::set<std::string> activeKeys, diskUpgrades;
    std::vector<bool> executionSlots;
};
} // namespace UltraArch

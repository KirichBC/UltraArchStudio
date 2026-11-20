#include "core/InputManifest.hpp"
#include "core/PublicationTransaction.hpp"
#include "core/ResourceScheduler.hpp"
#include "core/AdmissionBroker.hpp"
#include "core/SettingsJson.hpp"
#include "core/TaskMatrixGenerator.hpp"
#include "core/DictionaryPolicy.hpp"
#include "core/ToolCapabilities.hpp"
#include "core/ProcessManager.hpp"
#include "core/WorkspaceManager.hpp"
#include "core/SearchPlanner.hpp"
#include "core/GroupPlanner.hpp"
#include "core/ProbePlanner.hpp"
#include "core/GroupCheckpoint.hpp"
#include <fstream>
#include <iostream>
#include <limits>
using namespace UltraArch;
namespace fs = std::filesystem;
#define REQUIRE(x)                                                                                 \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error(#x);                                                          \
    } while (false)
void write(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
    REQUIRE(out.good());
}
void admissionBrokerContracts() {
    // Units are 0.1 GiB: 20 GiB total, 2 persistent, 10 active, 15 waiting, five 0.5 backfills.
    auto mixed = std::make_shared<AdmissionBroker>(180, 6, 1, 20, 6);
    auto ten = mixed->tryAcquire("ten", 100, 1, false);
    REQUIRE(ten && !mixed->tryAcquire("fifteen", 150, 1, false));
    std::vector<std::shared_ptr<AdmissionBroker::Lease>> small;
    for (int i = 0; i < 5; ++i) {
        small.push_back(mixed->tryAcquire("small-" + std::to_string(i), 5, 1, false));
        REQUIRE(small.back());
    }
    REQUIRE(mixed->snapshot().active == 6 && mixed->snapshot().diskActive == 0);
    REQUIRE(!mixed->tryAcquire("executor-overflow", 0, 0, false));
    mixed->withdraw("executor-overflow");
    ten.reset();
    auto fifteen = mixed->tryAcquire("fifteen", 150, 1, false);
    REQUIRE(fifteen && mixed->snapshot().ramReserved + mixed->persistentMB() == 195);
    REQUIRE(!mixed->tryAcquire("fifteen", 0, 0, false)); // Duplicate launch.
    REQUIRE(mixed->tryEnableDisk(fifteen));
    REQUIRE(!mixed->tryEnableDisk(small.front()));
    fifteen.reset();
    REQUIRE(!mixed->tryAcquire("new-disk-before-upgrade", 0, 0));
    mixed->withdraw("new-disk-before-upgrade");
    REQUIRE(mixed->tryEnableDisk(small.front()));
    small.clear();
    REQUIRE(mixed->snapshot().active == 0 && mixed->snapshot().diskActive == 0);
    REQUIRE(mixed->snapshot().ramReserved == 0 && mixed->snapshot().waiting == 0);
    REQUIRE(mixed->setPersistent(30) && mixed->baselineRam() == 170);
    auto buffer = mixed->tryAcquire("buffer", 10, 0, false, false);
    REQUIRE(buffer && mixed->snapshot().active == 0 && !mixed->setPersistent(20));
    buffer.reset();
    REQUIRE(mixed->setPersistent(20) && mixed->baselineRam() == 180);

    bool pressure = true;
    mixed->setAdmissionCheck([&](uint64_t planned, uint32_t) {
        REQUIRE(planned == 100);
        return pressure ? std::string("Injected system pressure") : std::string{};
    });
    REQUIRE(!mixed->tryAcquire("pressure", 100, 1));
    REQUIRE(mixed->snapshot().ramReserved == 0 && mixed->snapshot().blockReason == "Injected system pressure");
    pressure = false;
    auto afterPressure = mixed->tryAcquire("pressure", 100, 1);
    REQUIRE(afterPressure && mixed->snapshot().blockReason.empty());
    afterPressure.reset();
    auto broker = std::make_shared<AdmissionBroker>(100, 4, 2, 8);
    REQUIRE(broker->baselineRam() == 100 && broker->persistentMB() == 8);
    REQUIRE(broker->snapshot().ramReserved == 0 && broker->snapshot().active == 0);
    // Child groups share these exact reservations; orchestration needs no lease.
    auto first = broker->tryAcquire("group-a/task", 60, 2);
    REQUIRE(first && broker->snapshot().ramReserved == 60 && broker->snapshot().cpuReserved == 2);
    REQUIRE(!broker->tryAcquire("group-b/blocked", 50, 2));
    broker->withdraw("group-b/blocked");
    auto second = broker->tryAcquire("group-b/small", 40, 2);
    REQUIRE(second && broker->snapshot().active == 2);
    REQUIRE(first->workerIndex != second->workerIndex);
    const auto releasedSlot = first->workerIndex;
    first.reset();
    auto replacement = broker->tryAcquire("reused-slot", 60, 2);
    REQUIRE(replacement && replacement->workerIndex == releasedSlot);
    first = std::move(replacement);
    REQUIRE(!broker->tryAcquire("cpu-hard-limit", 0, 1));
    broker->withdraw("cpu-hard-limit");
    REQUIRE(!broker->tryAcquire("disk-hard-limit", 0, 0));
    broker->withdraw("disk-hard-limit");
    first.reset(); second.reset();
    REQUIRE(broker->snapshot().ramReserved == 0 && broker->snapshot().cpuReserved == 0);
    bool workerStarted = false;
    try {
        auto failedWorker = broker->tryAcquire("failed-worker", 100, 4);
        REQUIRE(failedWorker);
        workerStarted = true;
        throw std::runtime_error("worker failure");
    } catch (const std::runtime_error &) {}
    REQUIRE(workerStarted && broker->snapshot().active == 0 && broker->snapshot().ramReserved == 0);
    auto cancelled = broker->tryAcquire("cancelled-worker", 100, 4);
    REQUIRE(cancelled);
    cancelled.reset();
    REQUIRE(broker->snapshot().active == 0);

    auto fair = std::make_shared<AdmissionBroker>(100, 4, 4);
    auto occupied = fair->tryAcquire("running", 80, 1);
    REQUIRE(occupied && !fair->tryAcquire("heavy-head", 50, 1));
    for (unsigned i = 0; i < 32; ++i) {
        auto small = fair->tryAcquire("backfill-" + std::to_string(i), 10, 1);
        REQUIRE(small);
    }
    REQUIRE(!fair->tryAcquire("after-32", 10, 1));
    occupied.reset();
    auto heavy = fair->tryAcquire("heavy-head", 50, 1);
    REQUIRE(heavy);
    fair->withdraw("after-32");
    heavy.reset();
    auto anotherOccupied = fair->tryAcquire("another-running", 80, 1);
    REQUIRE(!fair->tryAcquire("cancelled-head", 50, 1));
    fair->withdraw("cancelled-head");
    REQUIRE(fair->tryAcquire("after-cancel", 10, 1));
    anotherOccupied.reset();

    auto overrideBroker = std::make_shared<AdmissionBroker>(1000, 4, 4);
    unsigned callbacks = 0;
    REQUIRE(!overrideBroker->raiseLimit(1001, [&] { ++callbacks; return false; }));
    REQUIRE(callbacks == 1 && overrideBroker->snapshot().ramLimit == 1000);
    REQUIRE(!overrideBroker->raiseLimit(2025, [&] { ++callbacks; return true; }));
    REQUIRE(callbacks == 1);
    REQUIRE(overrideBroker->raiseLimit(2024, [&] { ++callbacks; return true; }));
    REQUIRE(callbacks == 2 && overrideBroker->snapshot().ramLimit == 2024);
    REQUIRE(!overrideBroker->raiseLimit(std::numeric_limits<uint64_t>::max(), [&] { ++callbacks; return true; }));
    REQUIRE(callbacks == 2);
    auto extra = overrideBroker->tryAcquire("extra-reservation", 1500, 2);
    REQUIRE(extra);
    REQUIRE(!overrideBroker->restoreLimit(1000, [&] { ++callbacks; return true; }));
    REQUIRE(callbacks == 2 && overrideBroker->snapshot().ramLimit == 2024);
    REQUIRE(overrideBroker->snapshot().restoreState == AdmissionBroker::RestoreState::WaitingReservations);
    REQUIRE(!overrideBroker->snapshot().admissionsBlocked);
    extra.reset();
    REQUIRE(!overrideBroker->restoreLimit(1000, [&] { ++callbacks; return true; }, 1001));
    REQUIRE(callbacks == 2 && overrideBroker->snapshot().restoreState == AdmissionBroker::RestoreState::WaitingCommit);
    REQUIRE(!overrideBroker->snapshot().admissionsBlocked);
    auto finishing = overrideBroker->tryAcquire("finishing-after-update-failure", 100, 1);
    REQUIRE(finishing);
    REQUIRE(!overrideBroker->restoreLimit(1000, [&] { ++callbacks; return false; }));
    REQUIRE(callbacks == 3 && overrideBroker->snapshot().ramLimit == 2024);
    REQUIRE(overrideBroker->snapshot().restoreState == AdmissionBroker::RestoreState::UpdateFailed);
    REQUIRE(overrideBroker->snapshot().admissionsBlocked && !overrideBroker->snapshot().blockReason.empty());
    REQUIRE(!overrideBroker->tryAcquire("blocked-after-update-failure", 1, 1));
    REQUIRE(!overrideBroker->raiseLimit(2025, [&] { ++callbacks; return true; }));
    finishing.reset(); // Existing work can release normally while new admissions are blocked.
    REQUIRE(overrideBroker->snapshot().active == 0);
    REQUIRE(overrideBroker->restoreLimit(1000, [&] { ++callbacks; return true; }));
    REQUIRE(callbacks == 4 && overrideBroker->snapshot().ramLimit == 1000);
    REQUIRE(!overrideBroker->snapshot().admissionsBlocked && overrideBroker->snapshot().blockReason.empty());
    REQUIRE(overrideBroker->snapshot().restoreState == AdmissionBroker::RestoreState::Restored);

    auto draining = std::make_shared<AdmissionBroker>(1000, 4, 4, 100);
    REQUIRE(draining->raiseLimit(1500, [] { return true; }));
    auto live = draining->tryAcquire("live-at-drain", 1400, 1);
    REQUIRE(live && !draining->tryAcquire("stale-drain-waiter", 200, 1));
    draining->beginDrain();
    REQUIRE(draining->snapshot().draining && !draining->snapshot().blockReason.empty());
    REQUIRE(!draining->tryAcquire("new-during-drain", 1, 1));
    REQUIRE(!draining->restoreLimit(1000, [] { return true; }, 1100));
    live.reset();
    REQUIRE(!draining->restoreLimit(1000, [] { return true; }, 1101));
    REQUIRE(draining->restoreLimit(1000, [] { return true; }, 1100));
    REQUIRE(!draining->tryAcquire("still-draining", 1, 1));
    draining->endDrain();
    REQUIRE(draining->tryAcquire("final-assembly", 1000, 4));

    // Impossible requests retained for override must not permanently block backfill.
    auto blockedHead = std::make_shared<AdmissionBroker>(100, 4, 4);
    REQUIRE(!blockedHead->tryAcquire("requires-override", 101, 1));
    for (unsigned i = 0; i < 40; ++i)
        REQUIRE(blockedHead->tryAcquire("small-" + std::to_string(i), 10, 1));
    auto running = blockedHead->tryAcquire("running-for-fairness", 80, 1);
    REQUIRE(running && !blockedHead->tryAcquire("cancelled-feasible-head", 50, 1));
    for (unsigned i = 0; i < 32; ++i)
        REQUIRE(blockedHead->tryAcquire("backfill-cancel-" + std::to_string(i), 10, 1));
    REQUIRE(!blockedHead->tryAcquire("after-cancelled-head", 10, 1));
    blockedHead->withdraw("cancelled-feasible-head");
    REQUIRE(blockedHead->tryAcquire("after-cancelled-head", 10, 1));
    running.reset();

    auto atomicOverride = std::make_shared<AdmissionBroker>(1000, 4, 2, 128);
    unsigned atomicUpdates = 0;
    auto update = [&] { ++atomicUpdates; return true; };
    REQUIRE(!atomicOverride->raiseAndAcquire("plus-1025", 2025, 2, 2025, update));
    REQUIRE(!atomicOverride->raiseAndAcquire("cpu-over", 1500, 5, 1500, update));
    REQUIRE(atomicUpdates == 0 && atomicOverride->snapshot().ramLimit == 1000);
    REQUIRE(!atomicOverride->raiseAndAcquire("update-fails", 1500, 2, 1500, [&] {
        ++atomicUpdates; return false;
    }));
    REQUIRE(atomicUpdates == 1 && atomicOverride->snapshot().ramLimit == 1000 &&
            atomicOverride->snapshot().ramReserved == 0 && atomicOverride->snapshot().active == 0);
    auto pending = atomicOverride->raiseAndAcquire("selected", 1500, 2, 2024, update);
    REQUIRE(pending && atomicUpdates == 2 && atomicOverride->snapshot().ramReserved == 1500);
    REQUIRE(atomicOverride->snapshot().active == 1 && atomicOverride->snapshot().ramLimit == 2024);
    REQUIRE(!atomicOverride->raiseAndAcquire("selected", 1, 1, 2024, update));
    REQUIRE(!atomicOverride->tryAcquire("selected", 1, 1));
    REQUIRE(!atomicOverride->restoreLimit(1000, update)); // The not-yet-launched candidate owns its lease.
    REQUIRE(atomicUpdates == 2);
    auto withinEffective = atomicOverride->raiseAndAcquire("within-effective", 500, 2, 2000, update);
    REQUIRE(withinEffective && atomicUpdates == 2); // No redundant Job update or accumulated +1 GiB.
    REQUIRE(!atomicOverride->raiseAndAcquire("disk-full", 0, 0, 2024, update));
    withinEffective.reset();
    REQUIRE(!atomicOverride->raiseAndAcquire("total-over", 525, 1, 2024, update));
    REQUIRE(!atomicOverride->raiseAndAcquire("repeat-click", 1, 1, 3048, update));
    atomicOverride->beginDrain();
    REQUIRE(!atomicOverride->raiseAndAcquire("during-drain", 1, 1, 2024, update));
    atomicOverride->endDrain();
    pending.reset();
    REQUIRE(atomicOverride->restoreLimit(1000, update, 128));
    REQUIRE(atomicUpdates == 3 && atomicOverride->snapshot().active == 0);
    REQUIRE(atomicOverride->raiseAndAcquire("selected", 100, 1, 1000, update));
    REQUIRE(atomicUpdates == 3); // Releasing a key permits a later independent lease.

    // Sample buffers reserve RAM throughout their lifetime without consuming a disk worker.
    auto sampleBroker = std::make_shared<AdmissionBroker>(100, 4, 2);
    auto sampleBuffer = sampleBroker->tryAcquire("sample-buffer", 20, 0, false, false);
    REQUIRE(sampleBuffer && sampleBroker->snapshot().ramReserved == 20 &&
            sampleBroker->snapshot().active == 0 && sampleBroker->snapshot().cpuReserved == 0);
    auto encodeOne = sampleBroker->tryAcquire("encode-one", 40, 2);
    auto encodeTwo = sampleBroker->tryAcquire("encode-two", 40, 2);
    REQUIRE(encodeOne && encodeTwo && sampleBroker->snapshot().active == 2);
    REQUIRE(!sampleBroker->tryAcquire("disk-blocked", 0, 0));
    REQUIRE(!sampleBroker->tryAcquire("buffer-over-budget", 1, 0, false, false));
    sampleBroker->withdraw("disk-blocked"); sampleBroker->withdraw("buffer-over-budget");
    auto emptyBuffer = sampleBroker->tryAcquire("metadata-buffer", 0, 0, false, false);
    REQUIRE(emptyBuffer && sampleBroker->snapshot().active == 2);
    emptyBuffer.reset();
    REQUIRE(sampleBroker->snapshot().active == 2);
    encodeOne.reset(); encodeTwo.reset();
    REQUIRE(sampleBroker->snapshot().ramReserved == 20 && sampleBroker->snapshot().active == 0);
    sampleBuffer.reset();
    REQUIRE(sampleBroker->snapshot().ramReserved == 0 && sampleBroker->snapshot().cpuReserved == 0);

    auto maximum = std::make_shared<AdmissionBroker>(std::numeric_limits<uint64_t>::max(), 1, 2);
    auto all = maximum->tryAcquire("all", std::numeric_limits<uint64_t>::max(), 1);
    REQUIRE(all && !maximum->tryAcquire("overflow", 1, 0));
    maximum->withdraw("overflow");
    all.reset();
    REQUIRE(maximum->snapshot().ramReserved == 0);
}
void groupCheckpointContracts(const fs::path &root) {
    const auto directory = root / "group";
    fs::create_directories(directory);
    const auto a = root / "a.7z", b = root / "b.7z", c = root / "c.7z";
    write(a, "verified original bytes"); write(b, "CRC-only replacement"); write(c, "verified replacement");
    const auto first = GroupCheckpoint::publish(directory, a,
        {{"configId", "a"}, {"contentVerified", true}});
    bool interrupted = false;
    try {
        GroupCheckpoint::publish(directory, b, {{"configId", "b"}, {"contentVerified", false}},
            [] { throw std::runtime_error("simulated interruption before marker commit"); });
    } catch (const std::runtime_error &) { interrupted = true; }
    REQUIRE(interrupted && fs::is_regular_file(first));
    auto record = GroupCheckpoint::read(directory);
    REQUIRE(record.at("configId") == "a" && record.at("sha256") == fileChecksum(first));
    const auto second = GroupCheckpoint::publish(directory, b,
        {{"configId", "b"}, {"contentVerified", false}});
    record = GroupCheckpoint::read(directory);
    REQUIRE(record.at("contentFallback").at("configId") == "a" && fs::is_regular_file(first));
    GroupCheckpoint::copyTo(directory, root / "copy");
    const auto copied = GroupCheckpoint::read(root / "copy");
    REQUIRE(fileChecksum(GroupCheckpoint::artifact(root / "copy", copied)) == fileChecksum(second));
    REQUIRE(fileChecksum(GroupCheckpoint::artifact(root / "copy", copied.at("contentFallback"))) == fileChecksum(first));
    const auto third = GroupCheckpoint::publish(directory, c,
        {{"configId", "c"}, {"contentVerified", true}});
    REQUIRE(fs::is_regular_file(third) && !fs::exists(first) && !fs::exists(second));
    REQUIRE(!GroupCheckpoint::read(directory).contains("contentFallback"));
    // Matching metadata must not reuse an artifact whose bytes were damaged.
    write(third, "damaged bytes");
    const auto repaired = GroupCheckpoint::publish(directory, c,
        {{"configId", "c"}, {"contentVerified", true}});
    REQUIRE(repaired != third && fileChecksum(repaired) == fileChecksum(c));
    bool rejected = false;
    try { GroupCheckpoint::artifact(directory, {{"artifact", "../outside.7z"}}); }
    catch (const std::runtime_error &) { rejected = true; }
    REQUIRE(rejected);
}
int main() {
    auto root = fs::temp_directory_path() / ("ultraarch-contracts-" + uniqueId());
    fs::create_directory(root);
    struct Cleanup {
        fs::path p;
        ~Cleanup() {
            std::error_code ec;
            fs::remove_all(p, ec);
        }
    } cleanup{root};
    try {
        admissionBrokerContracts();
        groupCheckpointContracts(root / "checkpoint");
        REQUIRE(Hasher::text("") ==
                "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        REQUIRE(Hasher::text("abc") ==
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        REQUIRE(Hasher::text(std::string(1000000, 'a')) ==
                "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
        Hasher chunks;
        for (int n = 0; n < 1000; ++n)
            chunks.update("abc", 3);
        REQUIRE(chunks.finish() == Hasher::text(std::string("") += [] {
                    std::string s;
                    for (int n = 0; n < 1000; ++n)
                        s += "abc";
                    return s;
                }()));
        auto input = root / "input";
        auto file = input / "bytes.txt";
        write(file, std::string(262145, 'a'));
        fs::create_directories(input / "empty" / "nested");
        auto before = InputManifest::scan(input);
        REQUIRE(before.bytesRead == 262145 && before.summary.files.size() == 1 &&
                before.summary.emptyDirectories.size() == 1);
        REQUIRE(before.summary.files.front().contentChecksum == fileChecksum(file));
        auto modified = fs::last_write_time(file);
        write(file, std::string(262145, 'b'));
        fs::last_write_time(file, modified);
        REQUIRE(before.fingerprint != InputManifest::scan(input).fingerprint);
        auto settings = OptimizerSettings{};
        settings.cpuThreads = 1;
        settings.workerThreads = 4;
        settings.ramLimitMB = 256;
        ResourceScheduler backfill{100, 80, 4, 1};
        std::vector<TaskConfig> admission(35);
        std::vector<CandidateResult> waiting(35);
        admission.front().estimatedRamMB = 50;
        for (size_t i = 1; i < admission.size(); ++i)
            admission[i].estimatedRamMB = 10;
        for (size_t i = 1; i <= 32; ++i) {
            REQUIRE(backfill.select(admission, waiting, waiting.size()) == i);
            waiting[i].status = CandidateStatus::Verified;
        }
        REQUIRE(backfill.select(admission, waiting, waiting.size()) == admission.size());
        backfill.release(60, 1);
        REQUIRE(backfill.select(admission, waiting, waiting.size()) == 0);
        AnalysisSummary summary;
        summary.totalBytes = 8 * 1048576;
        summary.totalFiles = 1;
        TaskMatrixGenerator generator;
        auto a = generator.generateMatrix(summary, settings);
        settings.password = "test secret";
        auto b = generator.generateMatrix(summary, settings);
        REQUIRE(a.size() == b.size());
        bool heavy = false;
        for (size_t n = 0; n < a.size(); ++n) {
            REQUIRE(a[n].configId == b[n].configId);
            REQUIRE(a[n].estimatedRamMB <= settings.ramLimitMB + 1024);
            REQUIRE(a[n].resourceBlocked == (a[n].estimatedRamMB > settings.ramLimitMB));
            if (a[n].estimatedRamMB > settings.ramLimitMB / settings.workerThreads)
                heavy = true;
        }
        REQUIRE(heavy);
        CodecSpec classic;
        classic.codec = Codec::LZMA1;
        classic.lc = 8;
        CompressionSpec classicSpec;
        classicSpec.methods = {classic};
        REQUIRE(classicSpec.compile().front().find("lzma:") != std::string::npos);
        auto invalidContext = classicSpec;
        invalidContext.methods.front().codec = Codec::LZMA2;
        bool contextRejected = false;
        try { invalidContext.validate(); } catch (const std::invalid_argument &) { contextRejected = true; }
        REQUIRE(contextRejected);
        auto expandedSettings = settings;
        expandedSettings.profile = OptimizationProfile::Extreme_RePack;
        expandedSettings.ramLimitMB = 8192;
        AnalysisSummary expandedSummary;
        expandedSummary.totalBytes = 512ULL * 1048576;
        expandedSummary.totalFiles = 10;
        expandedSummary.hasExecutables = expandedSummary.hasUncompressedMedia = true;
        auto expanded = generator.generateMatrix(expandedSummary, expandedSettings);
        bool largeBcj2 = false, highPpmd = false, delta31 = false;
        for (const auto &task : expanded) {
            largeBcj2 |= task.spec.methods.front().codec == Codec::BCJ2 &&
                         task.spec.methods[1].dictionary.value >= 64ULL * 1048576;
            highPpmd |= task.spec.methods.front().codec == Codec::PPMd &&
                        task.spec.methods.front().order == 32;
            delta31 |= task.spec.methods.front().codec == Codec::Delta &&
                       task.spec.methods.front().delta == 31;
        }
        REQUIRE(largeBcj2 && highPpmd && delta31);
        auto ceilingSettings = expandedSettings;
        ceilingSettings.maxLzmaDictionaryMB = 1024;
        AnalysisSummary forty;
        forty.totalBytes = 40ULL * 1048576;
        forty.totalFiles = 2;
        forty.files = {{"a.bin", 20ULL * 1048576}, {"b.bin", 20ULL * 1048576}};
        forty.hasExecutables = true;
        auto fortyMatrix = generator.generateMatrix(forty, ceilingSettings);
        REQUIRE(!fortyMatrix.empty());
        for (const auto &task : fortyMatrix)
            for (const auto &method : task.spec.methods)
                if (method.codec == Codec::LZMA1 || method.codec == Codec::LZMA2)
                    REQUIRE(method.dictionary.value <= forty.totalBytes);
        REQUIRE(DictionaryPolicy::streamBytes(forty, "off") == 20ULL * 1048576);
        REQUIRE(DictionaryPolicy::streamBytes(forty, "8m") == 20ULL * 1048576);
        REQUIRE(DictionaryPolicy::streamBytes(forty, "e") == forty.totalBytes);
        forty.files.back().path = "b.txt";
        REQUIRE(DictionaryPolicy::streamBytes(forty, "e") == 20ULL * 1048576);
        forty.files.front().path = ".bin";
        forty.files.back().path = "b.bin";
        REQUIRE(DictionaryPolicy::streamBytes(forty, "e") == forty.totalBytes);
        auto incomplete = forty;
        incomplete.files.pop_back();
        REQUIRE(DictionaryPolicy::streamBytes(incomplete, "off") == forty.totalBytes);
        CodecSpec rounded;
        rounded.dictionary = MiB{40}.bytes();
        REQUIRE(DictionaryPolicy::decoderBytes(rounded) == 48ULL * 1048576);
        rounded.codec = Codec::LZMA1;
        REQUIRE(DictionaryPolicy::decoderBytes(rounded) == 40ULL * 1048576);
        rounded.dictionary = Bytes{40ULL * 1048576 + 1024};
        REQUIRE(DictionaryPolicy::decoderBytes(rounded) == 41ULL * 1048576);
        rounded.dictionary = Bytes{1024};
        REQUIRE(DictionaryPolicy::decoderBytes(rounded) == 4096);
        rounded.dictionary = Bytes{5 * 1024};
        REQUIRE(DictionaryPolicy::decoderBytes(rounded) == 6 * 1024);
        rounded.dictionary = Bytes{1048576 + 1024};
        REQUIRE(DictionaryPolicy::decoderBytes(rounded) == 1572864);
        // The display correction must preserve MiB reservations used by the
        // measured search for every dictionary emitted by our KiB policy.
        for (uint64_t bytes = 1024; bytes <= 1536ULL * 1048576; bytes += 1024) {
            rounded.dictionary = Bytes{bytes};
            REQUIRE((DictionaryPolicy::decoderBytes(rounded) + 1048575) / 1048576 ==
                    (bytes + 1048575) / 1048576);
        }
        ceilingSettings.maxLzmaDictionaryMB = 16;
        auto bounded = generator.generateMatrix(forty, ceilingSettings);
        for (const auto &task : bounded)
            for (const auto &method : task.spec.methods)
                if (method.codec == Codec::LZMA1 || method.codec == Codec::LZMA2)
                    REQUIRE(method.dictionary.value <= 16ULL * 1048576);
        REQUIRE(std::any_of(bounded.begin(), bounded.end(), [](const auto &task) {
            return task.spec.methods.front().codec == Codec::PPMd &&
                   task.spec.methods.front().dictionary.value > 16ULL * 1048576;
        }));
        REQUIRE(settingsFromJson(settingsJson(ceilingSettings)).maxLzmaDictionaryMB == 16);
        REQUIRE(settingsFromJson(Json::object()).maxLzmaDictionaryMB == 0);
        bool invalidDictionary = false;
        try { settingsFromJson(Json{{"maxLzmaDictionaryMB", -1}}); }
        catch (const std::invalid_argument &) { invalidDictionary = true; }
        REQUIRE(invalidDictionary);
        ProcessResult capability;
        capability.success = true;
        capability.exitCode = 0;
        REQUIRE(ToolCapabilities::classifyProbe(capability) == CapabilityStatus::Supported);
        capability.timedOut = true;
        REQUIRE(ToolCapabilities::classifyProbe(capability) == CapabilityStatus::Inconclusive);
        capability.timedOut = false;
        capability.success = false;
        capability.exitCode = 7;
        REQUIRE(ToolCapabilities::classifyProbe(capability) == CapabilityStatus::Unsupported);
        capability.killedByOOM = true;
        REQUIRE(ToolCapabilities::classifyProbe(capability) == CapabilityStatus::Inconclusive);
        capability.killedByOOM = false;
        capability.exitCode = 2;
        REQUIRE(ToolCapabilities::classifyProbe(capability) == CapabilityStatus::Inconclusive);
        capability.errOutput = "Unsupported Method";
        REQUIRE(ToolCapabilities::classifyProbe(capability) == CapabilityStatus::Unsupported);
        capability.cancelled = true;
        REQUIRE(ToolCapabilities::classifyProbe(capability) == CapabilityStatus::Inconclusive);
        std::vector<FileTypeInfo> peCoverage = {{"small.exe", 65536}, {"large.dll", 1048576}};
        for (auto &file : peCoverage) file.category = FileCategory::Executable_PE;
        REQUIRE(ProbePlanner::wholePeCovered(peCoverage, {{0,0,65536,""},{1,0,1048576,""}}));
        REQUIRE(!ProbePlanner::wholePeCovered(peCoverage, {{0,0,65536,""}}));
        REQUIRE(!ProbePlanner::wholePeCovered(peCoverage, {{0,0,65536,""},{1,0,65536,""}}));
        expandedSummary.totalBytes = 62ULL * 1048576;
        expandedSummary.hasExecutables = expandedSummary.hasUncompressedMedia = false;
        expandedSummary.hasStructuredText = true;
        auto extendedCap = generator.generateMatrix(expandedSummary, expandedSettings);
        for (auto dictionary : {48ULL, 62ULL})
            REQUIRE(std::any_of(extendedCap.begin(), extendedCap.end(), [&](const auto &task) {
                return task.spec.methods.size() == 1 &&
                       task.spec.methods.front().codec == Codec::LZMA1 &&
                       task.spec.methods.front().lc == 4 && task.spec.methods.front().pb == 0 &&
                       task.spec.methods.front().dictionary.value == dictionary * 1048576;
            }));
        std::vector<uint64_t> probes(a.size(), 100);
        probes.back() = 0;
        auto finalists = SearchPlanner::finalists(a, probes, SearchMode::Balanced, 1.05);
        REQUIRE(std::any_of(finalists.begin(), finalists.end(),
                            [&](const auto &t) { return t.configId == a.back().configId; }));
        REQUIRE(SearchPlanner::finalists(a, probes, SearchMode::Exhaustive, 1.05).size() ==
                a.size());
        auto refinement = SearchPlanner::refinement(a, {}, a[a.size() / 2].configId);
        REQUIRE(refinement.size() <= 8);
        auto encoded = settingsJson(settings);
        REQUIRE(encoded.dump().find("test secret") == std::string::npos);
        auto legacySearch = settings;
        legacySearch.searchMode = SearchMode::Exhaustive;
        legacySearch.enableAdaptivePruning = true;
        auto migratedSearch = normalizeSearchSettings(legacySearch);
        REQUIRE(migratedSearch.searchMode == SearchMode::Exhaustive);
        REQUIRE(!migratedSearch.enableAdaptivePruning);
        REQUIRE(normalizeSearchSettings(migratedSearch).searchMode == SearchMode::Exhaustive);
        legacySearch.enableAdaptivePruning = false;
        REQUIRE(normalizeSearchSettings(legacySearch).searchMode == SearchMode::Exhaustive);
        settings.searchMode = SearchMode::Balanced;
        settings.selectionGoal = SelectionGoal::FastWithinTolerance;
        settings.contentPolicy = ContentPolicy::OptimizeResources;
        settings.solidBlockSize = "384m";
        settings.ramLimitMB = 1777;
        REQUIRE(settingsJson(settingsFromJson(settingsJson(settings))) == settingsJson(settings));
        AnalysisSummary grouped;
        for (auto name : {"module.exe", "library.dll"}) {
            FileTypeInfo file;
            file.path = name;
            file.category = FileCategory::Executable_PE;
            file.sizeBytes = 65536;
            grouped.files.push_back(file);
        }
        REQUIRE(GroupPlanner::plan(grouped).size() == 1);
        FileTypeInfo unrelated;
        unrelated.path = "audio.wav";
        unrelated.category = FileCategory::Uncompressed_Media;
        unrelated.sizeBytes = 65536;
        grouped.files.push_back(unrelated);
        REQUIRE(GroupPlanner::plan(grouped).size() == 2);
        std::vector<FileTypeInfo> sampleInput;
        for (size_t n = 0; n < 120; ++n) {
            FileTypeInfo file;
            file.path = "text-" + std::to_string(n) + ".lua";
            file.category = FileCategory::Text_Database;
            file.sizeBytes = n < 100 ? 1024 : 1048576;
            sampleInput.push_back(file);
        }
        for (auto bytes : {32768ULL, 16ULL * 1048576}) {
            FileTypeInfo file;
            file.path = "program-" + std::to_string(bytes) + ".exe";
            file.category = FileCategory::Executable_PE;
            file.sizeBytes = bytes;
            sampleInput.push_back(file);
        }
        auto fragments = ProbePlanner::plan(sampleInput, 8 * 1048576);
        auto repeatedFragments = ProbePlanner::plan(sampleInput, 8 * 1048576);
        REQUIRE(fragments.size() == repeatedFragments.size());
        uint64_t sampleBytes = 0;
        bool sawSmall = false, sawLarge = false, sawPe = false;
        for (size_t n = 0; n < fragments.size(); ++n) {
            const auto &part = fragments[n];
            const auto &file = sampleInput[part.fileIndex];
            REQUIRE(part.bytes && part.offset + part.bytes <= file.sizeBytes);
            REQUIRE(part.fileIndex == repeatedFragments[n].fileIndex &&
                    part.offset == repeatedFragments[n].offset &&
                    part.bytes == repeatedFragments[n].bytes);
            sampleBytes += part.bytes;
            sawSmall |= file.sizeBytes == 1024;
            sawLarge |= file.sizeBytes == 1048576;
            if (file.category == FileCategory::Executable_PE) {
                sawPe = true;
                REQUIRE(part.offset == 0 && part.bytes == file.sizeBytes);
            }
        }
        REQUIRE(sampleBytes > 4 * 1048576 && sampleBytes <= 8 * 1048576);
        REQUIRE(sawSmall && sawLarge && sawPe);
        REQUIRE(ProbePlanner::plan({sampleInput.back()}, 8 * 1048576).empty());
        std::vector<TaskConfig> aliases(3);
        for (size_t n = 0; n < aliases.size(); ++n) {
            CodecSpec method;
            method.codec = Codec::LZMA1;
            method.dictionary = MiB{n ? 64ULL : 1ULL}.bytes();
            if (n == 2)
                method.lc = 8;
            aliases[n].spec.methods = {method};
            aliases[n].configId = aliases[n].spec.id();
            aliases[n].estimatedRamMB = aliases[n].spec.memoryMB();
        }
        auto diverse = SearchPlanner::finalists(aliases, {100, 100, 101}, SearchMode::Balanced, 0.5);
        REQUIRE(diverse.size() == 2);
        REQUIRE(diverse[0].configId == aliases[1].configId &&
                diverse[1].configId == aliases[2].configId);
        auto dictionaryRefinement = SearchPlanner::refinement(
            {aliases[0], aliases[2], aliases[1]}, {aliases[1], aliases[2]}, aliases[1].configId);
        REQUIRE(dictionaryRefinement.size() == 1 &&
                dictionaryRefinement.front().configId == aliases[0].configId);
        std::vector<TaskConfig> deltaContexts;
        for (auto lc : {3u, 7u})
            for (auto stride : {1u, 32u}) {
                CodecSpec filter, method;
                filter.codec = Codec::Delta;
                filter.delta = stride;
                method.codec = Codec::LZMA1;
                method.lc = lc;
                method.pb = lc == 7 ? 4 : 2;
                TaskConfig task;
                task.spec.methods = {filter, method};
                task.configId = task.spec.id();
                deltaContexts.push_back(task);
            }
        // The globally smaller context must not crowd out the other Delta context.
        auto deltaFinalists = SearchPlanner::finalists(
            deltaContexts, {100, 101, 200, 201}, SearchMode::Balanced, 0.5);
        REQUIRE(deltaFinalists.size() == 4);
        REQUIRE(SearchPlanner::finalists(deltaContexts, {100, 101, 200, 201},
                                         SearchMode::Fast, 0.5).size() == 2);
        std::vector<TaskConfig> contexts;
        for (auto lc : {5u, 2u, 4u, 6u, 3u}) {
            TaskConfig task;
            CodecSpec method;
            method.codec = Codec::LZMA1;
            method.lc = lc;
            method.pb = 0;
            task.spec.methods = {method};
            task.configId = task.spec.id();
            contexts.push_back(task);
        }
        auto neighbours = SearchPlanner::refinement(contexts, {contexts[2]}, contexts[2].configId);
        REQUIRE(neighbours.size() == 2);
        REQUIRE(neighbours[0].configId == contexts[4].configId &&
                neighbours[1].configId == contexts[0].configId);
        std::reverse(contexts.begin(), contexts.end());
        auto reordered = SearchPlanner::refinement(contexts, {contexts[2]}, contexts[2].configId);
        REQUIRE(reordered.size() == 2 && reordered[0].configId == neighbours[0].configId &&
                reordered[1].configId == neighbours[1].configId);
        REQUIRE(SearchPlanner::refinement(contexts, contexts, contexts[2].configId).empty());
        settings.cpuLimitPercent = 25;
        settings.diskTaskLimit = 1;
        REQUIRE(settingsJson(settingsFromJson(settingsJson(settings))) == settingsJson(settings));
        auto bad = a.at(1).spec;
        bad.methods.front().lc = 4;
        bad.methods.front().lp = 4;
        bool rejected = false;
        try {
            bad.compile();
        } catch (...) {
            rejected = true;
        }
        REQUIRE(rejected);
        for (const auto &path : {"../escape", "C:/escape", "a/../../escape", "a:stream", "CON.txt",
                                 "a/./b", "/absolute"}) {
            rejected = false;
            try {
                WorkspaceManager::validateArchivePath(path);
            } catch (...) {
                rejected = true;
            }
            if (!rejected)
                throw std::runtime_error(std::string("Unsafe archive path accepted: ") + path);
        }
        auto output = root / "out.7z";
        write(output, "previous archive");
        {
            PublicationTransaction tx(output);
            auto fresh = tx.stage() / "new" / output.filename();
            write(fresh, "new archive");
            tx.verified({fresh});
            std::atomic<bool> cancel{true};
            try {
                tx.publish(&cancel);
            } catch (...) {
                tx.rollback();
            }
            REQUIRE(fileChecksum(output) == Hasher::text("previous archive"));
        }
        {
            PublicationTransaction tx(output);
            auto fresh = tx.stage() / "new" / output.filename();
            write(fresh, "new archive");
            tx.verified({fresh});
            write(output, "external change");
            rejected = false;
            try {
                tx.publish();
            } catch (...) {
                rejected = true;
            }
            REQUIRE(rejected && fileChecksum(output) == Hasher::text("external change"));
            auto pending = PublicationTransaction::pending(root);
            REQUIRE(!pending.empty());
            rejected = false;
            try {
                tx.rollback();
            } catch (...) {
                rejected = true;
            }
            REQUIRE(rejected && fileChecksum(output) == Hasher::text("external change"));
        }
        std::cout << "SHA-256 vectors, input mutation, stable IDs, global-budget matrix, settings, "
                     "unsafe paths and publication ownership passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

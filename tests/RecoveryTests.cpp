#include "core/ProcessManager.hpp"
#include "core/PublicationTransaction.hpp"
#include "core/RamCandidate.hpp"
#include "core/GroupCheckpoint.hpp"
#include <fstream>
#include <iostream>
using namespace UltraArch;
namespace fs = std::filesystem;
void write(const fs::path &path, const std::string &text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}
int main(int argc, char **argv) {
    if (argc == 5 && std::string(argv[1]) == "--checkpoint-helper") {
        const auto root = utf8Path(argv[2]);
        const std::string phase = argv[3], storage = argv[4];
        const auto source = root / "candidate.bin";
        const std::string bytes = "new verified generation";
        if (storage == "ram") {
            auto budget = std::make_shared<RamBudget>(1048576);
            auto artifact = RamArtifact::allocate(budget, bytes.size(), bytes.size());
            if (!artifact) return 2;
            artifact->write(0, bytes.data(), bytes.size());
            artifact->size = bytes.size();
            artifact->materialize(source);
        } else write(source, bytes);
        // Payloads test durability, not archive decoding. Engine integration tests
        // separately verify that restored leaders contain the original files.
        if (phase == "BeforeVerification") std::_Exit(86);
        GroupCheckpoint::publish(root, source,
            {{"configId", "new"}, {"contentVerified", true}}, [&] {
                if (phase == "BeforeCommit") std::_Exit(86);
            });
        std::_Exit(86);
    }
    if (argc > 2 && std::string(argv[1]) == "--crash-helper") {
        auto root = utf8Path(argv[2]);
        PublicationTransaction tx(root / "out.7z");
        auto a = tx.stage() / "new" / "out.7z.001", b = tx.stage() / "new" / "out.7z.002";
        auto stage = [&](const fs::path &path, const std::string &bytes) {
            if (argc > 3 && std::string(argv[3]) == "ram") {
                auto budget = std::make_shared<RamBudget>(1048576);
                auto artifact = RamArtifact::allocate(budget, bytes.size(), bytes.size());
                if (!artifact) throw std::runtime_error("Cannot allocate recovery RAM artifact");
                artifact->write(0, bytes.data(), bytes.size()); artifact->size = bytes.size();
                artifact->materialize(path);
            } else write(path, bytes);
        };
        stage(a, "new first volume");
        stage(b, "new second volume");
        tx.verified({a, b});
        tx.publish();
        return 0;
    }
    auto root = fs::temp_directory_path() / ("ultraarch-recovery-" + uniqueId());
    fs::create_directory(root);
    struct Cleanup {
        fs::path p;
        ~Cleanup() {
            std::error_code ec;
            fs::remove_all(p, ec);
        }
    } cleanup{root};
#ifdef _WIN32
    wchar_t module[32768]{};
    GetModuleFileNameW(nullptr, module, 32768);
    auto exe = fs::path(module);
#else
    auto exe = fs::absolute(utf8Path(argv[0]));
#endif
    try {
        for (const auto &storage : {"disk", "ram"}) {
#ifndef _WIN32
            if (std::string(storage) == "ram") continue;
#endif
            for (const auto &phase : {"BeforeVerification", "BeforeCommit", "AfterCommit"}) {
                const auto dir = root / (std::string("checkpoint-") + storage + "-" + phase);
                fs::create_directory(dir);
                write(dir / "old.bin", "old verified generation");
                write(dir / "unrelated.txt", "retain unrelated data");
                const auto old = GroupCheckpoint::publish(dir, dir / "old.bin",
                    {{"configId", "old"}, {"contentVerified", true}});
                ProcessControl control{nullptr, 10};
                auto process = ProcessManager::execute(pathUtf8(exe),
                    {"--checkpoint-helper", pathUtf8(dir), phase, storage}, dir,
                    nullptr, 0, nullptr, nullptr, &control);
                if (process.exitCode != 86) throw std::runtime_error("Checkpoint helper did not crash");
                const auto marker = GroupCheckpoint::read(dir);
                const auto artifact = GroupCheckpoint::artifact(dir, marker);
                const bool committed = std::string(phase) == "AfterCommit";
                const auto expected = Hasher::text(committed ? "new verified generation" : "old verified generation");
                if (fileChecksum(artifact) != expected || marker.at("sha256") != expected ||
                    marker.at("configId") != (committed ? "new" : "old") ||
                    (!committed && (!fs::exists(old) || fileChecksum(old) != expected)) ||
                    fileChecksum(dir / "unrelated.txt") != Hasher::text("retain unrelated data"))
                    throw std::runtime_error("Checkpoint crash lost the committed generation");
                const auto restored = dir / "restored";
                GroupCheckpoint::copyTo(dir, restored);
                if (fileChecksum(GroupCheckpoint::artifact(restored, GroupCheckpoint::read(restored))) != expected)
                    throw std::runtime_error("Checkpoint copy after crash changed bytes");
                std::cout << "[PASS] checkpoint " << storage << " " << phase << '\n';
            }
            const auto dir = root / (std::string("checkpoint-cancel-") + storage);
            fs::create_directory(dir);
            write(dir / "old.bin", "old verified generation");
            const std::string replacement = "new verified generation";
            if (std::string(storage) == "ram") {
                auto budget = std::make_shared<RamBudget>(1048576);
                auto artifact = RamArtifact::allocate(budget, replacement.size(), replacement.size());
                if (!artifact) throw std::runtime_error("Cannot allocate checkpoint fixture");
                artifact->write(0, replacement.data(), replacement.size());
                artifact->size = replacement.size();
                artifact->materialize(dir / "new.bin");
            } else write(dir / "new.bin", replacement);
            const auto old = GroupCheckpoint::publish(dir, dir / "old.bin",
                {{"configId", "old"}, {"contentVerified", true}});
            const auto markerHash = fileChecksum(dir / "winner.json");
            for (int repeat = 0; repeat != 2; ++repeat) {
                bool cancelled = false;
                try {
                    GroupCheckpoint::publish(dir, dir / "new.bin",
                        {{"configId", "new"}, {"contentVerified", true}}, [] {
                            throw std::runtime_error("Cancelled before checkpoint commit");
                        });
                } catch (const std::runtime_error &) { cancelled = true; }
                if (!cancelled || fileChecksum(dir / "winner.json") != markerHash ||
                    fileChecksum(old) != Hasher::text("old verified generation"))
                    throw std::runtime_error("Cancelled checkpoint replaced its predecessor");
            }
            std::cout << "[PASS] checkpoint repeated cancellation " << storage << '\n';
#ifdef _WIN32
            if (!SetFileAttributesW((dir / "winner.json").c_str(), FILE_ATTRIBUTE_READONLY))
                throw std::runtime_error("Cannot protect marker for failure test");
            bool failed = false;
            try {
                GroupCheckpoint::publish(dir, dir / "new.bin",
                    {{"configId", "new"}, {"contentVerified", true}});
            } catch (const std::runtime_error &) { failed = true; }
            SetFileAttributesW((dir / "winner.json").c_str(), FILE_ATTRIBUTE_NORMAL);
            if (!failed || fileChecksum(dir / "winner.json") != markerHash ||
                fileChecksum(old) != Hasher::text("old verified generation"))
                throw std::runtime_error("Failed marker replacement lost its predecessor");
            std::cout << "[PASS] checkpoint marker failure " << storage << '\n';
#endif
            GroupCheckpoint::publish(dir, dir / "new.bin",
                {{"configId", "unchecked"}, {"contentVerified", false}});
            const auto unchecked = GroupCheckpoint::read(dir);
            if (!unchecked.contains("contentFallback") ||
                GroupCheckpoint::artifact(dir, unchecked.at("contentFallback")) != old ||
                fileChecksum(old) != Hasher::text("old verified generation"))
                throw std::runtime_error("Unchecked generation lost verified fallback");
            const auto copied = dir / "fallback-copy";
            GroupCheckpoint::copyTo(dir, copied);
            const auto copiedMarker = GroupCheckpoint::read(copied);
            if (fileChecksum(GroupCheckpoint::artifact(copied, copiedMarker.at("contentFallback"))) !=
                Hasher::text("old verified generation"))
                throw std::runtime_error("Checkpoint copy lost verified fallback");
            std::cout << "[PASS] checkpoint verified fallback " << storage << '\n';
        }
        for (const auto &phase :
             {"Prepared", "Verified", "BackingUp", "Publishing", "AfterFile", "Committed"}) {
          for (const auto &storage : {"disk", "ram"}) {
#ifndef _WIN32
            if (std::string(storage) == "ram") continue;
#endif
            auto dir = root / (std::string(phase) + "-" + storage);
            fs::create_directory(dir);
            write(dir / "out.7z.001", "old first volume");
            write(dir / "out.7z.002", "old second volume");
            write(dir / "unknown.txt", "do not remove");
#ifdef _WIN32
            SetEnvironmentVariableA("ULTRAARCH_CRASH_PHASE", phase);
#else
            setenv("ULTRAARCH_CRASH_PHASE", phase, 1);
#endif
            ProcessControl control{nullptr, 10};
            auto process = ProcessManager::execute(pathUtf8(exe), {"--crash-helper", pathUtf8(dir), storage},
                                                   dir, nullptr, 0, nullptr, nullptr, &control);
#ifdef _WIN32
            SetEnvironmentVariableA("ULTRAARCH_CRASH_PHASE", nullptr);
#else
            unsetenv("ULTRAARCH_CRASH_PHASE");
#endif
            if (process.exitCode != 86)
                throw std::runtime_error("Fault injection did not terminate child");
            auto pending = PublicationTransaction::pending(dir);
            if (pending.size() != 1)
                throw std::runtime_error("Durable journal missing");
            PublicationTransaction::recover(pending.front());
            bool committed = std::string(phase) == "Committed";
            if (fileChecksum(dir / "out.7z.001") !=
                    Hasher::text(committed ? "new first volume" : "old first volume") ||
                fileChecksum(dir / "out.7z.002") !=
                    Hasher::text(committed ? "new second volume" : "old second volume") ||
                fileChecksum(dir / "unknown.txt") != Hasher::text("do not remove"))
                throw std::runtime_error("Crash recovery changed unrelated/previous output");
            std::cout << "[PASS] recovery " << storage << " " << phase << '\n';
          }
        }
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

#include "ToolCapabilities.hpp"
#include "ProcessManager.hpp"
#include "SettingsJson.hpp"
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace UltraArch {
namespace {
struct CapabilityEntry {
    std::condition_variable ready;
    bool running = true;
    CapabilityStatus status = CapabilityStatus::Inconclusive;
};
std::mutex registryMutex;
std::unordered_map<std::string, std::shared_ptr<CapabilityEntry>> registry;
// Persistent cache transactions are short; actual probes never hold this mutex.
std::mutex cacheMutex;
std::atomic<uint64_t> directorySequence{0};

bool cancelled(const std::atomic<bool> *cancel) {
    return cancel && cancel->load();
}
Json readCache(const std::filesystem::path &path) {
    Json cache = Json::object();
    try {
        if (std::filesystem::file_size(path) < 4 * 1048576) {
            std::ifstream in(path);
            in >> cache;
        }
    } catch (...) {
    }
    if (!cache.is_object())
        cache = Json::object();
    return cache;
}
void publish(const std::string &key, const std::shared_ptr<CapabilityEntry> &entry,
             CapabilityStatus status) {
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        entry->status = status;
        entry->running = false;
        // Resource failures are not a statement about the backend, even in this session.
        if (status == CapabilityStatus::Inconclusive)
            registry.erase(key);
    }
    entry->ready.notify_all();
}
} // namespace

CapabilityStatus ToolCapabilities::classifyProbe(const ProcessResult &process) {
    const auto output = process.stdOutput + "\n" + process.errOutput;
    if (process.cancelled || process.timedOut || process.killedByOOM || process.win32Error != 0 ||
        process.exitCode < 0 || process.exitCode == 8 || process.exitCode == 255 ||
        output.find("Out of memory") != std::string::npos ||
        output.find("Can't allocate required memory") != std::string::npos)
        return CapabilityStatus::Inconclusive;
    if (process.success)
        return CapabilityStatus::Supported;
    // Exit 2 is also used for I/O, bad archives and environmental failures. Only explicit
    // method/argument rejection proves incompatibility with this normalized configuration.
    if (process.exitCode == 7 || output.find("Unsupported Method") != std::string::npos ||
        output.find("Unsupported method") != std::string::npos ||
        output.find("Unsupported compression method") != std::string::npos)
        return CapabilityStatus::Unsupported;
    return CapabilityStatus::Inconclusive;
}

std::vector<TaskConfig> ToolCapabilities::check(const std::vector<TaskConfig> &tasks,
                                              const std::filesystem::path &tool,
                                              const std::filesystem::path &scratch,
                                              ResourceGuard *guard,
                                              const std::atomic<bool> *cancel,
                                              std::function<void(const std::string &)> log) {
    auto identity = fileChecksum(tool, cancel);
    auto dll = tool.parent_path() / "7z.dll";
    if (std::filesystem::is_regular_file(dll))
        identity = Hasher::text(identity + fileChecksum(dll, cancel));
    // Do not trust old negative records: earlier versions cached decoder resource failures.
    const auto cachePath = scratch.parent_path() / "capabilities-v2-sha256.json";
    Json cache;
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        cache = readCache(cachePath);
    }
    std::filesystem::path directory;
    struct Cleanup {
        std::filesystem::path &path;
        ~Cleanup() {
            if (!path.empty()) {
                std::error_code ec;
                std::filesystem::remove_all(path, ec);
            }
        }
    } cleanup{directory};
    const auto prepareDirectory = [&] {
        if (!directory.empty())
            return;
        // Each concurrent caller owns its smoke files, including different keys in one scratch.
        for (;;) {
            auto candidate = scratch / ("capability-smoke-" + std::to_string(++directorySequence));
            if (std::filesystem::create_directories(candidate)) {
                directory = std::move(candidate);
                break;
            }
        }
        std::ofstream out(directory / "smoke.bin", std::ios::binary);
        for (int n = 0; n < 4096; ++n)
            out << "UltraArch codec smoke test\n";
        if (!out)
            throw std::runtime_error("Cannot write capability smoke input");
    };

    std::vector<TaskConfig> result;
    for (const auto &task : tasks) {
        if (cancelled(cancel))
            throw std::runtime_error("Cancelled");
        auto spec = task.spec;
        for (auto &m : spec.methods)
            if (m.codec == Codec::LZMA1 || m.codec == Codec::LZMA2 || m.codec == Codec::PPMd)
                m.dictionary = MiB{1}.bytes();
        spec.threads = 1;
        const auto key = "v2:" + identity + ":" + spec.id();
        CapabilityStatus status = CapabilityStatus::Inconclusive;
        if (cache.contains(key) && cache[key].is_boolean()) {
            status = cache[key].get<bool>() ? CapabilityStatus::Supported
                                           : CapabilityStatus::Unsupported;
        } else {
            std::shared_ptr<CapabilityEntry> entry;
            bool owner = false;
            {
                std::unique_lock<std::mutex> lock(registryMutex);
                auto found = registry.find(key);
                if (found == registry.end()) {
                    entry = std::make_shared<CapabilityEntry>();
                    registry.emplace(key, entry);
                    owner = true;
                } else {
                    entry = found->second;
                    while (entry->running) {
                        if (cancelled(cancel))
                            throw std::runtime_error("Cancelled");
                        entry->ready.wait_for(lock, std::chrono::milliseconds(50));
                    }
                    status = entry->status;
                }
            }
            if (owner) {
                try {
                    prepareDirectory();
                    const auto archive = directory / "smoke.7z";
                    std::error_code ec;
                    std::filesystem::remove(archive, ec);
                    std::vector<std::string> args = {"a", "-y", "-t7z"};
                    const auto flags = spec.compile();
                    args.insert(args.end(), flags.begin(), flags.end());
                    args.push_back(pathUtf8(archive));
                    args.push_back(pathUtf8(directory / "smoke.bin"));
                    ProcessControl control{nullptr, 15};
                    const auto encode = ProcessManager::execute(pathUtf8(tool), args, directory,
                                                                guard, 0, cancel, nullptr, &control);
                    status = classifyProbe(encode);
                    if (status == CapabilityStatus::Supported) {
                        const auto decode = ProcessManager::execute(
                            pathUtf8(tool), {"t", "-y", "-mmt1", pathUtf8(archive)}, directory, guard, 0,
                            cancel, nullptr, &control);
                        status = classifyProbe(decode);
                        if (decode.cancelled)
                            throw std::runtime_error("Cancelled");
                    }
                    if (encode.cancelled)
                        throw std::runtime_error("Cancelled");
                } catch (...) {
                    publish(key, entry, CapabilityStatus::Inconclusive);
                    throw;
                }
                publish(key, entry, status);
            }
            if (status != CapabilityStatus::Inconclusive) {
                cache[key] = status == CapabilityStatus::Supported;
                // Merge under the lock so unrelated callers cannot overwrite each other's keys.
                std::lock_guard<std::mutex> lock(cacheMutex);
                auto merged = readCache(cachePath);
                merged[key] = cache[key];
                writeAtomically(cachePath, merged.dump(2));
            }
        }
        if (cancelled(cancel))
            throw std::runtime_error("Cancelled");
        if (status != CapabilityStatus::Unsupported)
            result.push_back(task);
        else if (log)
            log("[Capabilities] Unsupported codec configuration excluded: " + task.name + " / " +
                task.configId);
    }
    return result;
}
} // namespace UltraArch

#include "RamCandidate.hpp"
#include "BackendIdentity.hpp"
#include "Diagnostics.hpp"
#include "SettingsJson.hpp"
#include <fstream>
#include <cstring>
namespace UltraArch {
struct RamArtifact::Impl {
    std::shared_ptr<RamBudget> budget;
    uint64_t reserved = 0;
    std::string name;
#ifdef _WIN32
    HANDLE mapping = nullptr;
    void *data = nullptr;
    ~Impl() {
        if (data) UnmapViewOfFile(data);
        if (mapping) CloseHandle(mapping);
        if (budget) budget->release(reserved);
    }
#else
    ~Impl() { if (budget) budget->release(reserved); }
#endif
};
RamArtifact::RamArtifact() : impl_(std::make_unique<Impl>()) {}
RamArtifact::~RamArtifact() = default;
void RamArtifact::write(uint64_t offset, const void *source, uint64_t bytes) {
    if (bytes > capacity || offset > capacity - bytes || (bytes && !source))
        throw std::runtime_error("RAM store write exceeds capacity");
#ifdef _WIN32
    if (bytes) std::memcpy(static_cast<uint8_t *>(impl_->data) + offset, source, static_cast<size_t>(bytes));
#else
    throw std::runtime_error("RAM store requires Windows");
#endif
}
const std::string &RamArtifact::mappingName() const { return impl_->name; }
const uint8_t *RamArtifact::data() const {
#ifdef _WIN32
    return static_cast<const uint8_t *>(impl_->data);
#else
    return nullptr;
#endif
}
std::shared_ptr<RamArtifact> RamArtifact::allocate(const std::shared_ptr<RamBudget> &budget,
    uint64_t reserved, uint64_t capacity) {
    auto artifact = std::make_shared<RamArtifact>();
#ifdef _WIN32
    if (!budget || !budget->acquire(reserved)) return {};
    artifact->impl_->budget = budget;
    artifact->impl_->reserved = reserved;
    artifact->capacity = capacity;
    artifact->impl_->name = "Local\\UltraArch-RAM-" + uniqueId();
    auto name = utf8Path(artifact->impl_->name).wstring();
    artifact->impl_->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        static_cast<DWORD>(capacity >> 32), static_cast<DWORD>(capacity), name.c_str());
    if (!artifact->impl_->mapping || GetLastError() == ERROR_ALREADY_EXISTS) return {};
    artifact->impl_->data = MapViewOfFile(artifact->impl_->mapping, FILE_MAP_READ | FILE_MAP_WRITE,
        0, 0, static_cast<SIZE_T>(capacity));
    if (!artifact->impl_->data) return {};
    return artifact;
#else
    (void)budget; (void)reserved; (void)capacity;
    return {};
#endif
}
void RamArtifact::materialize(const std::filesystem::path &path) const {
#ifdef _WIN32
    auto staged = path.parent_path() / (path.filename().wstring() + L".ram-" + utf8Path(uniqueId()).wstring());
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove(path, ec); } } cleanup{staged};
    std::ofstream output(staged, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("Cannot create durable RAM winner");
    for (uint64_t offset = 0; offset < size;) {
        auto count = std::min<uint64_t>(1048576, size - offset);
        output.write(static_cast<const char *>(impl_->data) + offset, static_cast<std::streamsize>(count));
        if (!output) throw std::runtime_error("Cannot write durable RAM winner (disk full/permission)");
        offset += count;
    }
    output.close();
    if (!output) throw std::runtime_error("Cannot close durable RAM winner");
    HANDLE durable = CreateFileW(staged.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (durable == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot flush durable RAM winner");
    const bool flushed = FlushFileBuffers(durable) != FALSE;
    CloseHandle(durable);
    if (!flushed || !MoveFileExW(staged.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot publish durable RAM winner");
#else
    (void)path;
    throw std::runtime_error("RAM candidates require Windows");
#endif
}
RamCandidateResult RamCandidate::run(const TaskConfig &task, const AnalysisSummary &summary,
    const std::filesystem::path &root, const OptimizerSettings &settings,
    const std::shared_ptr<RamBudget> &budget, ResourceGuard *guard, uint32_t worker,
    const std::atomic<bool> *cancel, const ProcessControl *control) {
    RamCandidateResult result;
    const uint64_t entries = summary.totalFiles + summary.emptyDirectories.size();
    if (!RamPolicy::eligible(settings, summary.totalBytes, entries)) {
        result.fallbackReason = "Group size/file count or temporary RAM budget";
        return result;
    }
#ifndef _WIN32
    (void)task; (void)root; (void)budget; (void)guard; (void)worker; (void)cancel; (void)control;
    result.fallbackReason = "RAM encoder requires Windows";
    return result;
#else
    if (task.groupedSearch || task.reuseInputArchive) {
        result.fallbackReason = "Assembly or original archive reuse";
        return result;
    }
    const auto reservation = RamPolicy::reservation(task, summary.totalBytes, entries);
    MEMORYSTATUSEX memory{sizeof(memory)};
    if (reservation > settings.ramLimitMB ||
        (GlobalMemoryStatusEx(&memory) && (memory.ullAvailPhys / RamPolicy::MiB < reservation + 512 ||
                                          memory.ullAvailPageFile / RamPolicy::MiB < reservation))) {
        result.fallbackReason = "Insufficient available memory for bounded RAM candidate";
        return result;
    }
    wchar_t module[32768]{};
    GetModuleFileNameW(nullptr, module, 32768);
    auto app = std::filesystem::path(module).parent_path();
    std::filesystem::path executable;
    for (const auto &candidate : {app / "encode-worker.exe", app.parent_path() / "encode-worker.exe",
         app.parent_path().parent_path() / app.filename() / "encode-worker.exe",
         settings.toolsBinDir.parent_path() / "encode-worker.exe"})
        if (std::filesystem::is_regular_file(candidate)) { executable = candidate; break; }
    if (executable.empty()) { result.fallbackReason = "RAM encoder unavailable"; return result; }
    auto dll = std::filesystem::absolute(settings.toolsBinDir / "7z.dll");
    if (!std::filesystem::is_regular_file(dll) || fileChecksum(dll, cancel) != pinned7ZipDllSha256) {
        result.fallbackReason = "Pinned RAM encoder DLL unavailable";
        return result;
    }
    auto reserved = RamPolicy::temporaryMB(summary.totalBytes, entries) * RamPolicy::MiB;
    auto artifact = RamArtifact::allocate(budget, reserved,
        RamPolicy::outputCapacity(summary.totalBytes, entries));
    if (!artifact) {
        result.fallbackReason = "Shared temporary RAM budget occupied";
        return result;
    }
    Json request = {{"schema", 1}, {"dll", pathUtf8(dll)}, {"mappingName", artifact->mappingName()},
        {"capacityBytes", artifact->capacity}, {"root", pathUtf8(std::filesystem::absolute(root))},
        {"inputLimitBytes", settings.ramGroupLimitMB * RamPolicy::MiB},
        {"files", Json::array()}, {"directories", Json::array()}, {"properties", Json::array()},
        {"password", settings.password}};
    for (const auto &file : summary.files) {
        Json entry = {{"path", pathUtf8(std::filesystem::absolute(file.path))},
            {"relative", pathUtf8(file.path.lexically_relative(root))}, {"size", file.sizeBytes},
            {"sha256", file.contentChecksum.empty() ? fileChecksum(file.path, cancel) : file.contentChecksum}};
        if (file.memoryContent) {
            const auto &source = *file.memoryContent;
            entry["memory"] = {{"mappingName", source.mappingName}, {"capacityBytes", source.capacity},
                {"offset", source.offset}, {"size", source.size}, {"attributes", source.attributes},
                {"mtime", source.mtime}, {"ctime", source.ctime}, {"atime", source.atime}};
        }
        request["files"].push_back(std::move(entry));
    }
    for (const auto &directory : summary.emptyDirectories)
        request["directories"].push_back(pathUtf8(directory.lexically_relative(root)));
    for (const auto &flag : task.spec.compile()) {
        if (flag.rfind("-m", 0) != 0) throw std::logic_error("Unexpected typed compression flag");
        auto property = flag.substr(2);
        auto equal = property.find('=');
        if (equal == std::string::npos) {
            if (property == "x0") {
                request["properties"].push_back({{"name", "x"}, {"value", "0"}});
                continue;
            }
            // BCJ2 bindings have syntax b0:1, no equals sign.
            if (property.rfind("b", 0) != 0) throw std::logic_error("Invalid encoder property");
            request["properties"].push_back({{"name", property}, {"value", ""}});
        } else request["properties"].push_back({{"name", property.substr(0, equal)}, {"value", property.substr(equal + 1)}});
    }
    request["properties"].push_back({{"name", "he"}, {"value", settings.encryptHeaders ? "on" : "off"}});
    auto text = request.dump();
    if (text.size() > 4 * 1048576) { result.fallbackReason = "RAM manifest exceeds bounded IPC"; return result; }
    ProcessControl workerControl = control ? *control : ProcessControl{};
    workerControl.stdinData = "UAR1";
    for (int i = 0; i < 4; ++i) workerControl.stdinData.push_back(static_cast<char>(uint32_t(text.size()) >> (8 * i)));
    workerControl.stdinData += text;
    result.process = ProcessManager::execute(pathUtf8(executable), {}, root, guard, worker, cancel, nullptr, &workerControl);
    if (result.process.cancelled || result.process.timedOut || result.process.killedByOOM)
        return result;
    try {
        const auto &frame = result.process.stdOutput;
        if (frame.size() < 8 || frame.substr(0, 4) != "UAR1") throw std::runtime_error("Invalid RAM encoder reply");
        uint32_t length = 0;
        for (int i = 0; i < 4; ++i) length |= uint32_t(static_cast<unsigned char>(frame[4+i])) << (8*i);
        if (length != frame.size() - 8 || length > 65536) throw std::runtime_error("Invalid RAM reply length");
        const auto reply = Json::parse(frame.substr(8));
        if (!result.process.success || !reply.value("success", false)) {
            if (reply.value("fallback", false)) result.fallbackReason = reply.value("error", std::string("RAM unsupported"));
            else throw std::runtime_error(reply.value("error", std::string("RAM encoder failed")));
            return result;
        }
        artifact->size = reply.at("bytes").get<uint64_t>();
        if (reply.value("schema", 0) != 1 || !reply.value("verified", false) ||
            reply.value("files", uint64_t(0)) != summary.totalFiles ||
            reply.value("rawBytes", uint64_t(0)) != summary.totalBytes ||
            artifact->size == 0 || artifact->size > artifact->capacity)
            throw std::runtime_error("RAM encoder verification contract mismatch");
        result.artifact = std::move(artifact);
    } catch (const std::exception &e) {
        result.process.success = false;
        result.process.errOutput = Diagnostics::redact(e.what(), settings.password);
    }
    return result;
#endif
}
} // namespace UltraArch

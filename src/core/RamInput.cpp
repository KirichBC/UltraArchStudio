#include "RamInput.hpp"
#include "BackendIdentity.hpp"
#include "SettingsJson.hpp"
#include "WorkspaceManager.hpp"
#include <fstream>
namespace UltraArch {
bool RamInput::materialize(const FileTypeInfo &file) {
    if (!file.memoryContent) return false;
    const auto &memory = *file.memoryContent;
    if (!memory.owner || !memory.data || memory.size > memory.capacity ||
        memory.offset > memory.capacity - memory.size)
        throw std::runtime_error("Invalid RAM input materialization source");
    for (auto parent = file.path; !parent.empty(); parent = parent.parent_path()) {
        if (isLinkOrReparse(parent)) throw std::runtime_error("Reparse point in RAM input materialization");
        if (parent == parent.parent_path()) break;
    }
    if (std::filesystem::is_regular_file(file.path)) {
        if (fileChecksum(file.path) != file.contentChecksum)
            throw std::runtime_error("Materialized RAM input was changed");
        return false;
    }
    std::filesystem::create_directories(file.path.parent_path());
    if (isLinkOrReparse(file.path) || isLinkOrReparse(file.path.parent_path()))
        throw std::runtime_error("Reparse point in RAM input materialization");
    const auto staged = file.path.parent_path() / ("ram-spill-" + uniqueId() + ".tmp");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
#ifdef _WIN32
            SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
#endif
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    } cleanup{staged};
    std::ofstream out(staged, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("Cannot materialize RAM input for disk fallback");
    for (uint64_t offset = 0; offset < memory.size;) {
        auto count = std::min<uint64_t>(1048576, memory.size - offset);
        out.write(reinterpret_cast<const char *>(memory.data + offset), static_cast<std::streamsize>(count));
        if (!out) throw std::runtime_error("Cannot write RAM input for disk fallback");
        offset += count;
    }
    out.close();
    if (!out) throw std::runtime_error("Cannot finish RAM input materialization");
#ifdef _WIN32
    auto handle = CreateFileW(staged.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot set RAM input metadata");
    FILETIME mt{static_cast<DWORD>(memory.mtime), static_cast<DWORD>(memory.mtime >> 32)};
    FILETIME ct{static_cast<DWORD>(memory.ctime), static_cast<DWORD>(memory.ctime >> 32)};
    FILETIME at{static_cast<DWORD>(memory.atime), static_cast<DWORD>(memory.atime >> 32)};
    bool ok = SetFileTime(handle, memory.ctime ? &ct : nullptr, memory.atime ? &at : nullptr,
                           memory.mtime ? &mt : nullptr) != 0;
    CloseHandle(handle);
    if (!ok) throw std::runtime_error("Cannot restore RAM input timestamps");
    const auto attributes = memory.attributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |
        FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED);
    if (!SetFileAttributesW(staged.c_str(), attributes ? attributes : FILE_ATTRIBUTE_NORMAL))
        throw std::runtime_error("Cannot restore RAM input attributes");
    // No replacement: a competing or changed input must never be silently overwritten.
    if (!MoveFileExW(staged.c_str(), file.path.c_str(), MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot publish materialized RAM input");
#else
    std::filesystem::rename(staged, file.path);
#endif
    return true;
}
RamInputResult RamInput::extract(const std::filesystem::path &archive, const std::filesystem::path &root,
    const OptimizerSettings &settings, const std::shared_ptr<RamBudget> &budget,
    ResourceGuard *guard, const std::atomic<bool> *cancel) {
    RamInputResult result;
    if (settings.ramLimitMB < 256) {
        result.fallbackReason = "Insufficient RAM headroom for input cache";
        return result;
    }
#ifndef _WIN32
    (void)archive; (void)root; (void)settings; (void)budget; (void)guard; (void)cancel;
    result.fallbackReason = "RAM input requires Windows";
    return result;
#else
    wchar_t module[32768]{};
    GetModuleFileNameW(nullptr, module, 32768);
    auto app = std::filesystem::path(module).parent_path();
    std::filesystem::path worker;
    for (const auto &candidate : {app / "extract-worker.exe", app.parent_path() / "extract-worker.exe",
         app.parent_path().parent_path() / app.filename() / "extract-worker.exe",
         settings.toolsBinDir.parent_path() / "extract-worker.exe"})
        if (std::filesystem::is_regular_file(candidate)) { worker = candidate; break; }
    if (worker.empty()) { result.fallbackReason = "RAM input worker unavailable"; return result; }
    auto dll = std::filesystem::absolute(settings.toolsBinDir / "7z.dll");
    if (!std::filesystem::is_regular_file(dll) || fileChecksum(dll, cancel) != pinned7ZipDllSha256) {
        result.fallbackReason = "Pinned RAM input DLL unavailable"; return result;
    }
    const auto workerFloor = uint64_t(settings.workerThreads) * 32;
    const auto spareMB = settings.ramLimitMB > workerFloor ? settings.ramLimitMB - workerFloor : 0;
    auto capacity = std::min<uint64_t>({settings.ramTemporaryBudgetMB / 2, settings.ramLimitMB / 4, spareMB, 512}) * RamPolicy::MiB;
    if (capacity < RamPolicy::MiB) { result.fallbackReason = "RAM input budget insufficient"; return result; }
    MEMORYSTATUSEX available{sizeof(available)};
    if (GlobalMemoryStatusEx(&available) &&
        (available.ullAvailPhys < capacity + 512 * RamPolicy::MiB ||
         available.ullAvailPageFile < capacity + 128 * RamPolicy::MiB)) {
        result.fallbackReason = "Insufficient system memory headroom for RAM input";
        return result;
    }
    auto storage = RamArtifact::allocate(budget, capacity, capacity);
    if (!storage) { result.fallbackReason = "RAM input mapping budget occupied"; return result; }
    std::filesystem::create_directories(root);
    auto manifestPath = root.parent_path() / ("ram-input-" + uniqueId() + ".json");
    Json request = {{"schema",1}, {"dll",pathUtf8(dll)}, {"archive",pathUtf8(std::filesystem::absolute(archive))},
        {"root",pathUtf8(std::filesystem::absolute(root))}, {"manifestPath",pathUtf8(manifestPath)},
        {"mappingName",storage->mappingName()}, {"capacityBytes",capacity},
        {"groupLimitBytes",settings.ramGroupLimitMB * RamPolicy::MiB},
        {"totalLimitBytes",settings.extractionLimitMB * RamPolicy::MiB},
        {"fileLimitBytes",settings.extractionFileLimitMB * RamPolicy::MiB},
        {"fileCountLimit",settings.extractionFileCount}, {"password",settings.password}};
    auto text = request.dump();
    if (text.size() > 65536) throw std::runtime_error("RAM input request exceeds IPC bound");
    ProcessControl control{nullptr, settings.processTimeoutSeconds};
    control.stdinData = "UAI1";
    for (int i=0; i<4; ++i) control.stdinData.push_back(static_cast<char>(uint32_t(text.size()) >> (8*i)));
    control.stdinData += text;
    ResourceGuard decoderGuard;
    const auto totalMB = guard && guard->getMemoryLimitMB()
        ? std::min(settings.ramLimitMB, guard->getMemoryLimitMB()) : settings.ramLimitMB;
    const auto reservedMB = capacity / RamPolicy::MiB;
    if (totalMB <= reservedMB + 128 || !decoderGuard.initialize(totalMB - reservedMB - 128))
        throw std::runtime_error("Cannot enforce RAM input decoder memory budget");
    decoderGuard.setScheduling(static_cast<int>(settings.affinityMode), settings.backgroundPriority);
    if (settings.cpuLimitPercent && !decoderGuard.setCpuRate(settings.cpuLimitPercent))
        throw std::runtime_error("Cannot enforce RAM input decoder CPU cap");
    result.process = ProcessManager::execute(pathUtf8(worker), {}, root, &decoderGuard, 0, cancel, nullptr, &control);
    result.cpuSeconds = decoderGuard.getCpuSeconds();
    if (result.process.cancelled || result.process.timedOut || result.process.killedByOOM) return result;
    try {
        const auto &frame = result.process.stdOutput;
        if (frame.size() < 8 || frame.substr(0,4) != "UAI1") throw std::runtime_error("Invalid RAM input reply");
        uint32_t length=0;
        for (int i=0;i<4;++i) length |= uint32_t(static_cast<unsigned char>(frame[4+i])) << (8*i);
        if (length != frame.size()-8 || length>65536) throw std::runtime_error("Invalid RAM input reply length");
        auto reply=Json::parse(frame.substr(8));
        if (!result.process.success || !reply.value("success",false)) {
            if (reply.value("fallback",false)) result.fallbackReason=reply.value("error",std::string("RAM input unsupported"));
            else throw std::runtime_error(reply.value("error",std::string("RAM input extraction failed")));
            return result;
        }
        if (std::filesystem::file_size(manifestPath)>64*RamPolicy::MiB ||
            fileChecksum(manifestPath,cancel)!=reply.at("manifestSha256").get<std::string>())
            throw std::runtime_error("RAM input manifest identity mismatch");
        Json manifest; std::ifstream stream(manifestPath); stream>>manifest;
        if (manifest.value("schema",0)!=1) throw std::runtime_error("RAM input manifest schema mismatch");
        std::vector<FileTypeInfo> files;
        std::vector<std::filesystem::path> dirs;
        std::set<std::string> nonempty;
        uint64_t total=0, count=0, ramBytes=0, diskBytes=0;
        bool hasRamEntries = false;
        for (const auto &entry:manifest.at("files")) {
            auto relative=entry.at("relative").get<std::string>();
            WorkspaceManager::validateArchivePath(relative);
            auto path=archiveEntryPath(root,relative);
            auto size=entry.at("size").get<uint64_t>();
            if (++count>settings.extractionFileCount || size>settings.extractionFileLimitMB*RamPolicy::MiB ||
                size>settings.extractionLimitMB*RamPolicy::MiB-total)
                throw std::runtime_error("RAM input manifest exceeds quota");
            total+=size;
            FileTypeInfo info;
            if (entry.at("storage")=="ram") {
                hasRamEntries = true;
                auto offset=entry.at("offset").get<uint64_t>();
                if (size>capacity || offset>capacity-size) throw std::runtime_error("RAM input outside mapping");
                auto source=std::make_shared<MemoryContent>();
                source->owner=storage; source->data=storage->data()+offset;
                source->mappingName=storage->mappingName(); source->offset=offset;
                source->size=size; source->capacity=capacity;
                source->attributes=entry.value("attributes",uint32_t(32));
                source->mtime=entry.value("mtime",uint64_t(0));
                source->ctime=entry.value("ctime",uint64_t(0));
                source->atime=entry.value("atime",uint64_t(0));
                info=ContentAnalyzer::inspectMemory(path,source,cancel,settings.enableSimilarityGroups);
                ramBytes+=size;
            } else if (entry.at("storage")=="disk") {
                info=ContentAnalyzer::inspectFile(path,true,cancel,settings.enableSimilarityGroups);
                diskBytes+=size;
            } else throw std::runtime_error("Invalid RAM input storage kind");
            if (info.sizeBytes!=size || info.contentChecksum!=entry.at("sha256").get<std::string>())
                throw std::runtime_error("RAM input content identity mismatch");
            files.push_back(std::move(info));
            for (auto parent=path.parent_path();parent!=root && !parent.empty();parent=parent.parent_path())
                nonempty.insert(archivePathKey(pathUtf8(parent.lexically_relative(root))));
        }
        for (const auto &entry:manifest.at("directories")) {
            auto relative=entry.get<std::string>(); WorkspaceManager::validateArchivePath(relative);
            if (++count>settings.extractionFileCount) throw std::runtime_error("RAM input directory quota exceeded");
            auto path=archiveEntryPath(root,relative);
            if (!nonempty.count(archivePathKey(relative))) dirs.push_back(path);
            for (auto parent=path.parent_path();parent!=root && !parent.empty();parent=parent.parent_path())
                nonempty.insert(archivePathKey(pathUtf8(parent.lexically_relative(root))));
        }
        dirs.erase(std::remove_if(dirs.begin(),dirs.end(),[&](const auto &p){return nonempty.count(archivePathKey(pathUtf8(p.lexically_relative(root))));}),dirs.end());
        result.manifest=InputManifest::fromEntries(root,std::move(files),std::move(dirs));
        result.ramBytes=ramBytes; result.diskBytes=diskBytes;
        result.reservedMB=hasRamEntries ? capacity/RamPolicy::MiB : 0;
        result.success=true;
        std::error_code ec; std::filesystem::remove(manifestPath,ec);
    } catch (const std::exception &e) { result.process.success=false; result.process.errOutput=e.what(); }
    return result;
#endif
}
} // namespace UltraArch

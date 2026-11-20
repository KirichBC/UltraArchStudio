#include "core/ArchiveExtractor.hpp"
#include "core/InputManifest.hpp"
#include "core/RamCandidate.hpp"
#include "core/RamInput.hpp"
#include "core/TaskMatrixGenerator.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
using namespace UltraArch;
namespace fs = std::filesystem;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)
struct Temp {
    fs::path path = fs::temp_directory_path() / utf8Path("UltraArch-RAM-日本語-" + uniqueId());
    Temp() { fs::create_directories(path); }
    ~Temp() { std::error_code ec; fs::remove_all(path, ec); }
};
static void write(const fs::path &path, const std::string &bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary); out.write(bytes.data(), bytes.size()); CHECK(out.good());
}
static std::string randomBytes(size_t size) {
    std::mt19937 rng(42); std::string bytes(size, '\0');
    for (auto &c : bytes) c = static_cast<char>(rng());
    return bytes;
}
static fs::path findTools(const char *argv0) {
    if (const auto *env = std::getenv("ULTRAARCH_TOOLS_DIR")) {
        auto found = ProcessManager::resolveExecutable("7z", utf8Path(env), true);
        if (!found.empty()) return found;
    }
    auto parent = fs::absolute(utf8Path(argv0)).parent_path();
    for (unsigned n = 0; n < 8; ++n) {
        auto found = ProcessManager::resolveExecutable("7z", parent / "bin", true);
        if (!found.empty()) return found;
        parent = parent.parent_path();
    }
    return ProcessManager::resolveExecutable("7z", "bin");
}
static TaskConfig pick(const std::vector<TaskConfig> &tasks, const std::string &family) {
    const TaskConfig *best = nullptr;
    for (const auto &task : tasks)
        if (!task.groupedSearch && task.spec.family() == family &&
            (!best || task.estimatedRamMB < best->estimatedRamMB)) best = &task;
    if (!best) throw std::runtime_error("Matrix missing family " + family);
    return *best;
}
static void independent(const fs::path &tool, const fs::path &archive, const fs::path &dest,
                        const InputManifest &original, const std::string &password = {}) {
    ProcessControl control{nullptr, 30};
    std::vector<std::string> args{"t", "-y", pathUtf8(archive)};
    if (!password.empty()) args.push_back("-p" + password); // Synthetic test-only password.
    auto tested = ProcessManager::execute(pathUtf8(tool), args,
                                         archive.parent_path(), nullptr, 0, nullptr, nullptr, &control);
    CHECK(tested.success);
    CHECK(tested.stdOutput.find(password) == std::string::npos || password.empty());
    CHECK(tested.errOutput.find(password) == std::string::npos || password.empty());
    CHECK(ArchiveExtractor::extract(archive, dest, tool.parent_path(), nullptr, nullptr, password, nullptr, 30));
    const auto restored = InputManifest::scan(dest);
    CHECK(restored.summary.totalFiles == original.summary.totalFiles);
    CHECK(restored.summary.totalBytes == original.summary.totalBytes);
    for (const auto &file : original.summary.files) {
        auto path = dest / file.path.lexically_relative(original.root);
        CHECK(fs::is_regular_file(path)); CHECK(fileChecksum(path) == file.contentChecksum);
    }
    for (const auto &dir : original.summary.emptyDirectories)
        CHECK(fs::is_directory(dest / dir.lexically_relative(original.root)));
}
static void contracts(const fs::path &tool) {
    Temp temp;
    {
        auto budget = std::make_shared<RamBudget>(16);
        auto store = RamArtifact::allocate(budget, 16, 16);
        CHECK(store && budget->used() == 16);
        store->write(15, "x", 1); CHECK(store->data()[15] == 'x');
        bool rejected = false;
        try { store->write(UINT64_MAX, "x", 1); } catch (...) { rejected = true; }
        CHECK(rejected); rejected = false;
        try { store->write(16, "x", 1); } catch (...) { rejected = true; }
        CHECK(rejected); store.reset(); CHECK(budget->used() == 0);
    }
    const auto root = temp.path / "input";
    write(root / utf8Path("данные-日本語.txt"), std::string(120000, 'a') + randomBytes(32768));
    write(root / "second.bin", randomBytes(65536));
    write(root / "empty.txt", ""); fs::create_directories(root / "empty-directory");
    auto longDirectory = root;
    for (unsigned i = 0; i < 9; ++i)
        longDirectory /= utf8Path("длинный-日本語-путь-" + std::to_string(i) + "-abcdefghijklmnop");
    write(longDirectory / utf8Path("данные.txt"), "Long Unicode path remains identical through RAM and disk.");
    CHECK(pathUtf8(longDirectory).size() > 260);
#ifdef _WIN32
    CHECK(longDirectory.native().size() > 260);
    wchar_t windows[32768]{}; CHECK(GetWindowsDirectoryW(windows, 32768));
    fs::copy_file(fs::path(windows) / "System32" / "cmd.exe", root / "native.exe");
    const auto x86 = fs::path(windows) / "SysWOW64" / "cmd.exe";
    CHECK(fs::is_regular_file(x86));
    fs::copy_file(x86, root / "native-x86.exe");
#endif
    const auto manifest = InputManifest::scan(root);
    OptimizerSettings s; s.profile = OptimizationProfile::Extreme_RePack;
    s.enableRamCandidates = true; s.ramGroupLimitMB = 40; s.ramTemporaryBudgetMB = 256;
    s.ramLimitMB = 4096; s.cpuThreads = 1; s.workerThreads = 1;
    s.processTimeoutSeconds = 30; s.toolsBinDir = tool.parent_path(); s.solidBlockSize = "on";
    auto matrix = TaskMatrixGenerator{}.generateMatrix(manifest.summary, s);
    auto budget = std::make_shared<RamBudget>(256 * RamPolicy::MiB);
    ResourceGuard guard; CHECK(guard.initialize(s.ramLimitMB));
    ProcessControl control{nullptr, 30};
    std::string list;
    for (const auto &file : manifest.summary.files) list += pathUtf8(file.path.lexically_relative(root)) + "\n";
    for (const auto &dir : manifest.summary.emptyDirectories) list += pathUtf8(dir.lexically_relative(root)) + "\n";
    write(temp.path / "files.txt", list);
    unsigned number = 0;
    for (const auto *family : {"Store", "LZMA1", "LZMA2", "PPMd", "BZip2", "Deflate", "Delta", "BCJ", "BCJ2"}) {
        const auto task = pick(matrix, family);
        auto result = RamCandidate::run(task, manifest.summary, root, s, budget, &guard, 0, nullptr, &control);
        if (!result.artifact) throw std::runtime_error(std::string(family) + ": " +
            result.fallbackReason + " / " + result.process.errOutput);
        CHECK(result.process.success && result.artifact->size <= result.artifact->capacity);
        CHECK(budget->used() > 0);
        const auto archive = temp.path / ("ram-" + std::to_string(number) + ".7z");
        result.artifact->materialize(archive);
        CHECK(fs::file_size(archive) == result.artifact->size);
        independent(tool, archive, temp.path / ("restore-" + std::to_string(number)), manifest);
        auto disk = temp.path / ("disk-" + std::to_string(number) + ".7z");
        std::vector<std::string> args{"a", "-y", "-t7z", "-scsUTF-8", "-sccUTF-8", "-spd"};
        auto flags = task.spec.compile(); args.insert(args.end(), flags.begin(), flags.end());
        args.push_back(pathUtf8(disk)); args.push_back("@" + pathUtf8(temp.path / "files.txt"));
        auto cli = ProcessManager::execute(pathUtf8(tool), args, root, &guard, 0, nullptr, nullptr, &control);
        CHECK(cli.success);
        if (fileChecksum(disk) != fileChecksum(archive)) {
            std::cerr << family << " RAM=" << fs::file_size(archive) << " Disk=" << fs::file_size(disk)
                      << " SHA mismatch (same input metadata/spec)\n";
            throw std::runtime_error("DLL/CLI archive parity failed");
        }
        result.artifact.reset(); CHECK(budget->used() == 0);
        std::cout << "[PASS] RAM/Disk " << family << '\n'; ++number;
    }
    auto task = pick(matrix, "LZMA1");
    auto fallback = [&](OptimizerSettings settings, std::shared_ptr<RamBudget> shared) {
        auto r = RamCandidate::run(task, manifest.summary, root, settings, shared, &guard, 0, nullptr, &control);
        CHECK(!r.artifact && !r.fallbackReason.empty()); CHECK(shared->used() == 0);
    };
    auto disabled = s; disabled.enableRamCandidates = false; fallback(disabled, budget);
    auto threshold = s; threshold.ramGroupLimitMB = 0; fallback(threshold, budget);
    fallback(s, std::make_shared<RamBudget>(1));
    auto directoryHeavy = manifest.summary;
    directoryHeavy.emptyDirectories.assign(65537, root / "synthetic-empty-directory");
    auto tooManyEntries = RamCandidate::run(task, directoryHeavy, root, s, budget, &guard, 0, nullptr, &control);
    CHECK(!tooManyEntries.artifact && !tooManyEntries.fallbackReason.empty());
    CHECK(tooManyEntries.process.durationSeconds == 0 && budget->used() == 0);
    std::atomic<bool> cancelled{true}; ProcessControl cancelControl{&cancelled, 30};
    auto aborted = RamCandidate::run(task, manifest.summary, root, s, budget, &guard, 0, nullptr, &cancelControl);
    CHECK(!aborted.artifact && aborted.process.cancelled && aborted.fallbackReason.empty());
    CHECK(budget->used() == 0);
    // A stale SHA must be a hard failure, never a silent retry with altered bytes.
    auto changed = manifest.summary; changed.files.front().contentChecksum = std::string(64, '0');
    auto invalid = RamCandidate::run(task, changed, root, s, budget, &guard, 0, nullptr, &control);
    CHECK(!invalid.artifact && !invalid.process.success && invalid.fallbackReason.empty());
    CHECK(budget->used() == 0);
    {
        // AES uses random salt/IV, including properties inside compressed headers.
        // Test full independent restoration; archive bytes/length need not match.
        const std::string password = "RAM-contract-only-日本語-42";
        auto encrypted = s; encrypted.password = password; encrypted.encryptHeaders = true;
        auto encoded = RamCandidate::run(task, manifest.summary, root, encrypted, budget,
                                         &guard, 0, nullptr, &control);
        CHECK(encoded.artifact && encoded.process.success);
        CHECK(encoded.process.stdOutput.find(password) == std::string::npos);
        CHECK(encoded.process.errOutput.find(password) == std::string::npos);
        const auto archive = temp.path / "encrypted-ram.7z";
        encoded.artifact->materialize(archive);
        independent(tool, archive, temp.path / "restore-encrypted", manifest, password);
        const auto disk = temp.path / "encrypted-disk.7z";
        std::vector<std::string> args{"a", "-y", "-t7z", "-scsUTF-8", "-sccUTF-8", "-spd"};
        auto flags = task.spec.compile(); args.insert(args.end(), flags.begin(), flags.end());
        args.push_back("-p" + password); args.push_back("-mhe=on");
        args.push_back(pathUtf8(disk)); args.push_back("@" + pathUtf8(temp.path / "files.txt"));
        auto cli = ProcessManager::execute(pathUtf8(tool), args, root, &guard, 0, nullptr, nullptr, &control);
        CHECK(cli.success && fs::file_size(disk) > 0);
        CHECK(cli.stdOutput.find(password) == std::string::npos);
        CHECK(cli.errOutput.find(password) == std::string::npos);
        independent(tool, disk, temp.path / "restore-encrypted-disk", manifest, password);
        encoded.artifact.reset(); CHECK(budget->used() == 0);
        auto decoded = RamInput::extract(archive, temp.path / "hybrid-encrypted", encrypted, budget, &guard, nullptr);
        CHECK(decoded.success && decoded.manifest.summary.totalFiles == manifest.summary.totalFiles);
        CHECK(decoded.process.stdOutput.find(password) == std::string::npos);
        CHECK(decoded.process.errOutput.find(password) == std::string::npos);
        const std::string wrongPassword = "RAM-contract-only-wrong-42";
        auto wrong = encrypted; wrong.password = wrongPassword;
        auto rejected = RamInput::extract(archive, temp.path / "hybrid-wrong-password", wrong, budget, &guard, nullptr);
        CHECK(!rejected.success && !rejected.process.success && rejected.fallbackReason.empty());
        CHECK(rejected.process.stdOutput.find(password) == std::string::npos);
        CHECK(rejected.process.errOutput.find(password) == std::string::npos);
        CHECK(rejected.process.stdOutput.find(wrongPassword) == std::string::npos);
        CHECK(rejected.process.errOutput.find(wrongPassword) == std::string::npos);
        for (const auto &entry : fs::directory_iterator(temp.path / "hybrid-wrong-password")) {
            (void)entry;
            throw std::runtime_error("Incorrect header password caused output writes");
        }
        auto malformed = encrypted;
        malformed.password = std::string("bad\0password", 12);
        auto malformedResult = RamCandidate::run(task, manifest.summary, root, malformed, budget,
                                                 &guard, 0, nullptr, &control);
        CHECK(!malformedResult.artifact && !malformedResult.process.success &&
              malformedResult.fallbackReason.empty());
        CHECK(malformedResult.process.stdOutput.find(malformed.password) == std::string::npos);
        CHECK(malformedResult.process.errOutput.find(malformed.password) == std::string::npos);
        std::cout << "[PASS] RAM encrypted headers, independent restore, wrong-password hard failure/no secret output\n";
    }
    CHECK(budget->used() == 0);
#ifdef _WIN32
    // A memory source remains usable after its physical file is removed. Other manifest
    // files stay on disk, proving explicit mixed-source handling rather than path fallback.
    auto memorySummary = manifest.summary;
    auto memoryPosition = std::find_if(memorySummary.files.begin(), memorySummary.files.end(),
        [](const auto &file) { return file.path.filename() == utf8Path("данные-日本語.txt"); });
    CHECK(memoryPosition != memorySummary.files.end());
    auto &memoryFile = *memoryPosition;
    const auto physicalPath = memoryFile.path;
    BY_HANDLE_FILE_INFORMATION info{};
    HANDLE sourceFile = CreateFileW(physicalPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(sourceFile != INVALID_HANDLE_VALUE);
    const bool haveInfo = GetFileInformationByHandle(sourceFile, &info) != FALSE;
    CloseHandle(sourceFile); CHECK(haveInfo);
    std::ifstream sourceStream(physicalPath, std::ios::binary);
    std::string contents((std::istreambuf_iterator<char>(sourceStream)), std::istreambuf_iterator<char>());
    CHECK(contents.size() == memoryFile.sizeBytes); sourceStream.close();
    auto sourceBudget = std::make_shared<RamBudget>(RamPolicy::MiB * 4);
    const uint64_t inputOffset = 13, inputCapacity = contents.size() + 32;
    auto input = RamArtifact::allocate(sourceBudget, inputCapacity, inputCapacity);
    CHECK(input && input->data());
    if (!contents.empty()) std::memcpy(const_cast<uint8_t *>(input->data()) + inputOffset,
                                       contents.data(), contents.size());
    auto memory = std::make_shared<MemoryContent>();
    memory->owner = input; memory->mappingName = input->mappingName();
    memory->data = input->data() + inputOffset; memory->offset = inputOffset;
    memory->size = contents.size(); memory->capacity = inputCapacity;
    auto ticks = [](FILETIME time) { return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
    memory->attributes = info.dwFileAttributes;
    memory->mtime = ticks(info.ftLastWriteTime); memory->ctime = ticks(info.ftCreationTime);
    memory->atime = ticks(info.ftLastAccessTime); memoryFile.memoryContent = memory;
    CHECK(fs::remove(physicalPath)); CHECK(!fs::exists(physicalPath));
    auto memoryResult = RamCandidate::run(task, memorySummary, root, s, budget, &guard, 0, nullptr, &control);
    if (!memoryResult.artifact) throw std::runtime_error("RAM input: " + memoryResult.fallbackReason +
                                                        " / " + memoryResult.process.errOutput);
    auto memoryArchive = temp.path / "memory-input.7z";
    memoryResult.artifact->materialize(memoryArchive);
    independent(tool, memoryArchive, temp.path / "restore-memory-input", manifest);
    memoryResult.artifact.reset(); CHECK(budget->used() == 0);
    memory->mappingName = "Local\\Not-UltraArch-Input";
    auto unknownSource = RamCandidate::run(task, memorySummary, root, s, budget, &guard, 0, nullptr, &control);
    CHECK(!unknownSource.artifact && !unknownSource.process.success && unknownSource.fallbackReason.empty());
    CHECK(budget->used() == 0);
    memory->mappingName = input->mappingName(); memory->offset = inputCapacity;
    memory->size = std::max<uint64_t>(1, contents.size());
    auto invalidRange = RamCandidate::run(task, memorySummary, root, s, budget, &guard, 0, nullptr, &control);
    CHECK(!invalidRange.artifact && !invalidRange.process.success && invalidRange.fallbackReason.empty());
    CHECK(budget->used() == 0);
    std::cout << "[PASS] Explicit RAM input, missing physical file, mapping/range rejection\n";
#endif
    const auto largeRoot = temp.path / "timeout-input";
    write(largeRoot / "noise.bin", randomBytes(16 * 1048576));
    const auto large = InputManifest::scan(largeRoot);
    ProcessControl timeout{nullptr, 1};
    auto timed = RamCandidate::run(task, large.summary, largeRoot, s, budget, &guard, 0, nullptr, &timeout);
    CHECK(!timed.artifact && timed.process.timedOut && timed.fallbackReason.empty());
    CHECK(budget->used() == 0);
}
int main(int argc, char **argv) {
    (void)argc;
#ifndef _WIN32
    (void)argv; std::cout << "RAM integration requires Windows\n"; return 77;
#else
    try {
        const auto tool = findTools(argv[0]);
        if (tool.empty()) {
#ifdef ULTRAARCH_REQUIRE_TOOLS
            throw std::runtime_error("Required real 7-Zip unavailable");
#else
            return 77;
#endif
        }
        contracts(tool); std::cout << "RAM native integration passed\n"; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
#endif
}

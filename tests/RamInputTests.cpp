#include "core/RamInput.hpp"
#include "core/SettingsJson.hpp"
#include "core/WorkspaceManager.hpp"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
using namespace UltraArch;
namespace fs = std::filesystem;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)
struct Temp {
    fs::path path = fs::temp_directory_path() / utf8Path("ram-input-日本語-" + uniqueId());
    Temp() {
#ifdef _WIN32
        // Explicit extended-length paths also work when the machine policy is disabled.
        auto native = fs::absolute(path).native();
        if (native.rfind(L"\\\\?\\", 0) != 0)
            path = fs::path(native.rfind(L"\\\\", 0) == 0 ? L"\\\\?\\UNC\\" + native.substr(2)
                                                         : L"\\\\?\\" + native);
#endif
        fs::create_directories(path);
    }
    ~Temp() {
        std::error_code ec;
#ifdef _WIN32
        for (fs::recursive_directory_iterator it(path, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec))
            SetFileAttributesW(it->path().c_str(), FILE_ATTRIBUTE_NORMAL);
#endif
        ec.clear(); fs::remove_all(path, ec);
    }
};
static void write(const fs::path &path, const std::string &data) {
    fs::create_directories(path.parent_path()); std::ofstream out(path, std::ios::binary);
    out.write(data.data(), data.size());
    if (!out.good()) throw std::runtime_error("Cannot write fixture: " + pathUtf8(path));
}
static fs::path tools(const char *exe) {
    if (const char *env = std::getenv("ULTRAARCH_TOOLS_DIR")) {
        auto tool = ProcessManager::resolveExecutable("7z", utf8Path(env), true);
        if (!tool.empty()) return tool;
    }
    auto parent = fs::absolute(utf8Path(exe)).parent_path();
    for (unsigned i = 0; i < 8; ++i) {
        auto tool = ProcessManager::resolveExecutable("7z", parent / "bin", true);
        if (!tool.empty()) return tool; parent = parent.parent_path();
    }
    return ProcessManager::resolveExecutable("7z", "bin");
}
static void contracts(const fs::path &tool) {
    CHECK(archivePathKey("").empty());
    bool emptyNameRejected = false;
    try { WorkspaceManager::validateArchivePath(""); }
    catch (const std::exception &) { emptyNameRejected = true; }
    CHECK(emptyNameRejected);
    Temp temp; const auto source = temp.path / "source";
    write(source / utf8Path("папка-日本語") / "one.txt", std::string(40000, 'a'));
    write(source / "two.txt", std::string(45000, 'b'));
    write(source / "zero.txt", ""); fs::create_directories(source / "empty" / "leaf");
    write(source / "LICENSE", "Permission to use this test fixture.\n");
    write(source / "README", "Extensionless input must remain a valid RAM group.\n");
    write(source / ".gitignore", "temporary/\n");
    write(source / utf8Path("данные-日本語"), "Unicode without an extension.\n");
    write(source / "EMPTYFILE", "");
    auto longFile = source;
    for (unsigned i = 0; i < 6; ++i)
        longFile /= std::string(40, static_cast<char>('a' + i));
    longFile /= "README";
    CHECK(pathUtf8(longFile).size() > 260);
    write(longFile, "Long extensionless input.\n");
    fs::create_directories(longFile.parent_path() / "empty" / "leaf");
    std::mt19937 rng(42); std::string noise(1100000, '\0');
    for (auto &c : noise) c = static_cast<char>(rng());
    // The large file spills to disk, exercising the native long-path sink as
    // well as RAM materialization and directory metadata on the same subtree.
    write(longFile.parent_path() / "noise.bin", noise);
#ifdef _WIN32
    CHECK(SetFileAttributesW((source / utf8Path("папка-日本語") / "one.txt").c_str(),
                            FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE));
    CHECK(SetFileAttributesW((source / "LICENSE").c_str(),
                            FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE));
#endif
    const auto original = InputManifest::scan(source);
    const auto archive = temp.path / "input.7z";
    auto compressed = ProcessManager::execute(pathUtf8(tool),
        {"a", "-y", "-t7z", "-mx=1", pathUtf8(archive), "."}, source);
    CHECK(compressed.success);
    const auto diskRoot = temp.path / "disk";
    CHECK(ProcessManager::execute(pathUtf8(tool),
        {"x", "-y", "-o" + pathUtf8(diskRoot), pathUtf8(archive)}, temp.path).success);
    CHECK(InputManifest::scan(diskRoot).fingerprint == original.fingerprint);
#ifdef _WIN32
    constexpr DWORD fixtureAttributes = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE;
    CHECK((GetFileAttributesW((diskRoot / "LICENSE").c_str()) & fixtureAttributes) == fixtureAttributes);
#endif
    OptimizerSettings s; s.enableRamCandidates = true; s.ramGroupLimitMB = 1;
    s.ramTemporaryBudgetMB = 128; s.ramLimitMB = 4096; s.processTimeoutSeconds = 20;
    s.toolsBinDir = tool.parent_path();
    auto budget = std::make_shared<RamBudget>(128 * RamPolicy::MiB);
    ResourceGuard guard; CHECK(guard.initialize(s.ramLimitMB));
    auto result = RamInput::extract(archive, temp.path / "hybrid", s, budget, &guard, nullptr);
    if (!result.success) throw std::runtime_error(result.fallbackReason + ": " + result.process.errOutput);
    CHECK(result.ramBytes > 0 && result.diskBytes == noise.size());
    CHECK(result.manifest.fingerprint == original.fingerprint);
#ifdef _WIN32
    for (const auto &file : result.manifest.summary.files)
        CHECK(file.path.native().find(L'/') == std::wstring::npos);
    CHECK(archiveEntryPath(temp.path, "nested/forward/slashes").native().find(L'/') == std::wstring::npos);
#endif
    CHECK(result.reservedMB > 0 && budget->used() == result.reservedMB * RamPolicy::MiB);
#ifdef _WIN32
    {
        const auto chosen = std::find_if(result.manifest.summary.files.begin(), result.manifest.summary.files.end(),
            [](const auto &file) { return file.memoryContent && file.sizeBytes > 0; });
        CHECK(chosen != result.manifest.summary.files.end());
        auto blocked = *chosen;
        blocked.path = temp.path / "occupied-materialization-target";
        fs::create_directory(blocked.path);
        auto memory = std::make_shared<MemoryContent>(*blocked.memoryContent);
        memory->attributes |= FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE;
        blocked.memoryContent = memory;
        const auto reservation = budget->used();
        bool failed = false;
        try { RamInput::materialize(blocked); } catch (const std::exception &) { failed = true; }
        CHECK(failed && fs::is_directory(blocked.path));
        for (const auto &entry : fs::directory_iterator(temp.path))
            CHECK(pathUtf8(entry.path().filename()).rfind("ram-spill-", 0) != 0);
        CHECK(budget->used() == reservation && reservation > 0);
        Hasher hash; hash.update(memory->data, static_cast<size_t>(memory->size));
        CHECK(hash.finish() == chosen->contentChecksum);
        CHECK(!fs::exists(chosen->path));
        std::cout << "[PASS] Failed RAM materialization removes staged readonly file and retains source\n";
    }
#endif
    unsigned ram = 0, disk = 0;
    for (const auto &file : result.manifest.summary.files) {
        if (file.memoryContent) {
            ++ram; CHECK(!fs::exists(file.path));
            Hasher hash; hash.update(file.memoryContent->data, static_cast<size_t>(file.sizeBytes));
            CHECK(hash.finish() == file.contentChecksum);
#ifdef _WIN32
            RamInput::materialize(file);
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            CHECK(GetFileAttributesExW(file.path.c_str(), GetFileExInfoStandard, &attributes));
            const auto mtime = uint64_t(attributes.ftLastWriteTime.dwLowDateTime) |
                               (uint64_t(attributes.ftLastWriteTime.dwHighDateTime) << 32);
            CHECK(mtime == file.memoryContent->mtime);
            constexpr DWORD preserved = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
                                        FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
            CHECK((attributes.dwFileAttributes & preserved) == (file.memoryContent->attributes & preserved));
            CHECK(fileChecksum(file.path) == file.contentChecksum);
#endif
        } else { ++disk; CHECK(fs::is_regular_file(file.path)); CHECK(fileChecksum(file.path) == file.contentChecksum); }
    }
    CHECK(ram >= 9 && disk == 1);
    CHECK(fs::is_directory(temp.path / "hybrid" / "empty" / "leaf"));
#ifdef _WIN32
    CHECK(InputManifest::scan(temp.path / "hybrid").fingerprint == original.fingerprint);
    const auto roundtrip = temp.path / "roundtrip.7z";
    CHECK(ProcessManager::execute(pathUtf8(tool),
        {"a", "-y", "-t7z", "-mx=1", pathUtf8(roundtrip), "."}, temp.path / "hybrid").success);
    const auto verifiedRoot = temp.path / "roundtrip-verified";
    CHECK(ProcessManager::execute(pathUtf8(tool),
        {"x", "-y", "-o" + pathUtf8(verifiedRoot), pathUtf8(roundtrip)}, temp.path).success);
    CHECK(InputManifest::scan(verifiedRoot).fingerprint == original.fingerprint);
    std::cout << "[PASS] Extensionless, dotfile, Unicode and long-path inputs retain full manifest/SHA in Disk and RAM roundtrip\n";
#endif
    result.manifest.summary.files.clear(); CHECK(budget->used() == 0);
    // Zero-byte content still owns an allocated mapping and must reserve its capacity.
    const auto emptySource = temp.path / "zero-source", emptyArchive = temp.path / "zero.7z";
    write(emptySource / "empty.txt", "");
    CHECK(ProcessManager::execute(pathUtf8(tool),
        {"a", "-y", "-t7z", pathUtf8(emptyArchive), "."}, emptySource).success);
    auto empty = RamInput::extract(emptyArchive, temp.path / "zero-hybrid", s, budget, &guard, nullptr);
    CHECK(empty.success && empty.ramBytes == 0 && empty.diskBytes == 0);
    CHECK(empty.manifest.summary.files.size() == 1 && empty.manifest.summary.files.front().memoryContent);
    CHECK(empty.reservedMB > 0 && budget->used() == empty.reservedMB * RamPolicy::MiB);
    CHECK(!fs::exists(empty.manifest.summary.files.front().path));
    empty.manifest.summary.files.clear(); CHECK(budget->used() == 0);
    auto low = s; low.ramLimitMB = 256; low.workerThreads = 8;
    validateSettings(low);
    auto headroom = RamInput::extract(archive, temp.path / "low-budget", low, budget, &guard, nullptr);
    CHECK(!headroom.success && !headroom.fallbackReason.empty() && budget->used() == 0);
    auto quota = s; quota.extractionFileLimitMB = 1;
    auto rejected = RamInput::extract(archive, temp.path / "quota", quota, budget, &guard, nullptr);
    CHECK(!rejected.success && rejected.fallbackReason.empty() && budget->used() == 0);
    const auto corrupt = temp.path / "corrupt.7z";
    write(corrupt, std::string("7z\xbc\xaf\x27\x1c", 6) + std::string(128, '\0'));
    auto malformed = RamInput::extract(corrupt, temp.path / "malformed", s, budget, &guard, nullptr);
    CHECK(!malformed.success && malformed.fallbackReason.empty() && budget->used() == 0);
    const auto zip = temp.path / "input.zip";
    CHECK(ProcessManager::execute(pathUtf8(tool), {"a", "-y", "-tzip", pathUtf8(zip), "."}, source).success);
    auto unsupported = RamInput::extract(zip, temp.path / "zip", s, budget, &guard, nullptr);
    CHECK(!unsupported.success && !unsupported.fallbackReason.empty() && budget->used() == 0);
    std::atomic<bool> cancel{true}; bool cancelled = false;
    try {
        auto stopped = RamInput::extract(archive, temp.path / "cancel", s, budget, &guard, &cancel);
        cancelled = !stopped.success && stopped.fallbackReason.empty();
    } catch (const std::exception &) { cancelled = true; }
    CHECK(cancelled && budget->used() == 0);
}
int main(int argc, char **argv) {
    (void)argc;
#ifndef _WIN32
    (void)argv; return 77;
#else
    try { auto tool = tools(argv[0]); CHECK(!tool.empty()); contracts(tool);
        std::cout << "RAM input contracts passed\n"; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
#endif
}

#include "EmbeddedTools.hpp"
#include "FileUtils.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <shlobj.h>
#include <windows.h>
#endif

namespace UltraArch {

std::filesystem::path EmbeddedTools::s_cachedToolsDir;
std::once_flag EmbeddedTools::s_initFlag;

std::filesystem::path EmbeddedTools::getDataDirectory() {
#ifdef _WIN32
    PWSTR folder = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &folder)))
        throw std::runtime_error("Cannot locate LocalAppData");
    std::filesystem::path result = std::filesystem::path(folder) / "UltraArchStudio";
    CoTaskMemFree(folder);
#else
    std::filesystem::path result = std::filesystem::temp_directory_path() / "UltraArchStudio";
#endif
    std::filesystem::create_directories(result);
    return result;
}

void EmbeddedTools::verifyToolManifest(const std::filesystem::path &directory) {
    auto manifest = directory / "tool-manifest.json";
    if (!std::filesystem::is_regular_file(manifest))
        return; // Explicit custom toolsets may use their own provenance.
    if (std::filesystem::file_size(manifest) > 1048576)
        throw std::runtime_error("Tool manifest too large");
    nlohmann::json json;
    std::ifstream in(manifest);
    in >> json;
    if (json.at("schema") != 1)
        throw std::runtime_error("Unsupported tool manifest");
    for (const auto &entry : json.at("tools")) {
        auto name = utf8Path(entry.at("file").get<std::string>());
        if (name.filename() != name || name == "." || name == ".." ||
            !std::filesystem::is_regular_file(directory / name) ||
            fileChecksum(directory / name) != entry.at("sha256").get<std::string>())
            throw std::runtime_error("Bundled tool identity mismatch: " + pathUtf8(name));
    }
}

std::filesystem::path EmbeddedTools::getToolsDirectory() {
    ensureToolsReady();
    return s_cachedToolsDir;
}

void EmbeddedTools::initOnce() {
    std::error_code ec;

    // Prefer tools installed alongside the executable; do not execute tools from an arbitrary CWD.
    std::vector<std::filesystem::path> localCandidates;
#ifdef _WIN32
    wchar_t exePath[32768] = {0};
    if (GetModuleFileNameW(NULL, exePath, 32768) > 0) {
        std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
        localCandidates.push_back(exeDir / "bin");
        localCandidates.push_back(exeDir);
    }
#endif
#ifndef _WIN32
    localCandidates.push_back(std::filesystem::current_path() / "bin");
#endif

    for (const auto &dir : localCandidates) {
#ifdef _WIN32
        std::filesystem::path p7z = dir / "7za.exe";
        if (!std::filesystem::exists(p7z, ec))
            p7z = dir / "7z.exe";
#else
        std::filesystem::path p7z = dir / "7za";
        if (!std::filesystem::exists(p7z, ec))
            p7z = dir / "7zz";
        if (!std::filesystem::exists(p7z, ec))
            p7z = dir / "7z";
#endif
        if (std::filesystem::exists(p7z, ec)) {
            verifyToolManifest(dir);
            s_cachedToolsDir = dir;
            return;
        }
    }

    // An empty explicit directory permits the process resolver to search PATH.
    s_cachedToolsDir.clear();
}

std::filesystem::path EmbeddedTools::ensureToolsReady() {
    std::call_once(s_initFlag, initOnce);
    return s_cachedToolsDir;
}

} // namespace UltraArch
